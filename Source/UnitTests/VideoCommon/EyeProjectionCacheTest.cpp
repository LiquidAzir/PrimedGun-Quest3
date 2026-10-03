// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <limits>

#include <gtest/gtest.h>

#include "VideoCommon/VR/EyeProjectionCache.h"

namespace
{
class EyeProjectionCacheTest : public testing::Test
{
protected:
  VR::EyeProjectionRows Get(VR::EyeProjectionCache::Parameters parameters)
  {
    return cache.Get(owner, generation, parameters, [&](VR::EyeProjectionRows& rows) {
      ++builds;
      // Distinct row data exposes a hit on any stale input or incomplete copy.
      for (std::size_t i = 0; i < rows.eye.size(); ++i)
      {
        rows.eye[i] = {parameters.units_per_meter, parameters.lean_back_angle,
                       parameters.camera_forward, static_cast<float>(generation + i)};
        rows.head[i] = {parameters.units_per_meter, static_cast<float>(generation),
                        static_cast<float>(i), parameters.camera_forward};
      }
      rows.depth = {rows.eye[0], rows.eye[2]};
    });
  }

  VR::EyeProjectionCache cache;
  int owner_storage[2]{};
  const void* owner = &owner_storage[0];
  std::uint64_t generation = 1;
  unsigned int builds = 0;
};

TEST_F(EyeProjectionCacheTest, ReusesAllRowsForRepeatedDraws)
{
  const auto expected = Get({10.0f, 0.0f, 0.0f});
  for (int draw = 0; draw < 1000; ++draw)
  {
    const auto actual = Get({10.0f, 0.0f, 0.0f});
    EXPECT_EQ(actual.eye, expected.eye);
    EXPECT_EQ(actual.depth, expected.depth);
    EXPECT_EQ(actual.head, expected.head);
  }
  EXPECT_EQ(builds, 1u);
}

TEST_F(EyeProjectionCacheTest, ChangesEveryProjectionInputIndependently)
{
  Get({10.0f, 0.0f, 0.0f});
  EXPECT_EQ(Get({20.0f, 0.0f, 0.0f}).eye[0][0], 20.0f);
  EXPECT_EQ(Get({20.0f, 15.0f, 0.0f}).eye[0][1], 15.0f);
  EXPECT_EQ(Get({20.0f, 15.0f, -1.0f}).eye[0][2], -1.0f);
  EXPECT_EQ(builds, 4u);
  EXPECT_EQ(Get({10.0f, 0.0f, 0.0f}).eye[0][0], 10.0f);
  EXPECT_EQ(builds, 4u);
}

TEST_F(EyeProjectionCacheTest, AdvancesPoseAndResetsRuntimeIdentity)
{
  Get({10.0f, 0.0f, 0.0f});
  ++generation;  // New LocateViews, including FOV/recenter/validity changes.
  EXPECT_EQ(Get({10.0f, 0.0f, 0.0f}).eye[0][3], 2.0f);
  EXPECT_EQ(builds, 2u);
  owner = &owner_storage[1];
  Get({10.0f, 0.0f, 0.0f});
  EXPECT_EQ(builds, 3u);
  cache.Invalidate();  // Init, state load, or explicit invalidation.
  Get({10.0f, 0.0f, 0.0f});
  EXPECT_EQ(builds, 4u);
}

TEST_F(EyeProjectionCacheTest, AlternatingWorldAndHudScalesStayCurrent)
{
  for (int frame = 0; frame < 3; ++frame)
  {
    ++generation;
    for (int draw = 0; draw < 160; ++draw)
    {
      const float upm = 10.0f * static_cast<float>(1 + draw % 8);
      const auto rows = Get({upm, 5.0f, 0.5f});
      EXPECT_EQ(rows.eye[0][0], upm);
      EXPECT_EQ(rows.eye[0][3], static_cast<float>(generation));
    }
    EXPECT_EQ(builds, 8u * (frame + 1));
  }
}

TEST_F(EyeProjectionCacheTest, EvictionRecomputesWithoutReturningAnotherDraw)
{
  for (int index = 0; index < 9; ++index)
    Get({static_cast<float>(index), 0.0f, 0.0f});
  EXPECT_EQ(Get({0.0f, 0.0f, 0.0f}).eye[0][0], 0.0f);
  EXPECT_EQ(builds, 10u);
}

TEST_F(EyeProjectionCacheTest, InvalidNumericInputsCannotHitUnrelatedCachedRows)
{
  Get({1.0f, 0.0f, 0.0f});
  const float nan = std::numeric_limits<float>::quiet_NaN();
  Get({1.0f, nan, 0.0f});
  Get({1.0f, nan, 0.0f});
  EXPECT_EQ(builds, 3u);
  EXPECT_EQ(Get({1.0f, 0.0f, 0.0f}).eye[0][1], 0.0f);
  EXPECT_EQ(builds, 3u);
}
}  // namespace
