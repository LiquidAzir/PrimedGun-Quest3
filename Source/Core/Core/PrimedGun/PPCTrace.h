// Copyright 2026 PrimedGun
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "Common/CommonTypes.h"

namespace PrimedGun::PPCTrace
{
u32* GetActiveFlagAddress();
bool IsActive();
bool Toggle();
void Start();
void Stop();
void TraceBlockFromJit(u32 pc);
}
