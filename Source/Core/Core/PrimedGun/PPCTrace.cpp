// Copyright 2026 PrimedGun
// SPDX-License-Identifier: GPL-2.0-or-later

#include "Core/PrimedGun/PPCTrace.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fmt/chrono.h>
#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/Logging/Log.h"
#include "Common/CommonPaths.h"
#include "Core/Core.h"

namespace PrimedGun::PPCTrace
{
namespace
{
constexpr std::size_t MAX_TRACE_ENTRIES = 1'000'000;
constexpr std::size_t MAX_FOCUSED_TRACE_ENTRIES = 200'000;
constexpr auto TRACE_DURATION = std::chrono::seconds{10};
constexpr std::size_t SAMPLE_STRIDE = 4096;

struct TraceRange
{
  u32 start = 0;
  u32 end = 0;
  const char* name = "";
};

struct TraceEntry
{
  u32 pc = 0;
};

constexpr std::array<TraceRange, 4> SCAN_RETICLE_RANGES = {{
    {0x800BD700, 0x800BD980, "CCompoundTargetReticle::DrawNextLockOnGroup"},
    {0x800BE100, 0x800BE3D0, "CCompoundTargetReticle::DrawCurrLockOnGroup"},
    {0x80111E00, 0x80112180, "CPlayerVisor::DrawScanObjectIndicators"},
    {0x80112400, 0x80112880, "CPlayerVisor::UpdateScanObjectIndicators"},
}};

alignas(4) u32 s_active_flag = 0;
std::atomic_ref<u32> s_active{s_active_flag};
std::atomic<bool> s_flushing{false};
std::atomic<std::size_t> s_total_seen{0};
std::atomic<std::size_t> s_recorded_count{0};
std::atomic<std::size_t> s_focused_recorded_count{0};
std::array<TraceEntry, MAX_TRACE_ENTRIES> s_entries;
std::array<TraceEntry, MAX_FOCUSED_TRACE_ENTRIES> s_focused_entries;
std::chrono::steady_clock::time_point s_started_steady_time;
std::chrono::steady_clock::time_point s_end_time;
std::chrono::system_clock::time_point s_started_wall_time;

std::string MakeTracePath()
{
  const std::string root = File::GetUserPath(D_DUMPDEBUG_IDX) + "PrimedGun" DIR_SEP;
  File::CreateFullPath(root);

  const auto now = std::chrono::system_clock::now();
  return fmt::format("{}ppc_block_trace_{:%Y%m%d_%H%M%S}.txt", root, now);
}

void Flush(const char* reason)
{
  if (s_flushing.exchange(true))
    return;

  s_active.store(0, std::memory_order_release);
  std::this_thread::sleep_for(std::chrono::milliseconds{2});

  const std::size_t recorded =
      std::min(s_recorded_count.load(std::memory_order_acquire), MAX_TRACE_ENTRIES);
  const std::size_t focused_recorded = std::min(
      s_focused_recorded_count.load(std::memory_order_acquire), MAX_FOCUSED_TRACE_ENTRIES);
  const std::size_t total_seen = s_total_seen.load(std::memory_order_acquire);
  const std::string path = MakeTracePath();
  File::IOFile file(path, "w");
  if (!file)
  {
    ERROR_LOG_FMT(POWERPC, "PrimedGun PPC trace failed to open {}", path);
    s_flushing.store(false);
    return;
  }

  std::unordered_map<u32, std::size_t> counts;
  counts.reserve(recorded / 4 + 1);
  for (std::size_t i = 0; i < recorded; ++i)
    ++counts[s_entries[i].pc];

  std::vector<std::pair<u32, std::size_t>> sorted_counts(counts.begin(), counts.end());
  std::sort(sorted_counts.begin(), sorted_counts.end(), [](const auto& a, const auto& b) {
    if (a.second != b.second)
      return a.second > b.second;
    return a.first < b.first;
  });

  std::fprintf(file.GetHandle(), "PrimedGun PPC JIT block trace\n");
  std::fprintf(file.GetHandle(), "Reason: %s\n", reason);
  std::fprintf(file.GetHandle(), "Started: %s\n",
               fmt::format("{:%Y-%m-%d %H:%M:%S}", s_started_wall_time).c_str());
  std::fprintf(file.GetHandle(), "Elapsed seconds: %.3f\n",
               std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                             s_started_steady_time)
                   .count());
  std::fprintf(file.GetHandle(), "Sample stride: every %zu JIT blocks\n", SAMPLE_STRIDE);
  std::fprintf(file.GetHandle(), "Total blocks seen: %zu\n", total_seen);
  std::fprintf(file.GetHandle(), "Entries recorded: %zu\n", recorded);
  std::fprintf(file.GetHandle(), "Focused scan/reticle entries recorded: %zu\n",
               focused_recorded);
  std::fprintf(file.GetHandle(), "Entries dropped after cap: %zu\n",
               s_recorded_count.load(std::memory_order_acquire) > MAX_TRACE_ENTRIES ?
                   s_recorded_count.load(std::memory_order_acquire) - MAX_TRACE_ENTRIES :
                   0);
  std::fprintf(file.GetHandle(), "Focused entries dropped after cap: %zu\n",
               s_focused_recorded_count.load(std::memory_order_acquire) >
                       MAX_FOCUSED_TRACE_ENTRIES ?
                   s_focused_recorded_count.load(std::memory_order_acquire) -
                       MAX_FOCUSED_TRACE_ENTRIES :
                   0);

  std::fprintf(file.GetHandle(), "\nFocused scan/reticle ranges:\n");
  for (const TraceRange& range : SCAN_RETICLE_RANGES)
    std::fprintf(file.GetHandle(), "%08x-%08x %s\n", range.start, range.end, range.name);
  std::fprintf(file.GetHandle(), "\nHot blocks:\n");
  for (const auto& [pc, count] : sorted_counts)
    std::fprintf(file.GetHandle(), "%08x %zu\n", pc, count);

  std::fprintf(file.GetHandle(), "\nFocused scan/reticle timeline:\n");
  for (std::size_t i = 0; i < focused_recorded; ++i)
  {
    const u32 pc = s_focused_entries[i].pc;
    const char* name = "unknown";
    for (const TraceRange& range : SCAN_RETICLE_RANGES)
    {
      if (pc >= range.start && pc < range.end)
      {
        name = range.name;
        break;
      }
    }
    std::fprintf(file.GetHandle(), "%08x %s\n", pc, name);
  }

  std::fprintf(file.GetHandle(), "\nTimeline:\n");
  for (std::size_t i = 0; i < recorded; ++i)
    std::fprintf(file.GetHandle(), "%08x\n", s_entries[i].pc);

  NOTICE_LOG_FMT(POWERPC, "PrimedGun PPC trace wrote {}", path);
  Core::DisplayMessage(fmt::format("PrimedGun PPC trace wrote {}", path), 5000);
  s_flushing.store(false);
}
}  // namespace

u32* GetActiveFlagAddress()
{
  return &s_active_flag;
}

bool IsActive()
{
  return s_active.load(std::memory_order_acquire) != 0;
}

bool Toggle()
{
  if (IsActive())
  {
    Stop();
    return false;
  }

  Start();
  return true;
}

void Start()
{
  s_active.store(0, std::memory_order_release);
  s_flushing.store(false);
  s_total_seen.store(0, std::memory_order_release);
  s_recorded_count.store(0, std::memory_order_release);
  s_focused_recorded_count.store(0, std::memory_order_release);
  s_started_wall_time = std::chrono::system_clock::now();
  s_started_steady_time = std::chrono::steady_clock::now();
  s_end_time = s_started_steady_time + TRACE_DURATION;
  s_active.store(1, std::memory_order_release);
  NOTICE_LOG_FMT(POWERPC, "PrimedGun PPC trace started");
  Core::DisplayMessage("PrimedGun PPC trace started", 2500);
}

void Stop()
{
  Flush("manual stop");
}

void TraceBlockFromJit(u32 pc)
{
  if (s_active.load(std::memory_order_relaxed) == 0)
    return;

  const std::size_t seen = s_total_seen.fetch_add(1, std::memory_order_relaxed);
  bool focused = false;
  for (const TraceRange& range : SCAN_RETICLE_RANGES)
  {
    if (pc >= range.start && pc < range.end)
    {
      focused = true;
      break;
    }
  }

  if (focused)
  {
    const std::size_t focused_index =
        s_focused_recorded_count.fetch_add(1, std::memory_order_relaxed);
    if (focused_index < MAX_FOCUSED_TRACE_ENTRIES)
      s_focused_entries[focused_index].pc = pc;
  }

  if ((seen % SAMPLE_STRIDE) != 0)
  {
    if ((seen & 0xffff) == 0 && std::chrono::steady_clock::now() >= s_end_time)
      Flush("10 second capture complete");
    return;
  }

  const std::size_t recorded_index = s_recorded_count.fetch_add(1, std::memory_order_relaxed);
  if (recorded_index < MAX_TRACE_ENTRIES)
    s_entries[recorded_index].pc = pc;

  if ((recorded_index & 0x3ff) == 0)
  {
    if (recorded_index >= MAX_TRACE_ENTRIES)
      Flush("entry cap reached");
    else if (std::chrono::steady_clock::now() >= s_end_time)
      Flush("10 second capture complete");
  }
}
}  // namespace PrimedGun::PPCTrace
