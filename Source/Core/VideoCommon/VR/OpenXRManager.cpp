// Copyright 2024 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifdef ENABLE_VR

#include "VideoCommon/VR/OpenXRManager.h"

#if defined(ANDROID)
#ifndef XR_USE_PLATFORM_ANDROID
#define XR_USE_PLATFORM_ANDROID
#endif
#include <openxr/openxr_platform.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "Common/Logging/Log.h"
#include "Common/Matrix.h"
#if defined(ANDROID)
#include "Common/CommonPaths.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"
#include "Common/ScopeGuard.h"
#include "Common/Timer.h"
#include "Core/Host.h"
#include "VideoCommon/VR/OpenXRRecenter.h"
#endif
#include "Common/VR/OpenXRInputState.h"
#include "VideoCommon/VideoConfig.h"

namespace VR
{
std::unique_ptr<OpenXRManager> g_openxr;

namespace
{
constexpr float PRIMEGUN_DEFAULT_STANDING_HEIGHT_M = 1.2f;

#if defined(ANDROID)
std::mutex s_android_openxr_mutex;
JavaVM* s_android_vm = nullptr;
jobject s_android_activity = nullptr;
bool s_android_loader_initialized = false;
std::atomic<uint64_t> s_android_tracking_origin_generation{0};

bool EnsureAndroidOpenXRLoaderInitialized()
{
  std::lock_guard guard{s_android_openxr_mutex};
  if (s_android_loader_initialized)
    return true;

  if (!s_android_vm || !s_android_activity)
  {
    ERROR_LOG_FMT(OPENXR, "OpenXR: Android VM/activity missing before loader initialization.");
    return false;
  }

  PFN_xrInitializeLoaderKHR initialize_loader = nullptr;
  XrResult result = xrGetInstanceProcAddr(
      XR_NULL_HANDLE, "xrInitializeLoaderKHR",
      reinterpret_cast<PFN_xrVoidFunction*>(&initialize_loader));
  if (XR_FAILED(result) || !initialize_loader)
  {
    ERROR_LOG_FMT(OPENXR, "OpenXR: Could not load xrInitializeLoaderKHR ({}).",
                  static_cast<int>(result));
    return false;
  }

  XrLoaderInitInfoAndroidKHR loader_info{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
  loader_info.applicationVM = s_android_vm;
  // An Activity is a Context and lets the Quest runtime track this immersive activity.
  loader_info.applicationContext = s_android_activity;
  result = initialize_loader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loader_info));
  if (XR_FAILED(result))
  {
    ERROR_LOG_FMT(OPENXR, "OpenXR: xrInitializeLoaderKHR failed ({}).", static_cast<int>(result));
    return false;
  }

  s_android_loader_initialized = true;
  INFO_LOG_FMT(OPENXR, "OpenXR: Android loader initialized.");
  return true;
}
#endif

struct EulerDeg
{
  float yaw = 0.0f;
  float pitch = 0.0f;
  float roll = 0.0f;
};

static XrQuaternionf MultiplyQuaternions(const XrQuaternionf& a, const XrQuaternionf& b)
{
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
          a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
          a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

static EulerDeg QuaternionToEulerDeg(const XrQuaternionf& q)
{
  // Yaw(Z), pitch(Y), roll(X) for quick diagnostics in logs.
  const float sinr_cosp = 2.0f * (q.w * q.x + q.y * q.z);
  const float cosr_cosp = 1.0f - 2.0f * (q.x * q.x + q.y * q.y);
  const float roll = std::atan2(sinr_cosp, cosr_cosp);

  const float sinp = 2.0f * (q.w * q.y - q.z * q.x);
  const float pitch = std::abs(sinp) >= 1.0f ? std::copysign(1.57079633f, sinp) :
                                                std::asin(sinp);

  const float siny_cosp = 2.0f * (q.w * q.z + q.x * q.y);
  const float cosy_cosp = 1.0f - 2.0f * (q.y * q.y + q.z * q.z);
  const float yaw = std::atan2(siny_cosp, cosy_cosp);

  constexpr float RAD_TO_DEG = 57.2957795f;
  return {yaw * RAD_TO_DEG, pitch * RAD_TO_DEG, roll * RAD_TO_DEG};
}

static void CopyOpenXRName(char* dst, size_t dst_size, std::string_view src)
{
  std::memset(dst, 0, dst_size);
  const size_t copy_size = std::min(dst_size - 1, src.size());
  std::memcpy(dst, src.data(), copy_size);
}
}  // namespace

// Checks an XrResult and returns false (with an error log) on failure.
// Requires m_instance to be valid for error string lookup.
#define XR_CHECK(expr)                                                                             \
  do                                                                                               \
  {                                                                                                \
    const XrResult _r = (expr);                                                                    \
    if (XR_FAILED(_r))                                                                             \
    {                                                                                              \
      char _buf[XR_MAX_RESULT_STRING_SIZE]{};                                                      \
      xrResultToString(m_instance, _r, _buf);                                                      \
      ERROR_LOG_FMT(OPENXR, "OpenXR: {} failed: {}", #expr, _buf);                                  \
      return false;                                                                                 \
    }                                                                                              \
  } while (false)

OpenXRManager::OpenXRManager() = default;

OpenXRManager::~OpenXRManager()
{
  if (m_swapchain)
    m_swapchain->WaitForPendingFrameFinalization("before XR session destruction");

  DestroyInputActions();
  ResetInputActionsState();

  if (m_reference_space != XR_NULL_HANDLE)
    xrDestroySpace(m_reference_space);
#if defined(ANDROID)
  if (m_android_tracking_space != XR_NULL_HANDLE)
    xrDestroySpace(m_android_tracking_space);
#endif

  if (m_session != XR_NULL_HANDLE)
  {
    if (m_session_running)
      xrEndSession(m_session);
    xrDestroySession(m_session);
  }

  if (m_instance != XR_NULL_HANDLE)
    xrDestroyInstance(m_instance);
}

bool OpenXRManager::IsRuntimeExtensionSupported(const char* extension_name)
{
  if (!extension_name || extension_name[0] == '\0')
    return false;

#if defined(ANDROID)
  if (!EnsureAndroidOpenXRLoaderInitialized())
    return false;
#endif

  uint32_t count = 0;
  XrResult result = xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr);
  if (XR_FAILED(result) || count == 0)
    return false;

  std::vector<XrExtensionProperties> properties(count, {XR_TYPE_EXTENSION_PROPERTIES});
  result = xrEnumerateInstanceExtensionProperties(nullptr, count, &count, properties.data());
  if (XR_FAILED(result))
    return false;

  const std::string_view wanted(extension_name);
  for (const XrExtensionProperties& property : properties)
  {
    if (wanted == property.extensionName)
      return true;
  }

  return false;
}

std::vector<const char*> OpenXRManager::GetAvailableControllerExtensions()
{
  std::vector<const char*> extensions;
  return extensions;
}

bool OpenXRManager::CreateInstance(const std::vector<const char*>& extra_extensions)
{
  m_enabled_extensions.clear();

#if defined(ANDROID)
  if (!EnsureAndroidOpenXRLoaderInitialized())
    return false;
#endif

  // Log available API layers.
  uint32_t layer_count = 0;
  xrEnumerateApiLayerProperties(0, &layer_count, nullptr);
  std::vector<XrApiLayerProperties> layers(layer_count, {XR_TYPE_API_LAYER_PROPERTIES});
  xrEnumerateApiLayerProperties(layer_count, &layer_count, layers.data());
  for (const auto& layer : layers)
    INFO_LOG_FMT(OPENXR, "OpenXR: Available API layer: {}", layer.layerName);

  // Enumerate and verify extensions.
  uint32_t ext_count = 0;
  xrEnumerateInstanceExtensionProperties(nullptr, 0, &ext_count, nullptr);
  std::vector<XrExtensionProperties> exts(ext_count, {XR_TYPE_EXTENSION_PROPERTIES});
  xrEnumerateInstanceExtensionProperties(nullptr, ext_count, &ext_count, exts.data());
  for (const auto& ext : exts)
    INFO_LOG_FMT(OPENXR, "OpenXR: Available extension: {}", ext.extensionName);

  for (const char* required : extra_extensions)
  {
    const bool found =
        std::any_of(exts.begin(), exts.end(), [required](const XrExtensionProperties& e) {
          return std::string_view{e.extensionName} == required;
        });
    if (!found)
    {
      ERROR_LOG_FMT(OPENXR, "OpenXR: Required extension '{}' not available.", required);
      return false;
    }
  }

  XrVersion requested_api_version = XR_CURRENT_API_VERSION;
  INFO_LOG_FMT(OPENXR, "OpenXR: Requesting API version {}.{}.{}.",
               XR_VERSION_MAJOR(requested_api_version), XR_VERSION_MINOR(requested_api_version),
               XR_VERSION_PATCH(requested_api_version));

  XrApplicationInfo app_info{};
  std::strncpy(app_info.applicationName, "PrimedGun", XR_MAX_APPLICATION_NAME_SIZE - 1);
  app_info.applicationVersion = 1;
  std::strncpy(app_info.engineName, "PrimedGun", XR_MAX_ENGINE_NAME_SIZE - 1);
  app_info.engineVersion = 1;
  app_info.apiVersion = requested_api_version;

  XrInstanceCreateInfo create_info{XR_TYPE_INSTANCE_CREATE_INFO};
  create_info.applicationInfo = app_info;
  create_info.enabledExtensionCount = static_cast<uint32_t>(extra_extensions.size());
  create_info.enabledExtensionNames = extra_extensions.data();

#if defined(ANDROID)
  XrInstanceCreateInfoAndroidKHR android_info{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
  {
    std::lock_guard guard{s_android_openxr_mutex};
    android_info.applicationVM = s_android_vm;
    android_info.applicationActivity = s_android_activity;
  }
  create_info.next = &android_info;
#endif

  XrResult result = xrCreateInstance(&create_info, &m_instance);
  if (result == XR_ERROR_API_VERSION_UNSUPPORTED && requested_api_version != XR_API_VERSION_1_0)
  {
    WARN_LOG_FMT(OPENXR,
                 "OpenXR: Runtime rejected API version {}.{}.{}; retrying with 1.0.",
                 XR_VERSION_MAJOR(requested_api_version), XR_VERSION_MINOR(requested_api_version),
                 XR_VERSION_PATCH(requested_api_version));
    app_info.apiVersion = XR_API_VERSION_1_0;
    create_info.applicationInfo = app_info;
    result = xrCreateInstance(&create_info, &m_instance);
  }

  if (XR_FAILED(result))
  {
    ERROR_LOG_FMT(OPENXR,
                  "OpenXR: xrCreateInstance failed ({}).",
                  static_cast<int>(result));
    return false;
  }

  m_enabled_extensions.reserve(extra_extensions.size());
  for (const char* extension : extra_extensions)
  {
    if (extension)
      m_enabled_extensions.emplace_back(extension);
  }

  XrInstanceProperties props{XR_TYPE_INSTANCE_PROPERTIES};
  xrGetInstanceProperties(m_instance, &props);
  m_runtime_name = props.runtimeName;
  INFO_LOG_FMT(OPENXR, "OpenXR: Runtime '{}' version {}.{}.{}", props.runtimeName,
               XR_VERSION_MAJOR(props.runtimeVersion), XR_VERSION_MINOR(props.runtimeVersion),
               XR_VERSION_PATCH(props.runtimeVersion));

#if defined(ANDROID)
  if (IsExtensionEnabled(XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME))
    xrGetInstanceProcAddr(m_instance, "xrSetAndroidApplicationThreadKHR", &m_set_android_thread);
  if (IsExtensionEnabled(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME))
  {
    xrGetInstanceProcAddr(m_instance, "xrPerfSettingsSetPerformanceLevelEXT",
                         reinterpret_cast<PFN_xrVoidFunction*>(&m_set_performance_level));
  }
  if (IsExtensionEnabled(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))
  {
    xrGetInstanceProcAddr(m_instance, "xrEnumerateDisplayRefreshRatesFB",
                         reinterpret_cast<PFN_xrVoidFunction*>(&m_enumerate_refresh_rates));
    xrGetInstanceProcAddr(m_instance, "xrRequestDisplayRefreshRateFB",
                         reinterpret_cast<PFN_xrVoidFunction*>(&m_request_refresh_rate));
    xrGetInstanceProcAddr(m_instance, "xrGetDisplayRefreshRateFB",
                         reinterpret_cast<PFN_xrVoidFunction*>(&m_get_refresh_rate));
  }
  if (IsExtensionEnabled(XR_META_PERFORMANCE_METRICS_EXTENSION_NAME))
  {
    xrGetInstanceProcAddr(m_instance, "xrEnumeratePerformanceMetricsCounterPathsMETA",
                         reinterpret_cast<PFN_xrVoidFunction*>(&m_enumerate_performance_counters));
    xrGetInstanceProcAddr(m_instance, "xrSetPerformanceMetricsStateMETA",
                         reinterpret_cast<PFN_xrVoidFunction*>(&m_set_performance_metrics));
    xrGetInstanceProcAddr(m_instance, "xrQueryPerformanceMetricsCounterMETA",
                         reinterpret_cast<PFN_xrVoidFunction*>(&m_query_performance_counter));
  }
#endif

  return true;
}

#if defined(ANDROID)
void OpenXRManager::SetAndroidAppInfo(JavaVM* vm, JNIEnv* env, jobject activity)
{
  std::lock_guard guard{s_android_openxr_mutex};
  if (s_android_activity)
    env->DeleteGlobalRef(s_android_activity);
  s_android_vm = vm;
  s_android_activity = activity ? env->NewGlobalRef(activity) : nullptr;
  s_android_loader_initialized = false;
}

void OpenXRManager::ClearAndroidAppInfo(JNIEnv* env)
{
  std::lock_guard guard{s_android_openxr_mutex};
  if (s_android_activity)
    env->DeleteGlobalRef(s_android_activity);
  s_android_activity = nullptr;
  s_android_vm = nullptr;
  s_android_loader_initialized = false;
}

bool OpenXRManager::RegisterAndroidThread(AndroidThreadType type, uint32_t thread_id,
                                          std::string_view label)
{
  if (!m_set_android_thread || m_session == XR_NULL_HANDLE)
    return false;

  XrAndroidThreadTypeKHR xr_type;
  switch (type)
  {
  case AndroidThreadType::ApplicationMain:
    xr_type = XR_ANDROID_THREAD_TYPE_APPLICATION_MAIN_KHR;
    break;
  case AndroidThreadType::ApplicationWorker:
    xr_type = XR_ANDROID_THREAD_TYPE_APPLICATION_WORKER_KHR;
    break;
  case AndroidThreadType::RendererMain:
    xr_type = XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR;
    break;
  case AndroidThreadType::RendererWorker:
    xr_type = XR_ANDROID_THREAD_TYPE_RENDERER_WORKER_KHR;
    break;
  }

  const auto set_thread = reinterpret_cast<PFN_xrSetAndroidApplicationThreadKHR>(m_set_android_thread);
  XrResult result = set_thread(m_session, xr_type, thread_id);
  // Some Quest runtime versions do not accept the renderer-worker category.
  if (XR_FAILED(result) && type == AndroidThreadType::RendererWorker)
    result = set_thread(m_session, XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR, thread_id);
  if (XR_FAILED(result))
    WARN_LOG_FMT(OPENXR, "OpenXR: Could not register Android thread '{}' ({}).", label,
                 static_cast<int>(result));
  return XR_SUCCEEDED(result);
}

bool OpenXRManager::RegisterCurrentAndroidThread(AndroidThreadType type, std::string_view label)
{
  const uint32_t thread_id = static_cast<uint32_t>(gettid());
  std::lock_guard guard{m_android_threads_mutex};
  if (m_session == XR_NULL_HANDLE)
  {
    m_pending_android_threads.push_back({type, thread_id, std::string(label)});
    return false;
  }
  return RegisterAndroidThread(type, thread_id, label);
}

void OpenXRManager::FlushPendingAndroidThreadRegistrations()
{
  std::lock_guard guard{m_android_threads_mutex};
  for (const auto& pending : m_pending_android_threads)
    RegisterAndroidThread(pending.type, pending.thread_id, pending.label);
  m_pending_android_threads.clear();
}

void OpenXRManager::ConfigureAndroidSession()
{
  m_perf_window_start_us = Common::Timer::NowUs();
  m_perf_frames = m_perf_wait_us = m_perf_pending_us = m_perf_begin_us = m_perf_locate_us =
      m_perf_input_us = 0;
  if (m_set_performance_level)
  {
    for (XrPerfSettingsDomainEXT domain : {XR_PERF_SETTINGS_DOMAIN_CPU_EXT,
                                         XR_PERF_SETTINGS_DOMAIN_GPU_EXT})
    {
      const XrResult result = m_set_performance_level(
          m_session, domain, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
      if (XR_FAILED(result))
        WARN_LOG_FMT(OPENXR, "OpenXR: Performance request failed for domain {} ({}).",
                     static_cast<int>(domain), static_cast<int>(result));
    }
  }

  ConfigureAndroidRefreshRate();
  ConfigureAndroidPerformanceMetrics();
}

void OpenXRManager::ConfigureAndroidRefreshRate()
{
  // Keep the conservative default, but permit session-local cadence experiments
  // without changing the headset's global refresh-rate override.
  constexpr float default_rate = 72.0f;
  Common::IniFile ini;
  ini.Load(File::GetUserPath(D_CONFIG_IDX) + "PrimedGun.ini");
  const auto* quest = ini.GetOrCreateSection("Quest");
  std::string configured_rate;
  quest->Get("RefreshRate", &configured_rate, "72");
  float requested_rate = default_rate;
  const bool parsed = quest->Get("RefreshRate", &requested_rate, default_rate);
  if ((!parsed && ini.Exists("Quest", "RefreshRate")) || !std::isfinite(requested_rate) ||
      requested_rate <= 0.0f)
  {
    WARN_LOG_FMT(OPENXR, "OpenXR: Invalid Quest RefreshRate '{}'; using {} Hz.",
                 configured_rate, default_rate);
    requested_rate = default_rate;
  }

  std::vector<float> rates;
  if (m_enumerate_refresh_rates)
  {
    uint32_t count = 0;
    XrResult result = m_enumerate_refresh_rates(m_session, 0, &count, nullptr);
    if (XR_SUCCEEDED(result) && count > 0)
    {
      rates.resize(count);
      result = m_enumerate_refresh_rates(m_session, count, &count, rates.data());
      if (XR_SUCCEEDED(result))
        rates.resize(count);
      else
        rates.clear();
    }
    if (XR_FAILED(result))
      WARN_LOG_FMT(OPENXR, "OpenXR: Could not enumerate Quest refresh rates ({}).",
                   static_cast<int>(result));
  }

  std::erase_if(rates, [](float rate) { return !std::isfinite(rate) || rate <= 0.0f; });
  std::string supported_rates;
  for (float rate : rates)
    supported_rates += fmt::format("{}{}", supported_rates.empty() ? "" : ", ", rate);

  const auto find_rate = [&rates](float wanted) {
    return std::find_if(rates.begin(), rates.end(),
                        [wanted](float rate) { return std::abs(rate - wanted) < 0.1f; });
  };
  float selected_rate = 0.0f;
  XrResult request_result = XR_ERROR_FUNCTION_UNSUPPORTED;
  if (m_request_refresh_rate && !rates.empty())
  {
    const auto default_it = find_rate(default_rate);
    const float fallback_rate =
        default_it != rates.end() ? *default_it : *std::min_element(rates.begin(), rates.end());
    const auto requested_it = find_rate(requested_rate);
    selected_rate = requested_it != rates.end() ? *requested_it : fallback_rate;
    if (requested_it == rates.end())
      WARN_LOG_FMT(OPENXR, "OpenXR: Quest {} Hz is unsupported; using {} Hz.",
                   requested_rate, selected_rate);

    // Pass the exact enumerated value; an accepted request is still only a
    // preference. xrGetDisplayRefreshRateFB and change events report reality.
    request_result = m_request_refresh_rate(m_session, selected_rate);
    if (XR_FAILED(request_result) && selected_rate != fallback_rate)
    {
      WARN_LOG_FMT(OPENXR, "OpenXR: Quest {} Hz request failed ({}); retrying {} Hz.",
                   selected_rate, static_cast<int>(request_result), fallback_rate);
      selected_rate = fallback_rate;
      request_result = m_request_refresh_rate(m_session, selected_rate);
    }
  }
  else
  {
    WARN_LOG_FMT(OPENXR, "OpenXR: Quest refresh-rate selection is unavailable; keeping runtime rate.");
  }

  std::string actual_rate = "unavailable";
  if (m_get_refresh_rate)
  {
    float refresh_rate = 0.0f;
    if (XR_SUCCEEDED(m_get_refresh_rate(m_session, &refresh_rate)) &&
        std::isfinite(refresh_rate) && refresh_rate > 0.0f)
    {
      m_native_display_period_ms = 1000.0 / refresh_rate;
      actual_rate = fmt::format("{} Hz", refresh_rate);
    }
  }
  INFO_LOG_FMT(OPENXR,
               "OpenXR: Quest refresh rate configured={} supported=[{}] requested={} Hz "
               "request_result={} actual={}.",
               configured_rate, supported_rates, selected_rate, static_cast<int>(request_result),
               actual_rate);
}

void OpenXRManager::ConfigureAndroidPerformanceMetrics()
{
  // Runtime metrics have their own collection cost, so enable them only for an
  // explicit profiling run. They are diagnostic inputs, never adaptive settings.
  Common::IniFile ini;
  ini.Load(File::GetUserPath(D_CONFIG_IDX) + "PrimedGun.ini");
  bool requested = false;
  ini.GetOrCreateSection("Quest")->Get("PerformanceMetrics", &requested, false);
  m_perf_logging_enabled = requested;
  m_performance_metrics_enabled = false;
  m_performance_counters.clear();
  if (!requested)
    return;
  if (!m_enumerate_performance_counters || !m_set_performance_metrics ||
      !m_query_performance_counter)
  {
    WARN_LOG_FMT(OPENXR, "XRPERF: runtime performance metrics are unavailable.");
    return;
  }

  uint32_t count = 0;
  if (XR_FAILED(m_enumerate_performance_counters(m_instance, 0, &count, nullptr)))
    return;
  std::vector<XrPath> paths(count);
  if (XR_FAILED(m_enumerate_performance_counters(m_instance, count, &count, paths.data())))
    return;
  paths.resize(count);
  constexpr std::array<std::string_view, 9> wanted = {
      "/perfmetrics_meta/app/cpu_frametime",
      "/perfmetrics_meta/app/gpu_frametime",
      "/perfmetrics_meta/app/motion_to_photon_latency",
      "/perfmetrics_meta/compositor/cpu_frametime",
      "/perfmetrics_meta/compositor/gpu_frametime",
      "/perfmetrics_meta/compositor/dropped_frame_count",
      "/perfmetrics_meta/device/cpu_utilization_average",
      "/perfmetrics_meta/device/cpu_utilization_worst",
      "/perfmetrics_meta/device/gpu_utilization"};
  for (XrPath path : paths)
  {
    std::array<char, XR_MAX_PATH_LENGTH> name{};
    uint32_t length = 0;
    if (XR_FAILED(xrPathToString(m_instance, path, static_cast<uint32_t>(name.size()), &length,
                                 name.data())))
      continue;
    const std::string_view full_name{name.data()};
    if (std::find(wanted.begin(), wanted.end(), full_name) != wanted.end())
    {
      constexpr std::string_view prefix = "/perfmetrics_meta/";
      m_performance_counters.push_back({path, std::string(full_name.substr(prefix.size()))});
    }
  }
  XrPerformanceMetricsStateMETA state{XR_TYPE_PERFORMANCE_METRICS_STATE_META};
  state.enabled = XR_TRUE;
  const XrResult result = m_set_performance_metrics(m_session, &state);
  m_performance_metrics_enabled = XR_SUCCEEDED(result);
  WARN_LOG_FMT(OPENXR, "XRPERF: runtime metrics enabled={} counters={} result={}",
               m_performance_metrics_enabled, m_performance_counters.size(),
               static_cast<int>(result));
}

void OpenXRManager::LogAndroidPerformance()
{
  if (!m_perf_logging_enabled)
    return;
  const uint64_t now_us = Common::Timer::NowUs();
  if (now_us - m_perf_window_start_us < 5'000'000 || m_perf_frames == 0)
    return;
  const double frames = static_cast<double>(m_perf_frames);
  WARN_LOG_FMT(OPENXR,
               "XRPERF loop: frames={} wall={:.2f}ms/f wait={:.2f}ms/f pending_submit={:.2f}ms/f "
               "begin={:.2f}ms/f locate={:.2f}ms/f input={:.2f}ms/f refresh={:.1f}Hz",
               m_perf_frames, (now_us - m_perf_window_start_us) / 1000.0 / frames,
               m_perf_wait_us / 1000.0 / frames, m_perf_pending_us / 1000.0 / frames,
               m_perf_begin_us / 1000.0 / frames, m_perf_locate_us / 1000.0 / frames,
               m_perf_input_us / 1000.0 / frames, GetStartupDisplayRefreshRateHz());
  m_perf_window_start_us = now_us;
  m_perf_frames = m_perf_wait_us = m_perf_pending_us = m_perf_begin_us = m_perf_locate_us =
      m_perf_input_us = 0;
  if (!m_performance_metrics_enabled)
    return;

  // These are runtime-defined sample intervals, not averages over our five-second
  // logging window. Preserve counter units and report missing values explicitly.
  std::string values;
  for (const auto& metric : m_performance_counters)
  {
    XrPerformanceMetricsCounterMETA counter{XR_TYPE_PERFORMANCE_METRICS_COUNTER_META};
    const XrResult result = m_query_performance_counter(m_session, metric.path, &counter);
    const char* unit = "";
    switch (counter.counterUnit)
    {
    case XR_PERFORMANCE_METRICS_COUNTER_UNIT_MILLISECONDS_META:
      unit = "ms";
      break;
    case XR_PERFORMANCE_METRICS_COUNTER_UNIT_PERCENTAGE_META:
      unit = "%";
      break;
    default:
      break;
    }
    if (XR_SUCCEEDED(result) &&
        (counter.counterFlags & XR_PERFORMANCE_METRICS_COUNTER_FLOAT_VALUE_VALID_BIT_META))
      values += fmt::format(" {}={:.2f}{}", metric.name, counter.floatValue, unit);
    else if (XR_SUCCEEDED(result) &&
             (counter.counterFlags & XR_PERFORMANCE_METRICS_COUNTER_UINT_VALUE_VALID_BIT_META))
      values += fmt::format(" {}={}{}", metric.name, counter.uintValue, unit);
    else
      values += fmt::format(" {}=unavailable", metric.name);
  }
  WARN_LOG_FMT(OPENXR, "XRPERF runtime sample:{}", values);
}
#endif

bool OpenXRManager::InitializeSystem()
{
  XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
  system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

  const XrResult result = xrGetSystem(m_instance, &system_info, &m_system_id);
  if (XR_FAILED(result))
  {
    ERROR_LOG_FMT(OPENXR,
                  "OpenXR: xrGetSystem failed ({}). Is a headset connected?",
                  static_cast<int>(result));
    return false;
  }

  XrSystemProperties props{XR_TYPE_SYSTEM_PROPERTIES};
  xrGetSystemProperties(m_instance, m_system_id, &props);
  INFO_LOG_FMT(OPENXR, "OpenXR: System '{}' (vendor {:08x})", props.systemName, props.vendorId);

  return true;
}

bool OpenXRManager::EnumerateViewConfigurations()
{
  uint32_t view_count = 0;
  XrResult result = xrEnumerateViewConfigurationViews(
      m_instance, m_system_id, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &view_count, nullptr);

  if (XR_FAILED(result) || view_count != 2)
  {
    ERROR_LOG_FMT(OPENXR,
                  "OpenXR: Failed to enumerate view configs or unexpected count ({}). "
                  "Expected 2 views for stereo.",
                  view_count);
    return false;
  }

  m_view_config_views.fill({XR_TYPE_VIEW_CONFIGURATION_VIEW});
  XR_CHECK(xrEnumerateViewConfigurationViews(m_instance, m_system_id,
                                             XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                             view_count, &view_count, m_view_config_views.data()));

  for (uint32_t i = 0; i < view_count; ++i)
  {
    INFO_LOG_FMT(OPENXR, "OpenXR: Eye {} recommended {}x{} (max {}x{})", i,
                 m_view_config_views[i].recommendedImageRectWidth,
                 m_view_config_views[i].recommendedImageRectHeight,
                 m_view_config_views[i].maxImageRectWidth,
                 m_view_config_views[i].maxImageRectHeight);
  }

  return true;
}

void OpenXRManager::SetSession(XrSession session)
{
  ++m_eye_views_generation;
#if defined(ANDROID)
  {
    std::lock_guard guard{m_android_threads_mutex};
    m_session = session;
  }
#else
  m_session = session;
#endif

  if (m_session == XR_NULL_HANDLE)
  {
    DestroyInputActions();
    ResetInputActionsState();
    return;
  }

  if (!InitializeInputActions())
  {
    WARN_LOG_FMT(OPENXR, "OpenXR: Controller input actions unavailable.");
    ResetInputActionsState();
  }
#if defined(ANDROID)
  FlushPendingAndroidThreadRegistrations();
  RegisterCurrentAndroidThread(AndroidThreadType::RendererMain, "Emulation render thread");
#endif
}

bool OpenXRManager::InitializeInputActions()
{
  if (m_input_action_set != XR_NULL_HANDLE)
    return true;

  if (m_instance == XR_NULL_HANDLE || m_session == XR_NULL_HANDLE)
    return false;

  auto to_path = [this](const char* path) -> XrPath {
    XrPath xr_path = XR_NULL_PATH;
    if (XR_FAILED(xrStringToPath(m_instance, path, &xr_path)))
      return XR_NULL_PATH;
    return xr_path;
  };

  m_input_hand_paths[0] = to_path("/user/hand/left");
  m_input_hand_paths[1] = to_path("/user/hand/right");
  if (m_input_hand_paths[0] == XR_NULL_PATH || m_input_hand_paths[1] == XR_NULL_PATH)
  {
    ERROR_LOG_FMT(OPENXR, "OpenXR: Failed to create hand subaction paths.");
    return false;
  }

  XrActionSetCreateInfo action_set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
  CopyOpenXRName(action_set_info.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "dolphin_input");
  CopyOpenXRName(action_set_info.localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE,
                 "Dolphin Input");
  action_set_info.priority = 0;

  XrResult result = xrCreateActionSet(m_instance, &action_set_info, &m_input_action_set);
  if (XR_FAILED(result))
  {
    ERROR_LOG_FMT(OPENXR, "OpenXR: xrCreateActionSet failed ({}).", static_cast<int>(result));
    return false;
  }

  const auto create_action = [this](XrAction* action, const char* name, const char* localized_name,
                                    XrActionType type) -> bool {
    XrActionCreateInfo action_info{XR_TYPE_ACTION_CREATE_INFO};
    CopyOpenXRName(action_info.actionName, XR_MAX_ACTION_NAME_SIZE, name);
    CopyOpenXRName(action_info.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE,
                   localized_name);
    action_info.actionType = type;
    action_info.countSubactionPaths = static_cast<uint32_t>(m_input_hand_paths.size());
    action_info.subactionPaths = m_input_hand_paths.data();

    const XrResult create_result = xrCreateAction(m_input_action_set, &action_info, action);
    if (XR_FAILED(create_result))
    {
      ERROR_LOG_FMT(OPENXR, "OpenXR: xrCreateAction('{}') failed ({}).", name,
                    static_cast<int>(create_result));
      return false;
    }
    return true;
  };

  if (!create_action(&m_action_primary_click, "primary_click", "Primary Button",
                     XR_ACTION_TYPE_BOOLEAN_INPUT) ||
      !create_action(&m_action_secondary_click, "secondary_click", "Secondary Button",
                     XR_ACTION_TYPE_BOOLEAN_INPUT) ||
      !create_action(&m_action_menu_click, "menu_click", "Menu Button",
                     XR_ACTION_TYPE_BOOLEAN_INPUT) ||
      !create_action(&m_action_thumbstick_click, "thumbstick_click", "Thumbstick Click",
                     XR_ACTION_TYPE_BOOLEAN_INPUT) ||
      !create_action(&m_action_trigger_click, "trigger_click", "Trigger Click",
                     XR_ACTION_TYPE_BOOLEAN_INPUT) ||
      !create_action(&m_action_squeeze_click, "squeeze_click", "Squeeze Click",
                     XR_ACTION_TYPE_BOOLEAN_INPUT) ||
      !create_action(&m_action_trigger_value, "trigger_value", "Trigger Value",
                     XR_ACTION_TYPE_FLOAT_INPUT) ||
      !create_action(&m_action_squeeze_value, "squeeze_value", "Squeeze Value",
                     XR_ACTION_TYPE_FLOAT_INPUT) ||
      !create_action(&m_action_thumbstick_x, "thumbstick_x", "Thumbstick X",
                     XR_ACTION_TYPE_FLOAT_INPUT) ||
      !create_action(&m_action_thumbstick_y, "thumbstick_y", "Thumbstick Y",
                     XR_ACTION_TYPE_FLOAT_INPUT) ||
      !create_action(&m_action_trackpad_click, "trackpad_click", "Trackpad Click",
                     XR_ACTION_TYPE_BOOLEAN_INPUT) ||
      !create_action(&m_action_trackpad_touch, "trackpad_touch", "Trackpad Touch",
                     XR_ACTION_TYPE_BOOLEAN_INPUT) ||
      !create_action(&m_action_trackpad_x, "trackpad_x", "Trackpad X",
                     XR_ACTION_TYPE_FLOAT_INPUT) ||
      !create_action(&m_action_trackpad_y, "trackpad_y", "Trackpad Y",
                     XR_ACTION_TYPE_FLOAT_INPUT) ||
      !create_action(&m_action_trackpad_force, "trackpad_force", "Trackpad Force",
                     XR_ACTION_TYPE_FLOAT_INPUT) ||
      !create_action(&m_action_aim_pose, "aim_pose", "Aim Pose", XR_ACTION_TYPE_POSE_INPUT) ||
      !create_action(&m_action_grip_pose, "grip_pose", "Grip Pose", XR_ACTION_TYPE_POSE_INPUT) ||
      !create_action(&m_action_haptic, "haptic", "Haptic Output",
                     XR_ACTION_TYPE_VIBRATION_OUTPUT))
  {
    DestroyInputActions();
    return false;
  }

  struct BindingDef
  {
    XrAction action = XR_NULL_HANDLE;
    const char* path = nullptr;
  };

  const auto suggest_bindings = [this, &to_path](const char* profile,
                                                 std::initializer_list<BindingDef> defs) {
    const XrPath profile_path = to_path(profile);
    if (profile_path == XR_NULL_PATH)
      return;

    std::vector<XrActionSuggestedBinding> bindings;
    bindings.reserve(defs.size());
    for (const auto& def : defs)
    {
      if (def.action == XR_NULL_HANDLE || def.path == nullptr)
        continue;
      const XrPath binding_path = to_path(def.path);
      if (binding_path == XR_NULL_PATH)
        continue;
      bindings.push_back({def.action, binding_path});
    }

    if (bindings.empty())
      return;

    XrInteractionProfileSuggestedBinding suggested{
        XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggested.interactionProfile = profile_path;
    suggested.countSuggestedBindings = static_cast<uint32_t>(bindings.size());
    suggested.suggestedBindings = bindings.data();
    const XrResult suggest_result = xrSuggestInteractionProfileBindings(m_instance, &suggested);
    if (XR_FAILED(suggest_result))
    {
      WARN_LOG_FMT(OPENXR, "OpenXR: xrSuggestInteractionProfileBindings('{}') failed ({}).", profile,
                   static_cast<int>(suggest_result));
    }
  };

  suggest_bindings("/interaction_profiles/khr/simple_controller",
                   {
                       {m_action_primary_click, "/user/hand/left/input/select/click"},
                       {m_action_primary_click, "/user/hand/right/input/select/click"},
                       {m_action_menu_click, "/user/hand/left/input/menu/click"},
                       {m_action_menu_click, "/user/hand/right/input/menu/click"},
                       {m_action_aim_pose, "/user/hand/left/input/aim/pose"},
                       {m_action_aim_pose, "/user/hand/right/input/aim/pose"},
                       {m_action_grip_pose, "/user/hand/left/input/grip/pose"},
                       {m_action_grip_pose, "/user/hand/right/input/grip/pose"},
                       {m_action_haptic, "/user/hand/left/output/haptic"},
                       {m_action_haptic, "/user/hand/right/output/haptic"},
                   });

  suggest_bindings("/interaction_profiles/oculus/touch_controller",
                   {
                       {m_action_primary_click, "/user/hand/left/input/x/click"},
                       {m_action_secondary_click, "/user/hand/left/input/y/click"},
                       {m_action_menu_click, "/user/hand/left/input/menu/click"},
                       {m_action_thumbstick_click, "/user/hand/left/input/thumbstick/click"},
                       {m_action_thumbstick_x, "/user/hand/left/input/thumbstick/x"},
                       {m_action_thumbstick_y, "/user/hand/left/input/thumbstick/y"},
                       {m_action_trigger_value, "/user/hand/left/input/trigger/value"},
                       {m_action_squeeze_value, "/user/hand/left/input/squeeze/value"},
                       {m_action_aim_pose, "/user/hand/left/input/aim/pose"},
                       {m_action_grip_pose, "/user/hand/left/input/grip/pose"},
                       {m_action_primary_click, "/user/hand/right/input/a/click"},
                       {m_action_secondary_click, "/user/hand/right/input/b/click"},
                       {m_action_thumbstick_click, "/user/hand/right/input/thumbstick/click"},
                       {m_action_thumbstick_x, "/user/hand/right/input/thumbstick/x"},
                       {m_action_thumbstick_y, "/user/hand/right/input/thumbstick/y"},
                       {m_action_trigger_value, "/user/hand/right/input/trigger/value"},
                       {m_action_squeeze_value, "/user/hand/right/input/squeeze/value"},
                       {m_action_aim_pose, "/user/hand/right/input/aim/pose"},
                       {m_action_grip_pose, "/user/hand/right/input/grip/pose"},
                       {m_action_haptic, "/user/hand/left/output/haptic"},
                       {m_action_haptic, "/user/hand/right/output/haptic"},
                   });

  suggest_bindings("/interaction_profiles/valve/index_controller",
                   {
                       {m_action_primary_click, "/user/hand/left/input/a/click"},
                       {m_action_secondary_click, "/user/hand/left/input/b/click"},
                       {m_action_thumbstick_click, "/user/hand/left/input/thumbstick/click"},
                       {m_action_thumbstick_x, "/user/hand/left/input/thumbstick/x"},
                       {m_action_thumbstick_y, "/user/hand/left/input/thumbstick/y"},
                       {m_action_trigger_value, "/user/hand/left/input/trigger/value"},
                       {m_action_squeeze_value, "/user/hand/left/input/squeeze/value"},
                       {m_action_trackpad_touch, "/user/hand/left/input/trackpad/touch"},
                       {m_action_trackpad_x, "/user/hand/left/input/trackpad/x"},
                       {m_action_trackpad_y, "/user/hand/left/input/trackpad/y"},
                       {m_action_trackpad_force, "/user/hand/left/input/trackpad/force"},
                       {m_action_aim_pose, "/user/hand/left/input/aim/pose"},
                       {m_action_grip_pose, "/user/hand/left/input/grip/pose"},
                       {m_action_primary_click, "/user/hand/right/input/a/click"},
                       {m_action_secondary_click, "/user/hand/right/input/b/click"},
                       {m_action_thumbstick_click, "/user/hand/right/input/thumbstick/click"},
                       {m_action_thumbstick_x, "/user/hand/right/input/thumbstick/x"},
                       {m_action_thumbstick_y, "/user/hand/right/input/thumbstick/y"},
                       {m_action_trigger_value, "/user/hand/right/input/trigger/value"},
                       {m_action_squeeze_value, "/user/hand/right/input/squeeze/value"},
                       {m_action_trackpad_touch, "/user/hand/right/input/trackpad/touch"},
                       {m_action_trackpad_x, "/user/hand/right/input/trackpad/x"},
                       {m_action_trackpad_y, "/user/hand/right/input/trackpad/y"},
                       {m_action_trackpad_force, "/user/hand/right/input/trackpad/force"},
                       {m_action_aim_pose, "/user/hand/right/input/aim/pose"},
                       {m_action_grip_pose, "/user/hand/right/input/grip/pose"},
                       {m_action_haptic, "/user/hand/left/output/haptic"},
                       {m_action_haptic, "/user/hand/right/output/haptic"},
                   });

  suggest_bindings("/interaction_profiles/microsoft/motion_controller",
                   {
                       {m_action_menu_click, "/user/hand/left/input/menu/click"},
                       {m_action_thumbstick_click, "/user/hand/left/input/thumbstick/click"},
                       {m_action_thumbstick_x, "/user/hand/left/input/thumbstick/x"},
                       {m_action_thumbstick_y, "/user/hand/left/input/thumbstick/y"},
                       {m_action_trigger_value, "/user/hand/left/input/trigger/value"},
                       {m_action_squeeze_click, "/user/hand/left/input/squeeze/click"},
                       {m_action_aim_pose, "/user/hand/left/input/aim/pose"},
                       {m_action_grip_pose, "/user/hand/left/input/grip/pose"},
                       {m_action_menu_click, "/user/hand/right/input/menu/click"},
                       {m_action_thumbstick_click, "/user/hand/right/input/thumbstick/click"},
                       {m_action_thumbstick_x, "/user/hand/right/input/thumbstick/x"},
                       {m_action_thumbstick_y, "/user/hand/right/input/thumbstick/y"},
                       {m_action_trigger_value, "/user/hand/right/input/trigger/value"},
                       {m_action_squeeze_click, "/user/hand/right/input/squeeze/click"},
                       {m_action_aim_pose, "/user/hand/right/input/aim/pose"},
                       {m_action_grip_pose, "/user/hand/right/input/grip/pose"},
                       {m_action_haptic, "/user/hand/left/output/haptic"},
                       {m_action_haptic, "/user/hand/right/output/haptic"},
                   });

  suggest_bindings("/interaction_profiles/htc/vive_controller",
                   {
                       {m_action_menu_click, "/user/hand/left/input/menu/click"},
                       {m_action_trackpad_click, "/user/hand/left/input/trackpad/click"},
                       {m_action_trackpad_touch, "/user/hand/left/input/trackpad/touch"},
                       {m_action_trackpad_x, "/user/hand/left/input/trackpad/x"},
                       {m_action_trackpad_y, "/user/hand/left/input/trackpad/y"},
                       {m_action_trigger_value, "/user/hand/left/input/trigger/value"},
                       {m_action_squeeze_click, "/user/hand/left/input/squeeze/click"},
                       {m_action_aim_pose, "/user/hand/left/input/aim/pose"},
                       {m_action_grip_pose, "/user/hand/left/input/grip/pose"},
                       {m_action_menu_click, "/user/hand/right/input/menu/click"},
                       {m_action_trackpad_click, "/user/hand/right/input/trackpad/click"},
                       {m_action_trackpad_touch, "/user/hand/right/input/trackpad/touch"},
                       {m_action_trackpad_x, "/user/hand/right/input/trackpad/x"},
                       {m_action_trackpad_y, "/user/hand/right/input/trackpad/y"},
                       {m_action_trigger_value, "/user/hand/right/input/trigger/value"},
                       {m_action_squeeze_click, "/user/hand/right/input/squeeze/click"},
                       {m_action_aim_pose, "/user/hand/right/input/aim/pose"},
                       {m_action_grip_pose, "/user/hand/right/input/grip/pose"},
                       {m_action_haptic, "/user/hand/left/output/haptic"},
                       {m_action_haptic, "/user/hand/right/output/haptic"},
                   });

  XrSessionActionSetsAttachInfo attach_info{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
  const XrActionSet action_set = m_input_action_set;
  attach_info.countActionSets = 1;
  attach_info.actionSets = &action_set;
  result = xrAttachSessionActionSets(m_session, &attach_info);
  if (XR_FAILED(result))
  {
    ERROR_LOG_FMT(OPENXR, "OpenXR: xrAttachSessionActionSets failed ({}).", static_cast<int>(result));
    DestroyInputActions();
    return false;
  }

  auto create_action_space = [this](XrAction action, XrPath subaction_path, XrSpace* out_space,
                                    const char* label) {
    if (action == XR_NULL_HANDLE || subaction_path == XR_NULL_PATH)
      return;

    XrActionSpaceCreateInfo space_info{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    space_info.action = action;
    space_info.subactionPath = subaction_path;
    space_info.poseInActionSpace = {{0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f}};
    const XrResult space_result = xrCreateActionSpace(m_session, &space_info, out_space);
    if (XR_FAILED(space_result))
    {
      WARN_LOG_FMT(OPENXR, "OpenXR: xrCreateActionSpace('{}') failed ({}).", label,
                   static_cast<int>(space_result));
    }
  };

  for (size_t hand = 0; hand < m_input_hand_paths.size(); ++hand)
  {
    create_action_space(m_action_aim_pose, m_input_hand_paths[hand], &m_aim_spaces[hand], "aim");
    create_action_space(m_action_grip_pose, m_input_hand_paths[hand], &m_grip_spaces[hand],
                        "grip");
  }

  return true;
}

void OpenXRManager::DestroyInputActions()
{
  for (auto& space : m_aim_spaces)
  {
    if (space != XR_NULL_HANDLE)
      xrDestroySpace(space);
    space = XR_NULL_HANDLE;
  }
  for (auto& space : m_grip_spaces)
  {
    if (space != XR_NULL_HANDLE)
      xrDestroySpace(space);
    space = XR_NULL_HANDLE;
  }

  if (m_input_action_set != XR_NULL_HANDLE)
  {
    xrDestroyActionSet(m_input_action_set);
    m_input_action_set = XR_NULL_HANDLE;
  }

  m_input_hand_paths = {XR_NULL_PATH, XR_NULL_PATH};
  m_action_primary_click = XR_NULL_HANDLE;
  m_action_secondary_click = XR_NULL_HANDLE;
  m_action_menu_click = XR_NULL_HANDLE;
  m_action_thumbstick_click = XR_NULL_HANDLE;
  m_action_trigger_click = XR_NULL_HANDLE;
  m_action_squeeze_click = XR_NULL_HANDLE;
  m_action_trigger_value = XR_NULL_HANDLE;
  m_action_squeeze_value = XR_NULL_HANDLE;
  m_action_thumbstick_x = XR_NULL_HANDLE;
  m_action_thumbstick_y = XR_NULL_HANDLE;
  m_action_trackpad_click = XR_NULL_HANDLE;
  m_action_trackpad_touch = XR_NULL_HANDLE;
  m_action_trackpad_x = XR_NULL_HANDLE;
  m_action_trackpad_y = XR_NULL_HANDLE;
  m_action_trackpad_force = XR_NULL_HANDLE;
  m_action_aim_pose = XR_NULL_HANDLE;
  m_action_grip_pose = XR_NULL_HANDLE;
  m_action_haptic = XR_NULL_HANDLE;
  m_haptics_active = {false, false};
}

void OpenXRManager::ResetInputActionsState()
{
  m_haptics_active = {false, false};
  Common::VR::OpenXRInputState::Reset();
}

void OpenXRManager::UpdateHaptics()
{
  if (!m_session_running || m_session == XR_NULL_HANDLE || m_action_haptic == XR_NULL_HANDLE)
    return;

  const auto haptics = Common::VR::OpenXRInputState::GetHaptics();

  // Re-send short pulses while active to approximate continuous rumble.
  constexpr XrDuration vibration_duration_ns = 50'000'000;

  for (size_t hand = 0; hand < m_input_hand_paths.size(); ++hand)
  {
    const XrPath hand_path = m_input_hand_paths[hand];
    if (hand_path == XR_NULL_PATH)
      continue;

    const float amplitude = std::clamp(haptics.amplitude[hand], 0.0f, 1.0f);

    XrHapticActionInfo action_info{XR_TYPE_HAPTIC_ACTION_INFO};
    action_info.action = m_action_haptic;
    action_info.subactionPath = hand_path;

    if (amplitude > 0.001f)
    {
      XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
      vibration.amplitude = amplitude;
      vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
      vibration.duration = vibration_duration_ns;

      xrApplyHapticFeedback(
          m_session, &action_info, reinterpret_cast<const XrHapticBaseHeader*>(&vibration));
      m_haptics_active[hand] = true;
    }
    else if (m_haptics_active[hand])
    {
      xrStopHapticFeedback(m_session, &action_info);
      m_haptics_active[hand] = false;
    }
  }
}

void OpenXRManager::UpdateInputActions()
{
#if defined(ANDROID)
  // Android publishes after locating this frame's eyes, in the same app space.
  // Never keep old movement/buttons alive through lost tracking or focus.
  if (!m_eye_views_valid || m_android_recenter_pending || !m_frame_state.shouldRender ||
      m_session_state != XR_SESSION_STATE_FOCUSED)
  {
    ResetInputActionsState();
    return;
  }
#endif
  if (!m_session_running || m_session == XR_NULL_HANDLE || m_input_action_set == XR_NULL_HANDLE)
  {
    ResetInputActionsState();
    return;
  }

  const XrActiveActionSet active_action_set{m_input_action_set, XR_NULL_PATH};
  XrActionsSyncInfo sync_info{XR_TYPE_ACTIONS_SYNC_INFO};
  sync_info.countActiveActionSets = 1;
  sync_info.activeActionSets = &active_action_set;
  const XrResult sync_result = xrSyncActions(m_session, &sync_info);
  if (XR_FAILED(sync_result))
  {
    ResetInputActionsState();
    return;
  }

  std::array<Common::VR::OpenXRControllerState, 2> controllers{};

  const auto locate_space_state = [this](XrSpace space, Common::VR::OpenXRPoseState* pose_state,
                                         Common::VR::OpenXRVelocityState* velocity_state) {
    if (space == XR_NULL_HANDLE || m_reference_space == XR_NULL_HANDLE)
      return;

    XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY};
    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    location.next = &velocity;

    if (XR_FAILED(
            xrLocateSpace(space, m_reference_space, m_frame_state.predictedDisplayTime, &location)))
    {
      return;
    }

    constexpr XrSpaceLocationFlags required_flags = XR_SPACE_LOCATION_POSITION_VALID_BIT |
                                                    XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
    pose_state->valid = (location.locationFlags & required_flags) == required_flags;
    if (pose_state->valid)
    {
      pose_state->position = {location.pose.position.x, location.pose.position.y,
                              location.pose.position.z};
      pose_state->orientation = {location.pose.orientation.x, location.pose.orientation.y,
                                 location.pose.orientation.z, location.pose.orientation.w};
    }

    if (velocity_state)
    {
      velocity_state->linear_valid =
          (velocity.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) != 0;
      if (velocity_state->linear_valid)
      {
        velocity_state->linear = {velocity.linearVelocity.x, velocity.linearVelocity.y,
                                  velocity.linearVelocity.z};
      }

      velocity_state->angular_valid =
          (velocity.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) != 0;
      if (velocity_state->angular_valid)
      {
        velocity_state->angular = {velocity.angularVelocity.x, velocity.angularVelocity.y,
                                   velocity.angularVelocity.z};
      }
    }
  };

  for (size_t hand = 0; hand < controllers.size(); ++hand)
  {
    const XrPath hand_path = m_input_hand_paths[hand];
    auto& controller = controllers[hand];

    bool action_seen = false;
    const auto get_boolean = [this, hand_path, &action_seen](XrAction action) -> bool {
      XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
      get_info.action = action;
      get_info.subactionPath = hand_path;
      XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
      if (XR_FAILED(xrGetActionStateBoolean(m_session, &get_info, &state)))
        return false;
      action_seen |= (state.isActive == XR_TRUE);
      return state.currentState == XR_TRUE;
    };

    const auto get_float = [this, hand_path, &action_seen](XrAction action) -> float {
      XrActionStateGetInfo get_info{XR_TYPE_ACTION_STATE_GET_INFO};
      get_info.action = action;
      get_info.subactionPath = hand_path;
      XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
      if (XR_FAILED(xrGetActionStateFloat(m_session, &get_info, &state)))
        return 0.0f;
      action_seen |= (state.isActive == XR_TRUE);
      return state.currentState;
    };

    controller.primary_button = get_boolean(m_action_primary_click);
    controller.secondary_button = get_boolean(m_action_secondary_click);
    controller.menu_button = get_boolean(m_action_menu_click);
    controller.thumbstick_button = get_boolean(m_action_thumbstick_click);
    const bool trackpad_click = get_boolean(m_action_trackpad_click);
    controller.trackpad_touch = get_boolean(m_action_trackpad_touch);

    const bool trigger_click = get_boolean(m_action_trigger_click);
    const bool squeeze_click = get_boolean(m_action_squeeze_click);

    controller.trigger_value = std::clamp(get_float(m_action_trigger_value), 0.0f, 1.0f);
    controller.squeeze_value = std::clamp(get_float(m_action_squeeze_value), 0.0f, 1.0f);
    controller.thumbstick_x = std::clamp(get_float(m_action_thumbstick_x), -1.0f, 1.0f);
    controller.thumbstick_y = std::clamp(get_float(m_action_thumbstick_y), -1.0f, 1.0f);
    controller.trackpad_x = std::clamp(get_float(m_action_trackpad_x), -1.0f, 1.0f);
    controller.trackpad_y = std::clamp(get_float(m_action_trackpad_y), -1.0f, 1.0f);
    controller.trackpad_force = std::clamp(get_float(m_action_trackpad_force), 0.0f, 1.0f);
    controller.trackpad_button = trackpad_click || controller.trackpad_force > 0.5f;

    locate_space_state(m_aim_spaces[hand], &controller.aim_pose, nullptr);
    locate_space_state(m_grip_spaces[hand], &controller.grip_pose, &controller.grip_velocity);

    controller.trigger_button = trigger_click || controller.trigger_value > 0.5f;
    controller.squeeze_button = squeeze_click || controller.squeeze_value > 0.5f;
    controller.connected = action_seen || controller.aim_pose.valid || controller.grip_pose.valid;
  }

  // Provide HMD head orientation for IR pointer reference direction.
  // Use left eye orientation as a proxy for head center (negligible difference from averaged).
  Common::VR::OpenXRPoseState head_pose;
  if (m_eye_views[0].pose.orientation.w != 0.0f || m_eye_views[0].pose.orientation.x != 0.0f ||
      m_eye_views[0].pose.orientation.y != 0.0f || m_eye_views[0].pose.orientation.z != 0.0f)
  {
    if (!m_home_set)
    {
      if (m_reference_space_is_stage)
      {
        m_home_position = {0.0f, PRIMEGUN_DEFAULT_STANDING_HEIGHT_M, 0.0f};
      }
      else
      {
        m_home_position.x =
            0.5f * (m_eye_views[0].pose.position.x + m_eye_views[1].pose.position.x);
        m_home_position.y =
            0.5f * (m_eye_views[0].pose.position.y + m_eye_views[1].pose.position.y);
        m_home_position.z =
            0.5f * (m_eye_views[0].pose.position.z + m_eye_views[1].pose.position.z);
      }
      m_home_set = true;
    }

    const auto make_home_relative = [this](Common::VR::OpenXRPoseState* pose) {
      if (!pose || !pose->valid)
        return;

      pose->position[0] -= m_home_position.x;
      pose->position[1] -= m_home_position.y;
      pose->position[2] -= m_home_position.z;
    };

    for (auto& controller : controllers)
    {
      make_home_relative(&controller.aim_pose);
      make_home_relative(&controller.grip_pose);
    }

    head_pose.valid = true;
    head_pose.orientation = {m_eye_views[0].pose.orientation.x,
                             m_eye_views[0].pose.orientation.y,
                             m_eye_views[0].pose.orientation.z,
                             m_eye_views[0].pose.orientation.w};
#if defined(ANDROID)
    head_pose.position = {
        0.5f * (m_eye_views[0].pose.position.x + m_eye_views[1].pose.position.x),
        0.5f * (m_eye_views[0].pose.position.y + m_eye_views[1].pose.position.y),
        0.5f * (m_eye_views[0].pose.position.z + m_eye_views[1].pose.position.z)};
#else
    head_pose.position = {m_eye_views[0].pose.position.x, m_eye_views[0].pose.position.y,
                          m_eye_views[0].pose.position.z};
#endif
    make_home_relative(&head_pose);
  }

  const std::array<float, 3> tracking_origin_position{m_home_position.x, m_home_position.y,
                                                      m_home_position.z};
#if defined(ANDROID)
  Common::VR::OpenXRInputState::SetControllers(controllers, true, head_pose,
                                               tracking_origin_position,
                                               m_tracking_origin_generation);
#else
  Common::VR::OpenXRInputState::SetControllers(controllers, true, head_pose,
                                               tracking_origin_position);
#endif
  UpdateHaptics();
}

bool OpenXRManager::CreateReferenceSpace()
{
  ++m_eye_views_generation;
  XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  space_info.poseInReferenceSpace = {{0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, 0.f}};

#if defined(ANDROID)
  // LOCAL follows Quest's system recenter. The second LOCAL space adds our
  // application origin, shared by eyes, controllers and all submitted layers.
  space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  XR_CHECK(xrCreateReferenceSpace(m_session, &space_info, &m_android_tracking_space));
  XR_CHECK(xrCreateReferenceSpace(m_session, &space_info, &m_reference_space));
  m_reference_space_is_stage = false;
  m_home_set = true;
  m_home_position = {0.0f, 0.0f, 0.0f};
  m_android_recenter_pending = true;
  m_android_reference_changes.clear();
  m_tracking_origin_generation =
      s_android_tracking_origin_generation.fetch_add(1, std::memory_order_relaxed) + 1;
  INFO_LOG_FMT(OPENXR, "OpenXR: Using LOCAL space; waiting for a valid initial facing direction.");
  return true;
#else
  // Prefer stage space so PrimedGun starts from the runtime's play-space origin instead of the
  // first HMD pose seen during boot.
  space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
  XrResult result = xrCreateReferenceSpace(m_session, &space_info, &m_reference_space);
  if (XR_SUCCEEDED(result))
  {
    m_reference_space_is_stage = true;
    m_home_position = {0.0f, PRIMEGUN_DEFAULT_STANDING_HEIGHT_M, 0.0f};
    m_home_set = true;
    INFO_LOG_FMT(OPENXR,
                 "OpenXR: Using stage reference space for PrimedGun play-space origin with default "
                 "startup height {:.2f}m.",
                 PRIMEGUN_DEFAULT_STANDING_HEIGHT_M);
    return true;
  }

  char result_string[XR_MAX_RESULT_STRING_SIZE]{};
  xrResultToString(m_instance, result, result_string);
  WARN_LOG_FMT(OPENXR, "OpenXR: Stage reference space unavailable ({}); falling back to local space.",
               result_string);

  space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  XR_CHECK(xrCreateReferenceSpace(m_session, &space_info, &m_reference_space));
  m_reference_space_is_stage = false;
  m_home_set = false;
  m_home_position = {0.0f, 0.0f, 0.0f};
  return true;
#endif
}

#if defined(ANDROID)
void OpenXRManager::RecenterAndroidReferenceSpace()
{
  if (m_recenter_requested.exchange(false, std::memory_order_acq_rel))
    m_android_recenter_pending = true;
  if (Recenter::ConsumeDueChanges(m_android_reference_changes, m_frame_state.predictedDisplayTime))
    m_android_recenter_pending = true;
  if (!m_android_recenter_pending || m_android_tracking_space == XR_NULL_HANDLE)
    return;

  XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
  locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  locate.displayTime = m_frame_state.predictedDisplayTime;
  locate.space = m_android_tracking_space;
  XrViewState state{XR_TYPE_VIEW_STATE};
  std::array<XrView, 2> views{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
  uint32_t count = 0;
  const XrResult located = xrLocateViews(m_session, &locate, &state,
                                         static_cast<uint32_t>(views.size()), &count, views.data());
  constexpr XrViewStateFlags valid_flags =
      XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
  if (XR_FAILED(located) || count != 2 || (state.viewStateFlags & valid_flags) != valid_flags)
    return;  // Retain the request until the runtime supplies a usable pose.
  const auto origin = Recenter::BuildOrigin(views[0].pose, views[1].pose);
  if (!origin)
    return;

  XrReferenceSpaceCreateInfo info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  info.poseInReferenceSpace = *origin;
  XrSpace new_space = XR_NULL_HANDLE;
  const XrResult result = xrCreateReferenceSpace(m_session, &info, &new_space);
  if (XR_FAILED(result))
  {
    WARN_LOG_FMT(OPENXR, "OpenXR: Could not create recentered LOCAL space ({}); retaining old space.",
                 static_cast<int>(result));
    return;
  }

  // Async submissions copy the space handle. Do not replace/destroy it while
  // the previous frame's xrEndFrame is still using that handle.
  if (m_swapchain && !m_swapchain->WaitForPendingFrameFinalization("before Android recenter"))
  {
    xrDestroySpace(new_space);
    return;
  }
  const XrSpace old_space = m_reference_space;
  m_reference_space = new_space;
  m_home_set = true;
  m_home_position = {0.0f, 0.0f, 0.0f};
  m_android_recenter_pending = false;
  m_eye_views_valid = false;
  m_submitted_eye_views_valid = false;
  ++m_eye_views_generation;
  m_tracking_origin_generation =
      s_android_tracking_origin_generation.fetch_add(1, std::memory_order_relaxed) + 1;
  if (old_space != XR_NULL_HANDLE)
    xrDestroySpace(old_space);
  INFO_LOG_FMT(OPENXR,
               "OpenXR: Recentered LOCAL app space generation={} center=({:.3f},{:.3f},{:.3f}) "
               "yaw_quaternion=({:.4f},{:.4f}).",
               m_tracking_origin_generation, origin->position.x, origin->position.y,
               origin->position.z, origin->orientation.y, origin->orientation.w);
}
#endif

bool OpenXRManager::PollEvents()
{
#if defined(ANDROID)
  // A terminal runtime event must also release the emulation core. Otherwise the Android
  // launcher sees an existing game forever, even after its immersive activity is gone.
  // Queue the existing host stop job; stopping directly here could join this GPU thread.
  Common::ScopeGuard stop_guard([this, was_exiting = m_exit_render_loop] {
    if (m_exit_render_loop && !was_exiting)
      Host_Message(HostMessageID::WMUserStop);
  });
#endif
  XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};

  while (xrPollEvent(m_instance, &event) == XR_SUCCESS)
  {
    switch (event.type)
    {
    case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
    {
      const auto& ev = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
      HandleSessionStateChange(ev.state);
      break;
    }
    case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
      ++m_eye_views_generation;
      WARN_LOG_FMT(OPENXR, "OpenXR: Instance loss pending — stopping VR.");
      m_exit_render_loop = true;
      ResetInputActionsState();
      return false;

    case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
    {
#if defined(ANDROID)
      const auto& ev = *reinterpret_cast<const XrEventDataReferenceSpaceChangePending*>(&event);
      if (ev.session == m_session && ev.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL)
      {
        m_android_reference_changes.push_back(ev.changeTime);
        INFO_LOG_FMT(OPENXR, "OpenXR: LOCAL reference change queued for time {}.", ev.changeTime);
      }
#else
      ++m_eye_views_generation;
      INFO_LOG_FMT(OPENXR, "OpenXR: Reference space change pending.");
#endif
      break;
    }

#if defined(ANDROID)
    case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB:
    {
      const auto& ev = *reinterpret_cast<const XrEventDataDisplayRefreshRateChangedFB*>(&event);
      if (ev.toDisplayRefreshRate > 0.0f)
        m_native_display_period_ms = 1000.0 / ev.toDisplayRefreshRate;
      INFO_LOG_FMT(OPENXR, "OpenXR: Display refresh rate changed to {} Hz.", ev.toDisplayRefreshRate);
      break;
    }
#endif

    default:
      break;
    }

    // Reset for next poll.
    event = {XR_TYPE_EVENT_DATA_BUFFER};
  }

  return !m_exit_render_loop;
}

void OpenXRManager::HandleSessionStateChange(XrSessionState new_state)
{
  ++m_eye_views_generation;
  INFO_LOG_FMT(OPENXR, "OpenXR: Session state → {}", static_cast<int>(new_state));
  m_session_state = new_state;
#if defined(ANDROID)
  if (new_state != XR_SESSION_STATE_FOCUSED)
    ResetInputActionsState();
#endif

  switch (new_state)
  {
  case XR_SESSION_STATE_READY:
  {
    XrSessionBeginInfo begin_info{XR_TYPE_SESSION_BEGIN_INFO};
    begin_info.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    if (XR_SUCCEEDED(xrBeginSession(m_session, &begin_info)))
    {
      m_session_running = true;
#if defined(ANDROID)
      ConfigureAndroidSession();
#endif
      INFO_LOG_FMT(OPENXR, "OpenXR: Session running.");
    }
    else
    {
      ERROR_LOG_FMT(OPENXR, "OpenXR: xrBeginSession failed.");
    }
    break;
  }
  case XR_SESSION_STATE_STOPPING:
    if (m_swapchain)
      m_swapchain->WaitForPendingFrameFinalization("before xrEndSession");
#if defined(ANDROID)
    if (m_performance_metrics_enabled)
    {
      XrPerformanceMetricsStateMETA state{XR_TYPE_PERFORMANCE_METRICS_STATE_META};
      state.enabled = XR_FALSE;
      m_set_performance_metrics(m_session, &state);
      m_performance_metrics_enabled = false;
    }
#endif
    xrEndSession(m_session);
    m_session_running = false;
    ResetInputActionsState();
    INFO_LOG_FMT(OPENXR, "OpenXR: Session stopped.");
    break;

  case XR_SESSION_STATE_LOSS_PENDING:
  case XR_SESSION_STATE_EXITING:
    m_exit_render_loop = true;
    ResetInputActionsState();
    break;

  default:
    break;
  }
}

bool OpenXRManager::WaitFrame()
{
#if defined(ANDROID)
  const uint64_t wait_start_us = m_perf_logging_enabled ? Common::Timer::NowUs() : 0;
  bool waited = false;
  Common::ScopeGuard input_guard([this, &waited] {
    if (!waited || !m_frame_state.shouldRender)
      ResetInputActionsState();
  });
#endif
  XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
  m_frame_state = {XR_TYPE_FRAME_STATE};
  XR_CHECK(xrWaitFrame(m_session, &wait_info, &m_frame_state));
#if defined(ANDROID)
  if (m_perf_logging_enabled)
    m_perf_wait_us += Common::Timer::NowUs() - wait_start_us;
  waited = true;
#else
  UpdateInputActions();
#endif
  return true;
}

bool OpenXRManager::BeginFrame()
{
#if defined(ANDROID)
  const uint64_t pending_start_us = m_perf_logging_enabled ? Common::Timer::NowUs() : 0;
  bool begun = false;
  Common::ScopeGuard input_guard([this, &begun] {
    if (!begun)
      ResetInputActionsState();
  });
#endif
  // Android finalizes the previous frame on Vulkan's submit worker. xrWaitFrame
  // may overlap that work, but beginning a new frame before xrEndFrame completes
  // would discard the previous frame and race its composition layers.
  if (m_swapchain && !m_swapchain->WaitForPendingFrameFinalization("before xrBeginFrame"))
    return false;

#if defined(ANDROID)
  const uint64_t begin_start_us = m_perf_logging_enabled ? Common::Timer::NowUs() : 0;
  if (m_perf_logging_enabled)
    m_perf_pending_us += begin_start_us - pending_start_us;
#endif

  XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
  XR_CHECK(xrBeginFrame(m_session, &begin_info));
#if defined(ANDROID)
  begun = true;
  if (m_perf_logging_enabled)
  {
    m_perf_begin_us += Common::Timer::NowUs() - begin_start_us;
    ++m_perf_frames;
    LogAndroidPerformance();
  }
#endif
  return true;
}

bool OpenXRManager::EndFrame(const std::vector<XrCompositionLayerBaseHeader*>& layers)
{
  XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
  end_info.displayTime = m_frame_state.predictedDisplayTime;
  end_info.environmentBlendMode = GetActiveBlendMode();

  // Only submit layers when the runtime requests rendering; otherwise submit 0 layers
  // (this handles the VISIBLE/SYNCHRONIZED states correctly).
  if (m_frame_state.shouldRender == XR_TRUE)
  {
    end_info.layerCount = static_cast<uint32_t>(layers.size());
    end_info.layers = layers.data();
  }

  auto queue_lock = m_swapchain ? m_swapchain->AcquireGraphicsQueueLock() :
                                 std::unique_lock<std::mutex>{};
  XR_CHECK(xrEndFrame(m_session, &end_info));
  return true;
}

bool OpenXRManager::EndFrameDetached(XrTime display_time,
                                     XrEnvironmentBlendMode environment_blend_mode,
                                     bool should_render,
                                     const std::vector<XrCompositionLayerBaseHeader*>& layers)
{
  XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
  end_info.displayTime = display_time;
  end_info.environmentBlendMode = environment_blend_mode;

  if (should_render)
  {
    end_info.layerCount = static_cast<uint32_t>(layers.size());
    end_info.layers = layers.data();
  }

  auto queue_lock = m_swapchain ? m_swapchain->AcquireGraphicsQueueLock() :
                                 std::unique_lock<std::mutex>{};
  XR_CHECK(xrEndFrame(m_session, &end_info));
  return true;
}

bool OpenXRManager::LocateViews()
{
  ++m_eye_views_generation;
  m_eye_views_valid = false;
#if defined(ANDROID)
  // Publish one coherent snapshot after the eyes and any app-space replacement
  // are settled. The guard also publishes inactive input if locating views fails.
  Common::ScopeGuard input_guard([this] {
    if (!m_eye_views_valid)
      m_submitted_eye_views_valid = false;
    const uint64_t start_us = m_perf_logging_enabled ? Common::Timer::NowUs() : 0;
    UpdateInputActions();
    if (m_perf_logging_enabled)
      m_perf_input_us += Common::Timer::NowUs() - start_us;
  });
  const uint64_t locate_start_us = m_perf_logging_enabled ? Common::Timer::NowUs() : 0;
  Common::ScopeGuard perf_guard([this, locate_start_us] {
    if (m_perf_logging_enabled)
      m_perf_locate_us += Common::Timer::NowUs() - locate_start_us;
  });
  RecenterAndroidReferenceSpace();
#endif
  XrViewLocateInfo locate_info{XR_TYPE_VIEW_LOCATE_INFO};
  locate_info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  locate_info.displayTime = m_frame_state.predictedDisplayTime;
  locate_info.space = m_reference_space;

  XrViewState view_state{XR_TYPE_VIEW_STATE};
  uint32_t view_count = static_cast<uint32_t>(m_views.size());
  m_views.fill({XR_TYPE_VIEW});
  m_eye_views_valid = false;

  XR_CHECK(xrLocateViews(m_session, &locate_info, &view_state,
                          view_count, &view_count, m_views.data()));

  constexpr XrViewStateFlags required_flags =
      XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT;
  m_eye_views_valid = view_count >= 2 && (view_state.viewStateFlags & required_flags) == required_flags;
  if (!m_eye_views_valid)
  {
    WARN_LOG_FMT(OPENXR, "OpenXR: Skipping projection layer because view pose is invalid (flags={:#x}, views={}).",
                 static_cast<unsigned int>(view_state.viewStateFlags), view_count);
    return true;
  }

  for (uint32_t i = 0; i < view_count; ++i)
  {
    m_eye_views[i].pose = m_views[i].pose;
    m_eye_views[i].fov = m_views[i].fov;
  }

#if !defined(ANDROID)
  if (m_recenter_requested.exchange(false, std::memory_order_acq_rel) && view_count >= 2)
  {
    const float center_x = 0.5f * (m_eye_views[0].pose.position.x + m_eye_views[1].pose.position.x);
    const float center_z = 0.5f * (m_eye_views[0].pose.position.z + m_eye_views[1].pose.position.z);
    const float old_x = m_home_set ? m_home_position.x : (m_reference_space_is_stage ? 0.0f : center_x);
    const float old_z = m_home_set ? m_home_position.z : (m_reference_space_is_stage ? 0.0f : center_z);
    m_home_position.y = 0.5f * (m_eye_views[0].pose.position.y + m_eye_views[1].pose.position.y);
    m_home_position.x = old_x;
    m_home_position.z = old_z;
    m_home_set = true;
    INFO_LOG_FMT(OPENXR, "OpenXR: Recentered home height only to ({:.4f},{:.4f},{:.4f})",
                 m_home_position.x, m_home_position.y, m_home_position.z);
  }
#endif

  static uint64_t s_locate_log_counter = 0;
  if ((++s_locate_log_counter % 120) == 1 && view_count >= 2)
  {
    const auto& e0 = m_eye_views[0];
    const auto& e1 = m_eye_views[1];
    const float hx = 0.5f * (e0.pose.position.x + e1.pose.position.x);
    const float hy = 0.5f * (e0.pose.position.y + e1.pose.position.y);
    const float hz = 0.5f * (e0.pose.position.z + e1.pose.position.z);
    const float dx = e1.pose.position.x - e0.pose.position.x;
    const float dy = e1.pose.position.y - e0.pose.position.y;
    const float dz = e1.pose.position.z - e0.pose.position.z;
    const float ipd_m = std::sqrt(dx * dx + dy * dy + dz * dz);
    const EulerDeg euler = QuaternionToEulerDeg(e0.pose.orientation);

    INFO_LOG_FMT(
        VIDEO,
        "OpenXR DBG locate #{}: eye0 pos=({:.4f},{:.4f},{:.4f}) quat=({:.5f},{:.5f},{:.5f},{:.5f}) "
        "eye1 pos=({:.4f},{:.4f},{:.4f}) head_center=({:.4f},{:.4f},{:.4f}) ipd={:.4f}m "
        "ypr=({:.2f},{:.2f},{:.2f})",
        s_locate_log_counter, e0.pose.position.x, e0.pose.position.y, e0.pose.position.z,
        e0.pose.orientation.x, e0.pose.orientation.y, e0.pose.orientation.z, e0.pose.orientation.w,
        e1.pose.position.x, e1.pose.position.y, e1.pose.position.z, hx, hy, hz, ipd_m, euler.yaw,
        euler.pitch, euler.roll);
  }

  return true;
}

void OpenXRManager::RecordRenderedEyeViews()
{
  m_submitted_eye_views = m_eye_views;
  m_submitted_eye_views_valid = m_eye_views_valid;
}

bool OpenXRManager::IsExtensionEnabled(const char* extension_name) const
{
  if (!extension_name || extension_name[0] == '\0')
    return false;

  const std::string_view wanted(extension_name);
  return std::any_of(m_enabled_extensions.begin(), m_enabled_extensions.end(),
                     [wanted](const std::string& extension) { return extension == wanted; });
}

bool OpenXRManager::ShouldUseVulkanLegacyProjectionFallback() const
{
  return false;
}

bool OpenXRManager::IsQuestOrVirtualDesktopRuntime() const
{
  std::string runtime = m_runtime_name;
  std::transform(runtime.begin(), runtime.end(), runtime.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });

  return runtime.find("quest") != std::string::npos ||
         runtime.find("oculus") != std::string::npos ||
         runtime.find("meta") != std::string::npos ||
         runtime.find("virtual desktop") != std::string::npos;
}

void OpenXRManager::RequestRecenter()
{
  m_recenter_requested.store(true, std::memory_order_release);
}

void OpenXRManager::GetEyeProjectionRows(
    float units_per_meter,
    std::array<std::array<float, 4>, 4>& out_proj_rows,
    std::array<std::array<float, 4>, 2>& out_z_rows) const
{
  static uint64_t s_proj_log_counter = 0;
  ++s_proj_log_counter;
  const bool do_log = false;
  const float s = std::max(units_per_meter, 0.0001f);
  constexpr float DEG_TO_RAD = 0.01745329252f;
  const float lean_back_rad = g_ActiveConfig.vr_lean_back_angle * DEG_TO_RAD;
  // Positive UI values should move the camera forward.
  // In this projection path, decreasing eye-space Z corresponds to moving forward.
  const float camera_forward_units = -g_ActiveConfig.vr_camera_forward * s;

  if (!m_home_set)
  {
    if (m_reference_space_is_stage)
    {
      m_home_position = {0.0f, PRIMEGUN_DEFAULT_STANDING_HEIGHT_M, 0.0f};
    }
    else
    {
      m_home_position.x =
          0.5f * (m_eye_views[0].pose.position.x + m_eye_views[1].pose.position.x);
      m_home_position.y =
          0.5f * (m_eye_views[0].pose.position.y + m_eye_views[1].pose.position.y);
      m_home_position.z =
          0.5f * (m_eye_views[0].pose.position.z + m_eye_views[1].pose.position.z);
    }
    m_home_set = true;
    INFO_LOG_FMT(OPENXR, "OpenXR: Home position set to ({:.4f},{:.4f},{:.4f})",
                 m_home_position.x, m_home_position.y, m_home_position.z);
  }

  for (uint32_t eye = 0; eye < 2; ++eye)
  {
    const XrFovf& fov = m_eye_views[eye].fov;
    const XrQuaternionf& q_xr = m_eye_views[eye].pose.orientation;
    const XrVector3f& eye_pos_xr = m_eye_views[eye].pose.position;
    XrQuaternionf q = {-q_xr.x, -q_xr.y, -q_xr.z, q_xr.w};
    if (lean_back_rad != 0.0f)
    {
      const float half_angle = 0.5f * lean_back_rad;
      const XrQuaternionf lean_back_quat = {std::sin(half_angle), 0.0f, 0.0f,
                                            std::cos(half_angle)};
      q = MultiplyQuaternions(q, lean_back_quat);
    }

    // --- Quaternion to 3x3 rotation matrix R ---
    // R transforms from eye-local frame to reference space (standard quaternion convention).
    // Columns of R are the eye's local axes expressed in reference-space coordinates.
    // To go reference→local we use R^T, but it's never needed explicitly: see below.
    const float x2 = 2.0f * q.x * q.x, y2 = 2.0f * q.y * q.y, z2 = 2.0f * q.z * q.z;
    const float xy = 2.0f * q.x * q.y, xz = 2.0f * q.x * q.z, yz = 2.0f * q.y * q.z;
    const float wx = 2.0f * q.w * q.x, wy = 2.0f * q.w * q.y, wz = 2.0f * q.w * q.z;

    // R rows (R[row][col]):
    // R[0] = { 1-y2-z2,  xy+wz,   xz-wy  }
    // R[1] = { xy-wz,    1-x2-z2, yz+wx   }
    // R[2] = { xz+wy,    yz-wx,   1-x2-y2 }
    //
    // R^T maps reference-space positions into eye-local coords:
    //   eye_local = R^T * (viewPos - eye_pos_game)
    //
    // clip_x = P_row0 · eye_local = P_row0 · R^T · d
    //        = Σ_j d_j · (Σ_i P_i · R[j][i])
    //        = (R * P_col0) · d
    //
    // So combined_row = R * proj_col  (NOT R^T * proj_col — that would be wrong).

    // R matrix elements (row, col)
    const float r00 = 1.0f - y2 - z2, r01 = xy + wz, r02 = xz - wy;
    const float r10 = xy - wz, r11 = 1.0f - x2 - z2, r12 = yz + wx;
    const float r20 = xz + wy, r21 = yz - wx, r22 = 1.0f - x2 - y2;

    // --- Asymmetric projection from FOV tangent angles ---
    const float tanL = tanf(fov.angleLeft);   // negative
    const float tanR_val = tanf(fov.angleRight);  // positive
    const float tanU = tanf(fov.angleUp);     // positive
    const float tanD = tanf(fov.angleDown);   // negative

    const float inv_w = 1.0f / (tanR_val - tanL);
    const float inv_h = 1.0f / (tanU - tanD);

    // Raw projection rows (before head rotation):
    //   proj_row0 = { 2*inv_w,  0,        (tanR+tanL)*inv_w }
    //   proj_row1 = { 0,        2*inv_h,  (tanU+tanD)*inv_h }
    const float p0x = 2.0f * inv_w;
    const float p0z = (tanR_val + tanL) * inv_w;
    const float p1y = 2.0f * inv_h;
    const float p1z = (tanU + tanD) * inv_h;

    // --- Bake rotation: combined = R * proj_row ---
    // combined_row0 = R * {p0x, 0, p0z}
    const float c0x = r00 * p0x + r02 * p0z;
    const float c0y = r10 * p0x + r12 * p0z;
    const float c0z = r20 * p0x + r22 * p0z;

    // combined_row1 = R * {0, p1y, p1z}
    const float c1x = r01 * p1y + r02 * p1z;
    const float c1y = r11 * p1y + r12 * p1z;
    const float c1z = r21 * p1y + r22 * p1z;

    // Eye position relative to home, in game units.
    // Includes both IPD offset (per-eye) and head positional tracking (shared).
    float ex = (eye_pos_xr.x - m_home_position.x) * s;
    float ey = (eye_pos_xr.y - m_home_position.y) * s;
    float ez = (eye_pos_xr.z - m_home_position.z) * s;
    if (camera_forward_units != 0.0f)
    {
      // Apply a fixed camera-space forward offset without coupling it to current
      // head orientation. This keeps head tracking anchored like freelook offsets.
      ez += camera_forward_units;
    }

    // W component: -dot(combined_xyz, eye_pos) using the ROTATED projection rows.
    // This gives the correct full view transform: P · R^T · (viewPos - eye_pos).
    const float c0w = -(c0x * ex + c0y * ey + c0z * ez);
    const float c1w = -(c1x * ex + c1y * ey + c1z * ez);

    out_proj_rows[eye * 2 + 0] = {c0x, c0y, c0z, c0w};
    out_proj_rows[eye * 2 + 1] = {c1x, c1y, c1z, c1w};

    // --- Z-axis row for depth/w computation ---
    // z_eye = (R^T * (viewPos - eye_pos)).z = dot(R_col2, viewPos - eye_pos)
    // R_col2 = {r02, r12, r22}
    const float zw = -(r02 * ex + r12 * ey + r22 * ez);
    out_z_rows[eye] = {r02, r12, r22, zw};

    if (do_log)
    {
      INFO_LOG_FMT(
          VIDEO,
          "OpenXR DBG proj #{} eye{}: upm={:.3f} eye_pos_m=({:.4f},{:.4f},{:.4f}) eye_rel_u=({:.3f},"
          "{:.3f},{:.3f}) row0=({:.5f},{:.5f},{:.5f},{:.5f}) row1=({:.5f},{:.5f},{:.5f},{:.5f}) "
          "zrow=({:.5f},{:.5f},{:.5f},{:.5f})",
          s_proj_log_counter, eye, s, eye_pos_xr.x, eye_pos_xr.y, eye_pos_xr.z, ex, ey, ez,
          c0x, c0y, c0z, c0w, c1x, c1y, c1z, c1w, r02, r12, r22, zw);
    }
  }
}

void OpenXRManager::GetRawEyeProjectionRows(
    float units_per_meter,
    std::array<std::array<float, 4>, 4>& out_proj_rows) const
{
  const float s = std::max(units_per_meter, 0.0001f);

  // Compute head center (average of both eyes) for extracting per-eye local offset.
  const float hx = 0.5f * (m_eye_views[0].pose.position.x + m_eye_views[1].pose.position.x);
  const float hy = 0.5f * (m_eye_views[0].pose.position.y + m_eye_views[1].pose.position.y);
  const float hz = 0.5f * (m_eye_views[0].pose.position.z + m_eye_views[1].pose.position.z);

  // Get head rotation to compute R^T * (eye_world - head_center) = local eye offset.
  const XrQuaternionf& q_xr = m_eye_views[0].pose.orientation;
  const XrQuaternionf q = {-q_xr.x, -q_xr.y, -q_xr.z, q_xr.w};
  const float x2 = 2.0f * q.x * q.x, y2 = 2.0f * q.y * q.y, z2 = 2.0f * q.z * q.z;
  const float xy = 2.0f * q.x * q.y, xz = 2.0f * q.x * q.z, yz = 2.0f * q.y * q.z;
  const float wx = 2.0f * q.w * q.x, wy = 2.0f * q.w * q.y, wz = 2.0f * q.w * q.z;

  // R^T rows (= R columns) for transforming world offset to head-local space
  const float rt00 = 1.0f - y2 - z2, rt01 = xy - wz, rt02 = xz + wy;
  const float rt10 = xy + wz, rt11 = 1.0f - x2 - z2, rt12 = yz - wx;

  for (uint32_t eye = 0; eye < 2; ++eye)
  {
    const XrFovf& fov = m_eye_views[eye].fov;
    const XrVector3f& eye_pos_xr = m_eye_views[eye].pose.position;

    // --- Asymmetric projection from FOV tangent angles (same as rotated version) ---
    const float tanL = tanf(fov.angleLeft);
    const float tanR_val = tanf(fov.angleRight);
    const float tanU = tanf(fov.angleUp);
    const float tanD = tanf(fov.angleDown);

    const float inv_w = 1.0f / (tanR_val - tanL);
    const float inv_h = 1.0f / (tanU - tanD);

    // Raw (unrotated) projection rows
    const float p0x = 2.0f * inv_w;
    const float p0z = (tanR_val + tanL) * inv_w;
    const float p1y = 2.0f * inv_h;
    const float p1z = (tanU + tanD) * inv_h;

    // Per-eye offset in head-local space: R^T * (eye_world - head_center)
    const float dx = eye_pos_xr.x - hx;
    const float dy = eye_pos_xr.y - hy;
    const float dz = eye_pos_xr.z - hz;

    const float local_ex = (rt00 * dx + rt01 * dy + rt02 * dz) * s;
    const float local_ey = (rt10 * dx + rt11 * dy + rt12 * dz) * s;

    // W component using raw (unrotated) P rows and head-local eye offset
    const float pw0 = -(p0x * local_ex + p0z * 0.0f);  // p0z * local_ez ≈ 0
    const float pw1 = -(p1y * local_ey + p1z * 0.0f);  // p1z * local_ez ≈ 0

    out_proj_rows[eye * 2 + 0] = {p0x, 0.0f, p0z, pw0};
    out_proj_rows[eye * 2 + 1] = {0.0f, p1y, p1z, pw1};
  }
}

bool OpenXRManager::GetLegacyViewMatrix(float units_per_meter,
                                        Common::Matrix44* out_view_matrix) const
{
  if (!out_view_matrix)
    return false;

  *out_view_matrix = Common::Matrix44::Identity();
  return false;
}

void OpenXRManager::GetLegacyProjectionAdjustments(
    float units_per_meter, float game_projection_x_scale, float game_projection_x_offset,
    float game_projection_y_scale, float game_projection_y_offset,
    std::array<std::array<float, 4>, 2>& out_x_rows,
    std::array<std::array<float, 4>, 2>& out_y_rows) const
{
  out_x_rows = {};
  out_y_rows = {};
}

}  // namespace VR

#endif  // ENABLE_VR
