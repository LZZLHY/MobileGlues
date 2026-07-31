// MobileGlues - platform/platform.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#ifndef MOBILEGLUES_PLATFORM_H
#define MOBILEGLUES_PLATFORM_H

// The narrow seam between MobileGlues and the operating system it runs on.
//
// Almost nothing in the translation layer is platform specific: turning desktop OpenGL into
// OpenGL ES is the same work everywhere. What does differ is where a log record goes, how the
// GL libraries are found, and where configuration lives. Android reaches its log through
// __android_log_print, iOS and Linux have historically had no sink at all, and OpenHarmony has
// hilog. Rather than sprinkling those differences across the call sites, each platform provides
// one implementation of this interface and everything else stays platform neutral.
//
// Exactly one platform_*.cpp is compiled, selected by the build.

namespace mg::platform {

    // Log severities, matching the scale MobileGlues already uses at its call sites so that no
    // caller has to be rewritten. Implementations map these onto whatever the platform provides.
    enum class LogLevel {
        Verbose,
        Debug,
        Info,
        Warn,
        Error,
        Fatal,
    };

    // Writes one already formatted record to the platform log sink.
    //
    // Callers own the formatting, so implementations must not interpret the message as a format
    // string. This is deliberate: the ANDROID_LOG_* call sites pass user-controlled text, and a
    // platform that treats it as a format (hilog does) would otherwise misread a literal '%'.
    //
    // Must be safe to call from any thread and before proc_init() has run. A platform without a
    // system log sink implements this as a no-op, which is what iOS and Linux did before this
    // seam existed.
    void log_write(LogLevel level, const char* tag, const char* message);

    // Short, stable platform identifier for diagnostics, e.g. "ohos" or "generic".
    const char* name();

    // Maps the ANDROID_LOG_* integer scale used by MobileGlues' logging macros onto LogLevel,
    // so the compatibility shim in gl/log.cpp does not have to know about either scale.
    LogLevel log_level_from_android_priority(int priority);

} // namespace mg::platform

#endif // MOBILEGLUES_PLATFORM_H
