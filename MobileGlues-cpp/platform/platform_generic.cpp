// MobileGlues - platform/platform_generic.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

// Fallback platform backend for targets without a system log sink of their own.
//
// This preserves the behaviour those targets already had: MobileGlues' own log file and stdout
// still receive everything, and the system log call is simply dropped. Keeping it as an explicit
// backend rather than an #ifdef means adding a platform is adding a file, not editing call sites.

#include "platform.h"

namespace mg::platform {

    void log_write(LogLevel, const char*, const char*) {
        // No system log sink on this platform.
    }

    const char* name() {
        return "generic";
    }

} // namespace mg::platform
