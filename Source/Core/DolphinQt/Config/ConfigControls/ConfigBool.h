// Copyright 2017 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "DolphinQt/Config/ConfigControls/ConfigControl.h"
#include "DolphinQt/Config/ToolTipControls/ToolTipCheckBox.h"

namespace Config
{
template <typename T>
class Info;
}

class ConfigBool final : public ConfigControl<ToolTipCheckBox>
{
  Q_OBJECT
public:
  ConfigBool(const QString& label, const Config::Info<bool>& setting, bool reverse = false);

protected:
  void OnConfigChanged() override;

private:
  void Update();

  const Config::Info<bool>& m_setting;
  bool m_reverse;
};
