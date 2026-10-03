// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef ENABLE_VR

#include <memory>
#include <vector>

#include "Common/CommonTypes.h"

namespace VR
{
class OpenXRManager;

class OpenXRUtilitySession
{
public:
  OpenXRUtilitySession() = default;
  ~OpenXRUtilitySession();

  bool Start();
  void Stop();
  bool IsRunning() const;
  void SetOverlayImage(const std::vector<u8>& rgba, int width, int height);

private:
  void PumpEventsAndInput();

  std::unique_ptr<OpenXRManager> m_manager;
  bool m_running = false;
  bool m_frame_started = false;
};
}  // namespace VR

#endif
