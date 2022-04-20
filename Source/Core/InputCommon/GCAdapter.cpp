// Copyright 2014 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "InputCommon/GCAdapter.h"

#ifndef ANDROID
#define GCADAPTER_USE_LIBUSB_IMPLEMENTATION true
#define GCADAPTER_USE_ANDROID_IMPLEMENTATION false
#else
#define GCADAPTER_USE_LIBUSB_IMPLEMENTATION false
#define GCADAPTER_USE_ANDROID_IMPLEMENTATION true
#endif

#include <algorithm>
#include <array>
#include <mutex>
#include <optional>

#if GCADAPTER_USE_LIBUSB_IMPLEMENTATION
#include <libusb.h>
#elif GCADAPTER_USE_ANDROID_IMPLEMENTATION
#include <jni.h>
#endif

#include "Common/Event.h"
#include "Common/Flag.h"
#include "Common/Logging/Log.h"
#include "Common/Thread.h"
#include "Core/Config/MainSettings.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "Core/HW/SI/SI.h"
#include "Core/HW/SI/SI_Device.h"
#include "Core/HW/SystemTimers.h"
#include "InputCommon/GCPadStatus.h"

#if GCADAPTER_USE_LIBUSB_IMPLEMENTATION
#include "Common/ScopeGuard.h"
#include "Core/LibusbUtils.h"
#elif GCADAPTER_USE_ANDROID_IMPLEMENTATION
#include "jni/AndroidCommon/IDCache.h"
#endif

#if GCADAPTER_USE_LIBUSB_IMPLEMENTATION
#if defined(LIBUSB_API_VERSION)
#define LIBUSB_API_VERSION_EXIST 1
#else
#define LIBUSB_API_VERSION_EXIST 0
#endif

#define LIBUSB_API_VERSION_ATLEAST(v) (LIBUSB_API_VERSION_EXIST && LIBUSB_API_VERSION >= (v))
#define LIBUSB_API_HAS_HOTPLUG LIBUSB_API_VERSION_ATLEAST(0x01000102)
#endif

namespace GCAdapter
{
#if GCADAPTER_USE_LIBUSB_IMPLEMENTATION
static bool CheckDeviceAccess(libusb_device* device);
static void AddGCAdapter(libusb_device* device);
static void ResetRumbleLockNeeded();
#endif
static void Reset();
static void Setup();

#if GCADAPTER_USE_LIBUSB_IMPLEMENTATION
enum
{
  NO_ADAPTER_DETECTED = 0,
  ADAPTER_DETECTED = 1,
};

// Current adapter status: detected/not detected/in error (holds the error code)
static std::atomic<int> s_status = NO_ADAPTER_DETECTED;
static libusb_device_handle* s_handle = nullptr;
#elif GCADAPTER_USE_ANDROID_IMPLEMENTATION
// Java classes
static jclass s_adapter_class;

static bool s_detected = false;
static int s_fd = 0;
#endif

enum class ControllerType : u8
{
  None = 0,
  Wired = 1,
  Wireless = 2,
};

static std::array<ControllerType, SerialInterface::MAX_SI_CHANNELS> s_controller_type = {
    ControllerType::None, ControllerType::None, ControllerType::None, ControllerType::None};
static std::array<u8, SerialInterface::MAX_SI_CHANNELS> s_controller_rumble{};

constexpr size_t CONTROLER_INPUT_PAYLOAD_EXPECTED_SIZE = 37;
constexpr size_t CONTROLER_OUTPUT_INIT_PAYLOAD_SIZE = 1;
constexpr size_t CONTROLER_OUTPUT_RUMBLE_PAYLOAD_SIZE = 5;

#if GCADAPTER_USE_LIBUSB_IMPLEMENTATION
static std::mutex s_mutex;
static std::array<u8, CONTROLER_INPUT_PAYLOAD_EXPECTED_SIZE> s_controller_payload;
static std::array<u8, CONTROLER_INPUT_PAYLOAD_EXPECTED_SIZE> s_controller_payload_swap;

// Only access with s_mutex held!
static int s_controller_payload_size = {0};

static std::thread s_adapter_input_thread;
static std::thread s_adapter_output_thread;
static Common::Flag s_adapter_thread_running;

static Common::Event s_rumble_data_available;

static std::mutex s_init_mutex;
#elif GCADAPTER_USE_ANDROID_IMPLEMENTATION
// Input handling
static std::mutex s_read_mutex;
static std::array<u8, CONTROLER_INPUT_PAYLOAD_EXPECTED_SIZE> s_controller_payload;
static int s_controller_payload_size = {0};

// Output handling
static std::mutex s_write_mutex;
static std::array<u8, CONTROLER_OUTPUT_RUMBLE_PAYLOAD_SIZE> s_controller_write_payload;
static std::atomic<int> s_controller_write_payload_size{0};

// Adapter running thread
static std::thread s_read_adapter_thread;
static Common::Flag s_read_adapter_thread_running;

static Common::Flag s_write_adapter_thread_running;
static Common::Event s_write_happened;
#endif

static std::thread s_adapter_detect_thread;
static Common::Flag s_adapter_detect_thread_running;

#if GCADAPTER_USE_LIBUSB_IMPLEMENTATION
static Common::Event s_hotplug_event;

static std::function<void(void)> s_detect_callback;

#if defined(__FreeBSD__) && __FreeBSD__ >= 11
static bool s_libusb_hotplug_enabled = true;
#else
static bool s_libusb_hotplug_enabled = false;
#endif
#if LIBUSB_API_HAS_HOTPLUG
static libusb_hotplug_callback_handle s_hotplug_handle;
#endif

static std::unique_ptr<LibusbUtils::Context> s_libusb_context;

static u8 s_endpoint_in = 0;
static u8 s_endpoint_out = 0;
#endif

static u64 s_last_init = 0;

static std::optional<size_t> s_config_callback_id = std::nullopt;
static std::array<SerialInterface::SIDevices, SerialInterface::MAX_SI_CHANNELS>
    s_config_si_device_type{};
static std::array<bool, SerialInterface::MAX_SI_CHANNELS> s_config_rumble_enabled{};

static void Read()
{
  Common::SetCurrentThreadName("GCAdapter Read Thread");

#if GCADAPTER_USE_LIBUSB_IMPLEMENTATION
  int payload_size = 0;
  while (s_adapter_thread_running.IsSet())
  {
    int err = libusb_interrupt_transfer(s_handle, s_endpoint_in, s_controller_payload_swap.data(),
                                        CONTROLER_INPUT_PAYLOAD_EXPECTED_SIZE, &payload_size, 16);
    if (err)
      ERROR_LOG_FMT(CONTROLLERINTERFACE, "adapter libusb read failed: err={}",
                    libusb_error_name(err));

    {
      std::lock_guard<std::mutex> lk(s_mutex);
      std::swap(s_controller_payload_swap, s_controller_payload);
      s_controller_payload_size = payload_size;
    }

    Common::YieldCPU();
  }
#elif GCADAPTER_USE_ANDROID_IMPLEMENTATION
  NOTICE_LOG_FMT(CONTROLLERINTERFACE, "GC Adapter read thread started");

  bool first_read = true;
  JNIEnv* env = IDCache::GetEnvForThread();

  jfieldID payload_field = env->GetStaticFieldID(s_adapter_class, "controller_payload", "[B");
  jobject payload_object = env->GetStaticObjectField(s_adapter_class, payload_field);
  auto* java_controller_payload = reinterpret_cast<jbyteArray*>(&payload_object);

  // Get function pointers
  jmethodID getfd_func = env->GetStaticMethodID(s_adapter_class, "GetFD", "()I");
  jmethodID input_func = env->GetStaticMethodID(s_adapter_class, "Input", "()I");
  jmethodID openadapter_func = env->GetStaticMethodID(s_adapter_class, "OpenAdapter", "()Z");

  bool connected = env->CallStaticBooleanMethod(s_adapter_class, openadapter_func);

  if (connected)
  {
    s_write_adapter_thread_running.Set(true);
    std::thread write_adapter_thread(Write);

    // Reset rumble once on initial reading
    ResetRumble();

    while (s_read_adapter_thread_running.IsSet())
    {
      int read_size = env->CallStaticIntMethod(s_adapter_class, input_func);

      jbyte* java_data = env->GetByteArrayElements(*java_controller_payload, nullptr);
      {
        std::lock_guard<std::mutex> lk(s_read_mutex);
        std::copy(java_data, java_data + CONTROLER_INPUT_PAYLOAD_EXPECTED_SIZE,
                  s_controller_payload.begin());
        s_controller_payload_size = read_size;
      }
      env->ReleaseByteArrayElements(*java_controller_payload, java_data, 0);

      if (first_read)
      {
        first_read = false;
        s_fd = env->CallStaticIntMethod(s_adapter_class, getfd_func);
      }

      Common::YieldCPU();
    }

    // Terminate the write thread on leaving
    if (s_write_adapter_thread_running.TestAndClear())
    {
      s_controller_write_payload_size.store(0);
      s_write_happened.Set();  // Kick the waiting event
      write_adapter_thread.join();
    }
  }

  s_fd = 0;
  s_detected = false;

  NOTICE_LOG_FMT(CONTROLLERINTERFACE, "GC Adapter read thread stopped");
#endif
}

static void Write()
{
  Common::SetCurrentThreadName("GCAdapter Write Thread");

#if GCADAPTER_USE_LIBUSB_IMPLEMENTATION
  int size = 0;

  while (true)
  {
    s_rumble_data_available.Wait();

    if (!s_adapter_thread_running.IsSet())
      return;

    std::array<u8, CONTROLER_OUTPUT_RUMBLE_PAYLOAD_SIZE> payload = {
        0x11,
        s_controller_rumble[0],
        s_controller_rumble[1],
        s_controller_rumble[2],
        s_controller_rumble[3],
    };
    const int err = libusb_interrupt_transfer(s_handle, s_endpoint_out, payload.data(),
                                              CONTROLER_OUTPUT_RUMBLE_PAYLOAD_SIZE, &size, 16);
    if (err != 0)
      ERROR_LOG_FMT(CONTROLLERINTERFACE, "adapter libusb write failed: err={}",
                    libusb_error_name(err));
  }
#elif GCADAPTER_USE_ANDROID_IMPLEMENTATION
  NOTICE_LOG_FMT(CONTROLLERINTERFACE, "GC Adapter write thread started");

  JNIEnv* env = IDCache::GetEnvForThread();
  jmethodID output_func = env->GetStaticMethodID(s_adapter_class, "Output", "([B)I");

  while (s_write_adapter_thread_running.IsSet())
  {
    s_write_happened.Wait();
    int write_size = s_controller_write_payload_size.load();
    if (write_size)
    {
      jbyteArray jrumble_array = env->NewByteArray(CONTROLER_OUTPUT_RUMBLE_PAYLOAD_SIZE);
      jbyte* jrumble = env->GetByteArrayElements(jrumble_array, nullptr);

      {
        std::lock_guard<std::mutex> lk(s_write_mutex);
        memcpy(jrumble, s_controller_write_payload.data(), write_size);
      }

      env->ReleaseByteArrayElements(jrumble_array, jrumble, 0);
      int size = env->CallStaticIntMethod(s_adapter_class, output_func, jrumble_array);
      // Netplay sends invalid data which results in size = 0x00.  Ignore it.
      if (size != write_size && size != 0x00)
      {
        ERROR_LOG_FMT(CONTROLLERINTERFACE, "error writing rumble (size: {})", size);
        Reset();
      }
    }

    Common::YieldCPU();
  }

  NOTICE_LOG_FMT(CONTROLLERINTERFACE, "GC Adapter write thread stopped");
#endif
}

#if GCADAPTER_USE_LIBUSB_IMPLEMENTATION
#if LIBUSB_API_HAS_HOTPLUG
static int HotplugCallback(libusb_context* ctx, libusb_device* dev, libusb_hotplug_event event,
                           void* user_data)
{
  if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED)
  {
    if (s_handle == nullptr)
      s_hotplug_event.Set();
  }
  else if (event == LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT)
  {
    if (s_handle != nullptr && libusb_get_device(s_handle) == dev)
      Reset();

    // Reset a potential error status now that the adapter is unplugged
    if (s_status < 0)
    {
      s_status = NO_ADAPTER_DETECTED;
      if (s_detect_callback != nullptr)
        s_detect_callback();
    }
  }
  return 0;
}
#endif
#endif

static void ScanThreadFunc()
{
  Common::SetCurrentThreadName("GC Adapter Scanning Thread");
  NOTICE_LOG_FMT(CONTROLLERINTERFACE, "GC Adapter scanning thread started");

#if GCADAPTER_USE_LIBUSB_IMPLEMENTATION
#if LIBUSB_API_HAS_HOTPLUG
#ifndef __FreeBSD__
  s_libusb_hotplug_enabled = libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG) != 0;
#endif
  if (s_libusb_hotplug_enabled)
  {
    if (libusb_hotplug_register_callback(
            *s_libusb_context,
            (libusb_hotplug_event)(LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED |
                                   LIBUSB_HOTPLUG_EVENT_DEVICE_LEFT),
            LIBUSB_HOTPLUG_ENUMERATE, 0x057e, 0x0337, LIBUSB_HOTPLUG_MATCH_ANY, HotplugCallback,
            nullptr, &s_hotplug_handle) != LIBUSB_SUCCESS)
      s_libusb_hotplug_enabled = false;
    if (s_libusb_hotplug_enabled)
      NOTICE_LOG_FMT(CONTROLLERINTERFACE, "Using libUSB hotplug detection");
  }
#endif

  while (s_adapter_detect_thread_running.IsSet())
  {
    if (s_handle == nullptr)
    {
      std::lock_guard<std::mutex> lk(s_init_mutex);
      Setup();
    }

    if (s_libusb_hotplug_enabled)
      s_hotplug_event.Wait();
    else
      Common::SleepCurrentThread(500);
  }
#elif GCADAPTER_USE_ANDROID_IMPLEMENTATION
  JNIEnv* env = IDCache::GetEnvForThread();

  jmethodID queryadapter_func = env->GetStaticMethodID(s_adapter_class, "QueryAdapter", "()Z");

  while (s_adapter_detect_thread_running.IsSet())
  {
    if (!s_detected && UseAdapter() &&
        env->CallStaticBooleanMethod(s_adapter_class, queryadapter_func))
      Setup();
    Common::SleepCurrentThread(1000);
  }
#endif

  NOTICE_LOG_FMT(CONTROLLERINTERFACE, "GC Adapter scanning thread stopped");
}

void SetAdapterCallback(std::function<void(void)> func)
{
#if GCADAPTER_USE_LIBUSB_IMPLEMENTATION
  s_detect_callback = func;
#endif
}

static void RefreshConfig()
{
  for (int i = 0; i < SerialInterface::MAX_SI_CHANNELS; ++i)
  {
    s_config_si_device_type[i] = Config::Get(Config::GetInfoForSIDevice(i));
    s_config_rumble_enabled[i] = Config::Get(Config::GetInfoForAdapterRumble(i));
  }
}

void Init()
{
#if GCADAPTER_USE_LIBUSB_IMPLEMENTATION
  if (s_handle != nullptr)
    return;

  s_libusb_context = std::make_unique<LibusbUtils::Context>();
#elif GCADAPTER_USE_ANDROID_IMPLEMENTATION
  if (s_fd)
    return;
#endif

  if (Core::GetState() != Core::State::Uninitialized && Core::GetState() != Core::State::Starting)
  {
    if ((CoreTiming::GetTicks() - s_last_init) < SystemTimers::GetTicksPerSecond())
      return;

    s_last_init = CoreTiming::GetTicks();
  }

#if GCADAPTER_USE_LIBUSB_IMPLEMENTATION
  s_status = NO_ADAPTER_DETECTED;
#elif GCADAPTER_USE_ANDROID_IMPLEMENTATION
  JNIEnv* env = IDCache::GetEnvForThread();

  jclass adapter_class = env->FindClass("org/dolphinemu/dolphinemu/utils/Java_GCAdapter");
  s_adapter_class = reinterpret_cast<jclass>(env->NewGlobalRef(adapter_class));
#endif

  if (!s_config_callback_id)
    s_config_callback_id = Config::AddConfigChangedCallback(RefreshConfig);
  RefreshConfig();

  if (UseAdapter())
    StartScanThread();
}

void StartScanThread()
{
  if (s_adapter_detect_thread_running.IsSet())
    return;
#if GCADAPTER_USE_LIBUSB_IMPLEMENTATION
  if (!s_libusb_context->IsValid())
    return;
#endif
  s_adapter_detect_thread_running.Set(true);
  s_adapter_detect_thread = std::thread(ScanThreadFunc);
}

void StopScanThread()
{
  if (s_adapter_detect_thread_running.TestAndClear())
  {
#if GCADAPTER_USE_LIBUSB_IMPLEMENTATION
    s_hotplug_event.Set();
#endif
    s_adapter_detect_thread.join();
  }
}

static void Setup()
{
#if GCADAPTER_USE_LIBUSB_IMPLEMENTATION
  int prev_status = s_status;

  // Reset the error status in case the adapter gets unplugged
  if (s_status < 0)
    s_status = NO_ADAPTER_DETECTED;

  s_controller_type.fill(ControllerType::None);
  s_controller_rumble.fill(0);

  s_libusb_context->GetDeviceList([](libusb_device* device) {
    if (CheckDeviceAccess(device))
    {
      // Only connect to a single adapter in case the user has multiple connected
      AddGCAdapter(device);
      return false;
    }
    return true;
  });

  if (s_status != ADAPTER_DETECTED && prev_status != s_status && s_detect_callback != nullptr)
    s_detect_callback();
#elif GCADAPTER_USE_ANDROID_IMPLEMENTATION
  s_fd = 0;
  s_detected = true;

  // Make sure the thread isn't in the middle of shutting down while starting a new one
  if (s_read_adapter_thread_running.TestAndClear())
    s_read_adapter_thread.join();

  s_read_adapter_thread_running.Set(true);
  s_read_adapter_thread = std::thread(Read);
#endif
}

#if GCADAPTER_USE_LIBUSB_IMPLEMENTATION
static bool CheckDeviceAccess(libusb_device* device)
{
  libusb_device_descriptor desc;
  int ret = libusb_get_device_descriptor(device, &desc);
  if (ret != 0)
  {
    // could not acquire the descriptor, no point in trying to use it.
    ERROR_LOG_FMT(CONTROLLERINTERFACE, "libusb_get_device_descriptor failed with error: {}", ret);
    return false;
  }

  if (desc.idVendor != 0x057e || desc.idProduct != 0x0337)
  {
    // This isn’t the device we are looking for.
    return false;
  }

  NOTICE_LOG_FMT(CONTROLLERINTERFACE, "Found GC Adapter with Vendor: {:X} Product: {:X} Devnum: {}",
                 desc.idVendor, desc.idProduct, 1);

  // In case of failure, capture the libusb error code into the adapter status
  Common::ScopeGuard status_guard([&ret] { s_status = ret; });

  const u8 bus = libusb_get_bus_number(device);
  const u8 port = libusb_get_device_address(device);
  ret = libusb_open(device, &s_handle);
  if (ret == LIBUSB_ERROR_ACCESS)
  {
    ERROR_LOG_FMT(
        CONTROLLERINTERFACE,
        "Dolphin does not have access to this device: Bus {:03d} Device {:03d}: ID {:04X}:{:04X}.",
        bus, port, desc.idVendor, desc.idProduct);
    return false;
  }
  if (ret != 0)
  {
    ERROR_LOG_FMT(CONTROLLERINTERFACE, "libusb_open failed to open device with error = {}", ret);
    return false;
  }

  bool detach_failed = false;
  ret = libusb_kernel_driver_active(s_handle, 0);
  if (ret == 1)
  {
    // On macos detaching would fail without root or entitlement.
    // We assume user is using GCAdapterDriver and therefor don't want to detach anything
#if !defined(__APPLE__)
    ret = libusb_detach_kernel_driver(s_handle, 0);
    detach_failed = ret < 0 && ret != LIBUSB_ERROR_NOT_FOUND && ret != LIBUSB_ERROR_NOT_SUPPORTED;
#endif
    if (detach_failed)
      ERROR_LOG_FMT(CONTROLLERINTERFACE, "libusb_detach_kernel_driver failed with error: {}", ret);
  }

  // This call makes Nyko-brand (and perhaps other) adapters work.
  // However it returns LIBUSB_ERROR_PIPE with Mayflash adapters.
  const int transfer = libusb_control_transfer(s_handle, 0x21, 11, 0x0001, 0, nullptr, 0, 1000);
  if (transfer < 0)
    WARN_LOG_FMT(CONTROLLERINTERFACE, "libusb_control_transfer failed with error: {}", transfer);

  // this split is needed so that we don't avoid claiming the interface when
  // detaching the kernel driver is successful
  if (detach_failed)
  {
    libusb_close(s_handle);
    s_handle = nullptr;
    return false;
  }

  ret = libusb_claim_interface(s_handle, 0);
  if (ret != 0)
  {
    ERROR_LOG_FMT(CONTROLLERINTERFACE, "libusb_claim_interface failed with error: {}", ret);
    libusb_close(s_handle);
    s_handle = nullptr;
    return false;
  }

  // Updating the adapter status will be done in AddGCAdapter
  status_guard.Dismiss();

  return true;
}

static void AddGCAdapter(libusb_device* device)
{
  libusb_config_descriptor* config = nullptr;
  libusb_get_config_descriptor(device, 0, &config);
  for (u8 ic = 0; ic < config->bNumInterfaces; ic++)
  {
    const libusb_interface* interfaceContainer = &config->interface[ic];
    for (int i = 0; i < interfaceContainer->num_altsetting; i++)
    {
      const libusb_interface_descriptor* interface = &interfaceContainer->altsetting[i];
      for (u8 e = 0; e < interface->bNumEndpoints; e++)
      {
        const libusb_endpoint_descriptor* endpoint = &interface->endpoint[e];
        if (endpoint->bEndpointAddress & LIBUSB_ENDPOINT_IN)
          s_endpoint_in = endpoint->bEndpointAddress;
        else
          s_endpoint_out = endpoint->bEndpointAddress;
      }
    }
  }

  int size = 0;
  std::array<u8, CONTROLER_OUTPUT_INIT_PAYLOAD_SIZE> payload = {0x13};
  libusb_interrupt_transfer(s_handle, s_endpoint_out, payload.data(),
                            CONTROLER_OUTPUT_INIT_PAYLOAD_SIZE, &size, 16);

  s_adapter_thread_running.Set(true);
  s_adapter_input_thread = std::thread(Read);
  s_adapter_output_thread = std::thread(Write);

  s_status = ADAPTER_DETECTED;
  if (s_detect_callback != nullptr)
    s_detect_callback();
  ResetRumbleLockNeeded();
}
#endif

void Shutdown()
{
  StopScanThread();
#if GCADAPTER_USE_LIBUSB_IMPLEMENTATION
#if LIBUSB_API_HAS_HOTPLUG
  if (s_libusb_context->IsValid() && s_libusb_hotplug_enabled)
    libusb_hotplug_deregister_callback(*s_libusb_context, s_hotplug_handle);
#endif
#endif
  Reset();

#if GCADAPTER_USE_LIBUSB_IMPLEMENTATION
  s_libusb_context.reset();
  s_status = NO_ADAPTER_DETECTED;
#endif

  if (s_config_callback_id)
  {
    Config::RemoveConfigChangedCallback(*s_config_callback_id);
    s_config_callback_id = std::nullopt;
  }
}

static void Reset()
{
#if GCADAPTER_USE_LIBUSB_IMPLEMENTATION
  std::unique_lock<std::mutex> lock(s_init_mutex, std::defer_lock);
  if (!lock.try_lock())
    return;
  if (s_status != ADAPTER_DETECTED)
    return;

  if (s_adapter_thread_running.TestAndClear())
  {
    s_rumble_data_available.Set();
    s_adapter_input_thread.join();
    s_adapter_output_thread.join();
  }
#elif GCADAPTER_USE_ANDROID_IMPLEMENTATION
  if (!s_detected)
    return;

  if (s_read_adapter_thread_running.TestAndClear())
    s_read_adapter_thread.join();
#endif

  s_controller_type.fill(ControllerType::None);

#if GCADAPTER_USE_LIBUSB_IMPLEMENTATION
  s_status = NO_ADAPTER_DETECTED;

  if (s_handle)
  {
    libusb_release_interface(s_handle, 0);
    libusb_close(s_handle);
    s_handle = nullptr;
  }
  if (s_detect_callback != nullptr)
    s_detect_callback();
#elif GCADAPTER_USE_ANDROID_IMPLEMENTATION
  s_detected = false;
  s_fd = 0;
#endif

  NOTICE_LOG_FMT(CONTROLLERINTERFACE, "GC Adapter detached");
}

GCPadStatus Input(int chan)
{
  if (!UseAdapter())
    return {};

#if GCADAPTER_USE_LIBUSB_IMPLEMENTATION
  if (s_handle == nullptr || s_status != ADAPTER_DETECTED)
    return {};
#elif GCADAPTER_USE_ANDROID_IMPLEMENTATION
  if (!s_detected || !s_fd)
    return {};
#endif

  int payload_size = 0;
  std::array<u8, CONTROLER_INPUT_PAYLOAD_EXPECTED_SIZE> controller_payload_copy{};

  {
#if GCADAPTER_USE_LIBUSB_IMPLEMENTATION
    std::lock_guard<std::mutex> lk(s_mutex);
#elif GCADAPTER_USE_ANDROID_IMPLEMENTATION
    std::lock_guard<std::mutex> lk(s_read_mutex);
#endif
    controller_payload_copy = s_controller_payload;
    payload_size = s_controller_payload_size;
  }

  GCPadStatus pad = {};
  if (payload_size != CONTROLER_INPUT_PAYLOAD_EXPECTED_SIZE
#if GCADAPTER_USE_LIBUSB_IMPLEMENTATION
      || controller_payload_copy[0] != LIBUSB_DT_HID
#endif
  )
  {
    // This can occur for a few frames on initialization.
    ERROR_LOG_FMT(CONTROLLERINTERFACE, "error reading payload (size: {}, type: {:02x})",
                  payload_size, controller_payload_copy[0]);
#if GCADAPTER_USE_ANDROID_IMPLEMENTATION
    Reset();
#endif
  }
  else
  {
    bool get_origin = false;
    // TODO: What do the other bits here indicate?  Does casting to an enum like this make sense?
    const auto type = static_cast<ControllerType>(controller_payload_copy[1 + (9 * chan)] >> 4);
    if (type != ControllerType::None && s_controller_type[chan] == ControllerType::None)
    {
      NOTICE_LOG_FMT(CONTROLLERINTERFACE, "New device connected to Port {} of Type: {:02x}",
                     chan + 1, controller_payload_copy[1 + (9 * chan)]);
      get_origin = true;
    }

    s_controller_type[chan] = type;

    if (s_controller_type[chan] != ControllerType::None)
    {
      u8 b1 = controller_payload_copy[1 + (9 * chan) + 1];
      u8 b2 = controller_payload_copy[1 + (9 * chan) + 2];

      if (b1 & (1 << 0))
        pad.button |= PAD_BUTTON_A;
      if (b1 & (1 << 1))
        pad.button |= PAD_BUTTON_B;
      if (b1 & (1 << 2))
        pad.button |= PAD_BUTTON_X;
      if (b1 & (1 << 3))
        pad.button |= PAD_BUTTON_Y;

      if (b1 & (1 << 4))
        pad.button |= PAD_BUTTON_LEFT;
      if (b1 & (1 << 5))
        pad.button |= PAD_BUTTON_RIGHT;
      if (b1 & (1 << 6))
        pad.button |= PAD_BUTTON_DOWN;
      if (b1 & (1 << 7))
        pad.button |= PAD_BUTTON_UP;

      if (b2 & (1 << 0))
        pad.button |= PAD_BUTTON_START;
      if (b2 & (1 << 1))
        pad.button |= PAD_TRIGGER_Z;
      if (b2 & (1 << 2))
        pad.button |= PAD_TRIGGER_R;
      if (b2 & (1 << 3))
        pad.button |= PAD_TRIGGER_L;

      if (get_origin)
        pad.button |= PAD_GET_ORIGIN;

      pad.stickX = controller_payload_copy[1 + (9 * chan) + 3];
      pad.stickY = controller_payload_copy[1 + (9 * chan) + 4];
      pad.substickX = controller_payload_copy[1 + (9 * chan) + 5];
      pad.substickY = controller_payload_copy[1 + (9 * chan) + 6];
      pad.triggerLeft = controller_payload_copy[1 + (9 * chan) + 7];
      pad.triggerRight = controller_payload_copy[1 + (9 * chan) + 8];
    }
    else if (!Core::WantsDeterminism())
    {
      // This is a hack to prevent a desync due to SI devices
      // being different and returning different values.
      // The corresponding code in DeviceGCAdapter has the same check
      pad.button = PAD_ERR_STATUS;
    }
  }

  return pad;
}

bool DeviceConnected(int chan)
{
  return s_controller_type[chan] != ControllerType::None;
}

void ResetDeviceType(int chan)
{
  s_controller_type[chan] = ControllerType::None;
}

bool UseAdapter()
{
  const auto& si_devices = s_config_si_device_type;
  return std::any_of(si_devices.begin(), si_devices.end(), [](const auto device_type) {
    return device_type == SerialInterface::SIDEVICE_WIIU_ADAPTER;
  });
}

void ResetRumble()
{
#if GCADAPTER_USE_LIBUSB_IMPLEMENTATION
  std::unique_lock<std::mutex> lock(s_init_mutex, std::defer_lock);
  if (!lock.try_lock())
    return;
  ResetRumbleLockNeeded();
#elif GCADAPTER_USE_ANDROID_IMPLEMENTATION
  std::array<u8, CONTROLER_OUTPUT_RUMBLE_PAYLOAD_SIZE> rumble = {0x11, 0, 0, 0, 0};
  {
    std::lock_guard<std::mutex> lk(s_write_mutex);
    s_controller_write_payload = rumble;
    s_controller_write_payload_size.store(CONTROLER_OUTPUT_RUMBLE_PAYLOAD_SIZE);
  }
  s_write_happened.Set();
#endif
}

#if GCADAPTER_USE_LIBUSB_IMPLEMENTATION
// Needs to be called when s_init_mutex is locked in order to avoid
// being called while the libusb state is being reset
static void ResetRumbleLockNeeded()
{
  if (!UseAdapter() || (s_handle == nullptr || s_status != ADAPTER_DETECTED))
  {
    return;
  }

  std::fill(std::begin(s_controller_rumble), std::end(s_controller_rumble), 0);

  std::array<u8, CONTROLER_OUTPUT_RUMBLE_PAYLOAD_SIZE> rumble = {
      0x11, s_controller_rumble[0], s_controller_rumble[1], s_controller_rumble[2],
      s_controller_rumble[3]};

  int size = 0;
  libusb_interrupt_transfer(s_handle, s_endpoint_out, rumble.data(),
                            CONTROLER_OUTPUT_RUMBLE_PAYLOAD_SIZE, &size, 16);

  INFO_LOG_FMT(CONTROLLERINTERFACE, "Rumble state reset");
}
#endif

void Output(int chan, u8 rumble_command)
{
  if (!UseAdapter() || !s_config_rumble_enabled[chan])
    return;

#if GCADAPTER_USE_LIBUSB_IMPLEMENTATION
  if (s_handle == nullptr)
    return;
#elif GCADAPTER_USE_ANDROID_IMPLEMENTATION
  if (!s_detected || !s_fd)
    return;
#endif

  // Skip over rumble commands if it has not changed or the controller is wireless
  if (rumble_command != s_controller_rumble[chan] &&
      s_controller_type[chan] != ControllerType::Wireless)
  {
    s_controller_rumble[chan] = rumble_command;
#if GCADAPTER_USE_LIBUSB_IMPLEMENTATION
    s_rumble_data_available.Set();
#elif GCADAPTER_USE_ANDROID_IMPLEMENTATION
    std::array<u8, CONTROLER_OUTPUT_RUMBLE_PAYLOAD_SIZE> rumble = {
        0x11, s_controller_rumble[0], s_controller_rumble[1], s_controller_rumble[2],
        s_controller_rumble[3]};
    {
      std::lock_guard<std::mutex> lk(s_write_mutex);
      s_controller_write_payload = rumble;
      s_controller_write_payload_size.store(CONTROLER_OUTPUT_RUMBLE_PAYLOAD_SIZE);
    }
    s_write_happened.Set();
#endif
  }
}

bool IsDetected(const char** error_message)
{
#if GCADAPTER_USE_LIBUSB_IMPLEMENTATION
  if (s_status >= 0)
  {
    if (error_message)
      *error_message = nullptr;

    return s_status == ADAPTER_DETECTED;
  }

  if (error_message)
    *error_message = libusb_strerror(static_cast<libusb_error>(s_status.load()));

  return false;
#elif GCADAPTER_USE_ANDROID_IMPLEMENTATION
  return s_detected;
#endif
}

}  // namespace GCAdapter
