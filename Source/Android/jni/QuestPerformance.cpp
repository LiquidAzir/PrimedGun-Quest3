// SPDX-License-Identifier: GPL-2.0-or-later

#include <jni.h>
#include <string>

#include "Common/CommonPaths.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Core/Config/GraphicsSettings.h"
#include "Core/Core.h"
#include "Core/System.h"
#include "jni/Host.h"

namespace
{
constexpr int GRAPHICS_PRESET_VERSION = 7;

bool ApplyGraphicsPreset(int preset, bool only_if_not_migrated)
{
  // Profiles are a launcher action. Never rewrite a running game's configuration
  // or compete with the runtime's own PrimedGun.ini save operation.
  HostThreadLock guard;
  if (preset < 0 || preset > 2 || !Core::IsUninitialized(Core::System::GetInstance()))
    return false;

  const std::string path = File::GetUserPath(D_CONFIG_IDX) + "PrimedGun.ini";
  Common::IniFile ini;
  if (File::Exists(path) && !ini.Load(path))
    return false;
  auto* quest = ini.GetOrCreateSection("Quest");
  int applied_version = 0;
  quest->Get("GraphicsPresetVersion", &applied_version, 0);
  if (only_if_not_migrated && applied_version >= GRAPHICS_PRESET_VERSION)
    return true;

  quest->Set("RenderScale", 0.75f);
  quest->Set("RefreshRate", preset == 0 ? 72 : 120);
  // The optional coarse scene shader protects HUD/menus and falls back to full
  // shading when the device or a draw does not satisfy its feature requirements.
  quest->Set("SceneShadingRate", preset == 1 ? 2 : 1);
  // Both migration and explicit selections set this marker. Selecting a preset
  // before first launch must not be undone by launch-time default migration.
  quest->Set("GraphicsPresetVersion", GRAPHICS_PRESET_VERSION);
  if (!ini.Save(path))
    return false;

  Config::SetBase(Config::GFX_EFB_SCALE, preset == 0 ? 1 : 2);
  Config::SetBase(Config::GFX_MSAA, 1u);
  Config::SetBase(Config::GFX_SSAA, false);
  Config::SetBase(Config::GFX_VR_USE_VULKAN_MULTIVIEW, true);
  Config::SetBase(Config::GFX_WAIT_FOR_SHADERS_BEFORE_STARTING, true);
  Config::Save();
  return true;
}
}  // namespace

extern "C" JNIEXPORT jboolean JNICALL
Java_org_dolphinemu_dolphinemu_NativeLibrary_ApplyQuestGraphicsPreset(JNIEnv*, jclass, jint preset)
{
  return ApplyGraphicsPreset(preset, false) ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL
Java_org_dolphinemu_dolphinemu_NativeLibrary_EnsureQuestGraphicsPresetDefault(JNIEnv*, jclass,
                                                                         jint preset)
{
  return ApplyGraphicsPreset(preset, true) ? JNI_TRUE : JNI_FALSE;
}
