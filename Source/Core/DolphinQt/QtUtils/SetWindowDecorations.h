// Copyright 2023 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

class QApplication;

namespace QtUtils
{
// Applies platform-specific dark window decoration hints for the active Dolphin style.
void InstallWindowDecorationFilter(QApplication*);
}  // namespace QtUtils
