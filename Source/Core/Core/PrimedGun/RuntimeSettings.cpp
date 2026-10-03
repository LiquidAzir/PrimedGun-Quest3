// Copyright 2026 PrimedGun Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PrimedGun/NativeRuntime.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <type_traits>

#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/Logging/Log.h"

namespace PrimedGun
{
namespace
{
std::string SettingsPath()
{
  return File::GetUserPath(D_CONFIG_IDX) + "PrimedGun.ini";
}

// Keep load/save coverage together. Like the desktop settings, tracking-origin
// offset_x/y/z are per-session calibration and must not survive a new playspace.
template <typename Visitor>
void VisitPersistentSettings(RuntimeSettings& settings, Visitor&& visit)
{
  const RuntimeSettings defaults;
#define SETTING(field) visit(#field, settings.field, defaults.field)
  SETTING(enabled);
  SETTING(builtin_patches_enabled);
  SETTING(patch_disable_frustum_culling);
  SETTING(patch_no_idle_sway);
  SETTING(patch_disable_arm_cannon_idle_fidget);
  SETTING(patch_beam_projectile_timing);
  SETTING(patch_xr_visor_dpad_timing);
  SETTING(patch_cannon_rotation);
  SETTING(patch_gun_ray_target);
  SETTING(patch_reticle);
  SETTING(use_right_hand);
  SETTING(model_offset_x);
  SETTING(model_offset_y);
  SETTING(model_offset_z);
  SETTING(rot_offset_x);
  SETTING(rot_offset_y);
  SETTING(rot_offset_z);
  SETTING(world_scale);
  SETTING(require_trigger);
  SETTING(trigger_threshold);
  SETTING(primegun_grip_inputs_enabled);
  SETTING(primegun_grip_inputs_use_trackpad);
  SETTING(primegun_trackpad_press_threshold);
  SETTING(gun_targeting_enabled);
  SETTING(gun_targeting_distance);
  SETTING(gun_targeting_radius);
  SETTING(visor_helmet_enabled);
  SETTING(vr_overlays_enabled);
  SETTING(xr_dpad_enabled);
  SETTING(xr_dpad_head_radius);
  SETTING(xr_dpad_head_y_below);
  SETTING(xr_dpad_deadzone);
  SETTING(directional_movement_enabled);
  SETTING(directional_movement_use_right_stick);
  SETTING(directional_movement_use_hmd_direction);
  SETTING(directional_movement_deadzone);
  SETTING(directional_movement_speed);
  SETTING(directional_movement_accel);
  SETTING(directional_movement_air_accel);
  SETTING(look_yaw_sensitivity);
#undef SETTING
}
}  // namespace

bool LoadRuntimeSettings()
{
  Common::IniFile ini;
  const std::string path = SettingsPath();
  if (File::Exists(path) && !ini.Load(path))
  {
    ERROR_LOG_FMT(CORE, "PrimedGun: could not load runtime settings from '{}'.", path);
    return false;
  }

  RuntimeSettings settings;
  const auto* section = ini.GetOrCreateSection("Runtime");
  VisitPersistentSettings(settings, [section](const char* key, auto& value, auto default_value) {
    section->Get(key, &value, default_value);
    if constexpr (std::is_floating_point_v<std::remove_reference_t<decltype(value)>>)
    {
      if (!std::isfinite(value))
        value = default_value;
    }
  });
  settings.world_scale = std::clamp(settings.world_scale, 0.1f, 10.0f);
  settings.trigger_threshold = std::clamp(settings.trigger_threshold, 0.0f, 1.0f);
  settings.gun_targeting_distance = std::clamp(settings.gun_targeting_distance, 1.0f, 200.0f);
  settings.gun_targeting_radius = std::clamp(settings.gun_targeting_radius, 0.1f, 25.0f);
  settings.xr_dpad_head_radius = std::clamp(settings.xr_dpad_head_radius, 0.05f, 0.60f);
  settings.xr_dpad_head_y_below = std::clamp(settings.xr_dpad_head_y_below, 0.0f, 0.60f);
  settings.xr_dpad_deadzone = std::clamp(settings.xr_dpad_deadzone, 0.0f, 0.95f);
  settings.directional_movement_deadzone =
      std::clamp(settings.directional_movement_deadzone, 0.0f, 0.95f);
  settings.directional_movement_speed =
      std::clamp(settings.directional_movement_speed, 1.0f, 60.0f);
  settings.directional_movement_accel =
      std::clamp(settings.directional_movement_accel, 1.0f, 120.0f);
  settings.directional_movement_air_accel =
      std::clamp(settings.directional_movement_air_accel, 1.0f, 120.0f);
  settings.look_yaw_sensitivity = std::clamp(settings.look_yaw_sensitivity, 0.20f, 3.0f);
  SetRuntimeSettings(settings);
  return true;
}

bool SaveRuntimeSettings()
{
  const std::string path = SettingsPath();
  Common::IniFile ini;
  ini.Load(path);
  auto* section = ini.GetOrCreateSection("Runtime");
  RuntimeSettings settings = GetRuntimeSettings();
  VisitPersistentSettings(
      settings, [section](const char* key, auto value, auto) { section->Set(key, value); });
  // Remove stale calibration from hand-edited or older files, too.
  section->Delete("offset_x");
  section->Delete("offset_y");
  section->Delete("offset_z");
  // IniFile::Save uses a temporary file and RenameSync for durable replacement.
  if (!File::CreateFullPath(path) || !ini.Save(path))
  {
    ERROR_LOG_FMT(CORE, "PrimedGun: could not save runtime settings to '{}'.", path);
    return false;
  }
  INFO_LOG_FMT(CORE, "PrimedGun: saved runtime settings to '{}'.", path);
  return true;
}
}  // namespace PrimedGun
