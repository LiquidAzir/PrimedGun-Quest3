// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace VR
{
struct EyeProjectionRows
{
  std::array<std::array<float, 4>, 4> eye{};
  std::array<std::array<float, 4>, 2> depth{};
  std::array<std::array<float, 4>, 4> head{};
};

// Render-thread cache of raw OpenXR rows. It deliberately excludes per-draw
// freelook, depth, screen placement, and HUD scaling. View generation includes
// pose/FOV changes, recentering, and session/reference-space resets.
class EyeProjectionCache
{
public:
  struct Parameters
  {
    float units_per_meter;
    float lean_back_angle;
    float camera_forward;

    bool operator==(const Parameters&) const = default;
  };

  void Invalidate()
  {
    m_owner = nullptr;
    m_size = 0;
    m_next = 0;
  }

  // The returned reference remains valid until the next lookup/invalidation.
  // Small bounded storage handles alternating world and HUD UPM values without
  // allocations. Eviction only recomputes rows; it never changes their source.
  template <typename Build>
  const EyeProjectionRows& Get(const void* owner, std::uint64_t generation,
                               const Parameters& parameters, Build&& build)
  {
    if (m_owner != owner || m_generation != generation)
    {
      Invalidate();
      m_owner = owner;
      m_generation = generation;
    }
    for (std::size_t i = 0; i < m_size; ++i)
    {
      if (m_entries[i].parameters == parameters)
        return m_entries[i].rows;
    }

    Entry& entry = m_entries[m_next];
    entry.parameters = parameters;
    build(entry.rows);
    if (m_size < m_entries.size())
      ++m_size;
    m_next = (m_next + 1) % m_entries.size();
    return entry.rows;
  }

private:
  struct Entry
  {
    Parameters parameters{};
    EyeProjectionRows rows;
  };
  std::array<Entry, 8> m_entries{};
  const void* m_owner = nullptr;
  std::uint64_t m_generation = 0;
  std::size_t m_size = 0;
  std::size_t m_next = 0;
};
}  // namespace VR
