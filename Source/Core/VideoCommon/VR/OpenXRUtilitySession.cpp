// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef ENABLE_VR

#include "VideoCommon/VR/OpenXRUtilitySession.h"

#include <openxr/openxr.h>

#include "Common/Logging/Log.h"
#include "Common/VR/OpenXRInputState.h"
#include "VideoCommon/VR/OpenXRManager.h"

namespace VR
{
OpenXRUtilitySession::~OpenXRUtilitySession()
{
  Stop();
}

bool OpenXRUtilitySession::Start()
{
  if (m_running)
    return true;

  if (!OpenXRManager::IsRuntimeExtensionSupported(XR_MND_HEADLESS_EXTENSION_NAME))
  {
    WARN_LOG_FMT(VIDEO,
                 "OpenXR utility config session requires {}, but the active runtime does not "
                 "advertise it.",
                 XR_MND_HEADLESS_EXTENSION_NAME);
    return false;
  }

  m_manager = std::make_unique<OpenXRManager>();
  if (!m_manager->CreateInstance({XR_MND_HEADLESS_EXTENSION_NAME}) ||
      !m_manager->InitializeSystem())
  {
    m_manager.reset();
    return false;
  }

  XrSession session = XR_NULL_HANDLE;
  XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
  session_info.systemId = m_manager->GetSystemId();
  const XrResult result = xrCreateSession(m_manager->GetInstance(), &session_info, &session);
  if (XR_FAILED(result) || session == XR_NULL_HANDLE)
  {
    WARN_LOG_FMT(VIDEO, "OpenXR utility config session xrCreateSession failed ({}).",
                 static_cast<int>(result));
    m_manager.reset();
    return false;
  }

  m_manager->SetSession(session);
  if (!m_manager->CreateReferenceSpace())
  {
    m_manager.reset();
    return false;
  }

  m_running = true;
  PumpEventsAndInput();
  return true;
}

void OpenXRUtilitySession::Stop()
{
  if (m_frame_started && m_manager)
  {
    m_manager->EndFrame({});
    m_frame_started = false;
  }

  m_running = false;
  m_manager.reset();
  Common::VR::OpenXRInputState::Reset();
}

bool OpenXRUtilitySession::IsRunning() const
{
  return m_running;
}

void OpenXRUtilitySession::SetOverlayImage(const std::vector<u8>& rgba, int width, int height)
{
  (void)rgba;
  (void)width;
  (void)height;

  PumpEventsAndInput();
}

void OpenXRUtilitySession::PumpEventsAndInput()
{
  if (!m_running || !m_manager)
    return;

  if (!m_manager->PollEvents())
  {
    Stop();
    return;
  }

  if (!m_manager->IsSessionRunning())
    return;

  if (m_frame_started)
  {
    m_manager->EndFrame({});
    m_frame_started = false;
  }

  if (!m_manager->WaitFrame())
  {
    Stop();
    return;
  }

  if (!m_manager->BeginFrame())
  {
    Stop();
    return;
  }

  m_frame_started = true;
  m_manager->LocateViews();
  m_manager->EndFrame({});
  m_frame_started = false;
}
}  // namespace VR

#endif
