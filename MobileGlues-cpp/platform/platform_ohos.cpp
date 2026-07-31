// MobileGlues - platform/platform_ohos.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

// OpenHarmony / HarmonyOS NEXT platform backend.
//
// Routes MobileGlues log records into hilog, which is where every other diagnostic on the device
// already goes. Without this, the translation layer's records existed only in its own log file, so
// a driver problem could not be correlated with the surrounding system and application timeline.

#include "platform.h"

#include <hilog/log.h>

namespace mg::platform {

    namespace {

        // hilog application domains are 16-bit. This one is unused by the launcher so that
        // MobileGlues records can be filtered on their own.
        constexpr unsigned int MG_LOG_DOMAIN = 0xB010;

        ::LogLevel to_hilog_level(LogLevel level) {
            switch (level) {
            case LogLevel::Verbose:
            case LogLevel::Debug:
                return LOG_DEBUG;
            case LogLevel::Info:
                return LOG_INFO;
            case LogLevel::Warn:
                return LOG_WARN;
            case LogLevel::Error:
                return LOG_ERROR;
            case LogLevel::Fatal:
                return LOG_FATAL;
            }
            return LOG_INFO;
        }

    } // namespace

    void log_write(LogLevel level, const char* tag, const char* message) {
        if (!message) return;
        // The message is already formatted, so it is passed as an argument rather than as the
        // format string: hilog would otherwise interpret a literal '%' inside GL strings, shader
        // source or driver messages. "%{public}s" keeps the payload readable in device logs
        // instead of being redacted as <private>.
        OH_LOG_Print(LOG_APP, to_hilog_level(level), MG_LOG_DOMAIN, tag ? tag : "MobileGlues", "%{public}s", message);
    }

    const char* name() {
        return "ohos";
    }

} // namespace mg::platform
