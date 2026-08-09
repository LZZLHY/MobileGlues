// MobileGlues - gl/log.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#ifndef MOBILEGLUES_LOG_H

#include "../includes.h"

#define FORCE_SYNC_WITH_LOG_FILE 0

#define GLOBAL_DEBUG 0

#define LOG_CALLED_FUNCS 0

#ifdef __cplusplus
extern "C"
{
#endif

    const char* glEnumToString(GLenum e);

#ifdef __cplusplus
}
#endif

#ifndef __ANDROID__
// Define a stub for __android_log_print if not on Android
#define ANDROID_LOG_UNKNOWN 0
#define ANDROID_LOG_DEFAULT 1
#define ANDROID_LOG_VERBOSE 2
#define ANDROID_LOG_DEBUG 3
#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_WARN 5
#define ANDROID_LOG_ERROR 6
#define ANDROID_LOG_FATAL 7
#define ANDROID_LOG_SILENT 8

typedef int android_LogPriority;

int __android_log_print(int prio, const char* tag, const char* fmt, ...);
#endif

// REMOVED 2026-08-07: the per-entry-point scope timer that used to hang off LOG().
//
// It answered its question and was then pure cost. What it established, kept because it is the reason
// not to add it back casually: with the u_SectionTimeInfo upload cost gone, only 14-18% of a frame's
// CPU time was inside this library at all in the steady state - about 4.9 ms of a 27 ms frame - and the
// rest is the application's own code. The frame-rate ceiling is not here, and an instrument on every
// entry point cannot find something that is not in the entry points.
//
// If it is ever needed again, the shape that worked was: a small RAII object appended to this macro and
// to NATIVE_FUNCTION_HEAD in gles/loader.h, a shared thread_local depth counter so nested entry points
// count once, and an inline flag test so a release build pays a load and a branch rather than two
// out-of-line calls. See docs/ohos/RENDER-ADAPTATION.md 6.11.
#if GLOBAL_DEBUG_FORCE_OFF
#define LOG()                                                                                                          \
    {}
#define LOG_D(...)                                                                                                     \
    {}
#define LOG_D_N(...)                                                                                                   \
    {}
#define LOG_W(...)                                                                                                     \
    {}
#define LOG_E(...)                                                                                                     \
    {}
#define LOG_F(...)                                                                                                     \
    {}
#else
#if PROFILING
#define LOG()                                                                                                          \
    perfetto::StaticString _FUNC_NAME_ = __func__;                                                                     \
    TRACE_EVENT("glcalls", _FUNC_NAME_);
#elif LOG_CALLED_FUNCS
#define LOG()                                                                                                          \
    if (DEBUG || GLOBAL_DEBUG) {                                                                                       \
        __android_log_print(ANDROID_LOG_DEBUG, RENDERERNAME, "Use function: %s", __FUNCTION__);                        \
        printf("Use function: %s\n", __FUNCTION__);                                                                    \
        write_log("Use function: %s\n", __FUNCTION__);                                                                 \
    }                                                                                                                  \
    log_unique_function(__FUNCTION__);
void log_unique_function(const char* func_name);
#else
#define LOG()                                                                                                          \
    if (DEBUG || GLOBAL_DEBUG) {                                                                                       \
        __android_log_print(ANDROID_LOG_DEBUG, RENDERERNAME, "\nUse function: %s", __FUNCTION__);                      \
        printf("\nUse function: %s\n", __FUNCTION__);                                                                  \
        write_log("\nUse function: %s\n", __FUNCTION__);                                                               \
    }
#endif

#define LOG_D(...)                                                                                                     \
    if (DEBUG || GLOBAL_DEBUG) {                                                                                       \
        __android_log_print(ANDROID_LOG_DEBUG, RENDERERNAME, __VA_ARGS__);                                             \
        printf(__VA_ARGS__);                                                                                           \
        printf("\n");                                                                                                  \
        write_log(__VA_ARGS__);                                                                                        \
    }
#define LOG_D_N(...)                                                                                                   \
    if (DEBUG || GLOBAL_DEBUG) {                                                                                       \
        __android_log_print(ANDROID_LOG_DEBUG, RENDERERNAME, __VA_ARGS__);                                             \
        printf(__VA_ARGS__);                                                                                           \
        write_log_n(__VA_ARGS__);                                                                                      \
    }
#define LOG_W(...)                                                                                                     \
    if (DEBUG || GLOBAL_DEBUG) {                                                                                       \
        __android_log_print(ANDROID_LOG_WARN, RENDERERNAME, __VA_ARGS__);                                              \
        printf(__VA_ARGS__);                                                                                           \
        printf("\n");                                                                                                  \
        write_log(__VA_ARGS__);                                                                                        \
    }
#define LOG_E(...)                                                                                                     \
    if (DEBUG || GLOBAL_DEBUG) {                                                                                       \
        __android_log_print(ANDROID_LOG_ERROR, RENDERERNAME, __VA_ARGS__);                                             \
        printf(__VA_ARGS__);                                                                                           \
        printf("\n");                                                                                                  \
        write_log(__VA_ARGS__);                                                                                        \
    }
#define LOG_F(...)                                                                                                     \
    if (DEBUG || GLOBAL_DEBUG) {                                                                                       \
        __android_log_print(ANDROID_LOG_FATAL, RENDERERNAME, __VA_ARGS__);                                             \
        printf(__VA_ARGS__);                                                                                           \
        printf("\n");                                                                                                  \
        write_log(__VA_ARGS__);                                                                                        \
    }
#endif

#define LOG_V(...)                                                                                                     \
    {                                                                                                                  \
        __android_log_print(ANDROID_LOG_VERBOSE, RENDERERNAME, __VA_ARGS__);                                           \
        printf(__VA_ARGS__);                                                                                           \
        printf("\n");                                                                                                  \
        write_log(__VA_ARGS__);                                                                                        \
    }
#define LOG_I(...)                                                                                                     \
    {                                                                                                                  \
        __android_log_print(ANDROID_LOG_INFO, RENDERERNAME, __VA_ARGS__);                                              \
        printf(__VA_ARGS__);                                                                                           \
        printf("\n");                                                                                                  \
        write_log(__VA_ARGS__);                                                                                        \
    }
#define LOG_W_FORCE(...)                                                                                               \
    {                                                                                                                  \
        __android_log_print(ANDROID_LOG_WARN, RENDERERNAME, __VA_ARGS__);                                              \
        printf(__VA_ARGS__);                                                                                           \
        printf("\n");                                                                                                  \
        write_log(__VA_ARGS__);                                                                                        \
    }

#define MOBILEGLUES_LOG_H

#endif // MOBILEGLUES_LOG_H
