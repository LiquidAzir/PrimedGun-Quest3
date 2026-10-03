// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

#include <openxr/openxr.h>

namespace VR::Recenter
{
// An upright application origin: remove heading and translation, retaining the
// user's pitch/roll and the separation between eyes when OpenXR locates poses.
inline std::optional<XrPosef> BuildOrigin(const XrPosef& left_eye, const XrPosef& right_eye)
{
  const auto finite_position = [](const XrVector3f& p) {
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z);
  };
  if (!finite_position(left_eye.position) || !finite_position(right_eye.position))
    return std::nullopt;

  XrQuaternionf q = left_eye.orientation;
  const float norm = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
  if (!std::isfinite(norm) || norm < 1e-6f)
    return std::nullopt;
  const float inverse_length = 1.0f / std::sqrt(norm);
  q.x *= inverse_length;
  q.y *= inverse_length;
  q.z *= inverse_length;
  q.w *= inverse_length;

  // OpenXR's forward axis is -Z. Project it onto the horizontal plane so
  // recentering while looking up/down does not tilt the game's horizon.
  const float forward_x = -2.0f * (q.x * q.z + q.w * q.y);
  const float forward_z = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
  float yaw;
  if (forward_x * forward_x + forward_z * forward_z > 1e-6f)
  {
    yaw = std::atan2(-forward_x, -forward_z);
  }
  else
  {
    // Looking almost vertically makes forward heading ambiguous. Use the
    // horizontal right axis instead; it remains defined at the poles.
    const float right_x = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
    const float right_z = 2.0f * (q.x * q.z - q.w * q.y);
    yaw = std::atan2(-right_z, right_x);
  }

  return XrPosef{{0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)},
                 {0.5f * left_eye.position.x + 0.5f * right_eye.position.x,
                  0.5f * left_eye.position.y + 0.5f * right_eye.position.y,
                  0.5f * left_eye.position.z + 0.5f * right_eye.position.z}};
}

// Retain future changes: receiving a pending event does not mean the runtime
// has changed the coordinates returned for this frame's predicted display time.
inline bool ConsumeDueChanges(std::vector<XrTime>& changes, XrTime display_time)
{
  return std::erase_if(changes, [display_time](XrTime change) { return change <= display_time; }) != 0;
}
}  // namespace VR::Recenter
