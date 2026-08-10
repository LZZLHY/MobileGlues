// MobileGlues - main.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#include "config/settings.h"
#include "config/stats.h"
#include "egl/egl.h"
#include "egl/loader.h"
#include "gl/envvars.h"
#include "gl/gl.h"
#include "gl/log.h"
#include "gl/mg.h"
#include "gles/loader.h"
#include "includes.h"
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <exception>
#include <mutex>
#include <sys/stat.h>
#include <thread>

#define DEBUG 0

#ifndef __APPLE__
__attribute__((used))
#endif
const char* license = "GNU LGPL-2.1 License";

bool init_config() {
    if (!check_path()) return false;
    config_refresh();
    // One dlopen of this library is one launch. Counting it here, before any
    // rendering work, means a game that crashes on the first frame still counts.
    bump_launch_count();
    return true;
}

void show_license() {
    LOG_V("The Open Source License of MobileGlues: ");
    LOG_V("  %s", license);
}

#if PROFILING

PERFETTO_TRACK_EVENT_STATIC_STORAGE();

void init_perfetto() {
    perfetto::TracingInitArgs args;

    args.backends |= perfetto::kSystemBackend;

    perfetto::Tracing::Initialize(args);
    perfetto::TrackEvent::Register();
}
#endif

namespace {

std::mutex g_init_mutex;
std::condition_variable g_init_cv;
std::atomic<int> g_init_state{MG_INIT_STATE_COLD};
mg_init_error_v1 g_init_error = MG_INIT_ERROR_NONE;
const char* g_init_stage = "cold";
std::thread::id g_init_owner;

void fill_init_report(mg_init_report_v1* report, int state, mg_init_error_v1 error, const char* stage) {
    if (!report) return;
    const uint32_t caller_size = report->struct_size;
    if (caller_size < sizeof(mg_init_report_v1)) return;
    report->struct_size = sizeof(mg_init_report_v1);
    report->abi_version = MG_INIT_ABI_VERSION;
    report->state = state;
    report->error = error;
    std::snprintf(report->stage, sizeof(report->stage), "%s", stage ? stage : "unknown");
}

bool initialize_impl(mg_init_error_v1* error, const char** stage) {
    *stage = "config";
    if (!init_config()) {
        *error = MG_INIT_ERROR_CONFIG_PATH;
        return false;
    }

    clear_log();
    start_log();

    LOG_V("Initializing %s ...", RENDERERNAME);
    show_license();

    *stage = "settings";
    init_settings();

    *stage = "backend-load";
    if (!load_libs()) {
        *error = MG_INIT_ERROR_BACKEND_LOAD;
        return false;
    }

    *stage = "egl-bootstrap";
    if (!init_target_egl()) {
        *error = MG_INIT_ERROR_EGL_BOOTSTRAP;
        return false;
    }

    *stage = "gles-bootstrap";
    if (!init_target_gles()) {
        destroy_temp_egl_ctx();
        *error = MG_INIT_ERROR_GLES_BOOTSTRAP;
        return false;
    }

    *stage = "multidraw";
    set_multidraw_setting();

    init_settings_post();

#if PROFILING
    init_perfetto();
#endif

    // Cleanup
#ifndef __APPLE__
    destroy_temp_egl_ctx();
#endif
    *stage = "ready";
    *error = MG_INIT_ERROR_NONE;
    return true;
}

} // namespace

extern "C" int mg_initialize_v1(mg_init_report_v1* report) {
    std::unique_lock<std::mutex> lock(g_init_mutex);

    while (g_init_state.load(std::memory_order_acquire) == MG_INIT_STATE_INITIALIZING) {
        if (g_init_owner == std::this_thread::get_id()) {
            fill_init_report(report, MG_INIT_STATE_FAILED, MG_INIT_ERROR_REENTRANT, "reentrant");
            return 0;
        }
        g_init_cv.wait(lock);
    }

    const int current = g_init_state.load(std::memory_order_acquire);
    if (current == MG_INIT_STATE_READY || current == MG_INIT_STATE_FAILED) {
        fill_init_report(report, current, g_init_error, g_init_stage);
        return current == MG_INIT_STATE_READY ? 1 : 0;
    }

    g_init_owner = std::this_thread::get_id();
    g_init_error = MG_INIT_ERROR_NONE;
    g_init_stage = "initializing";
    g_init_state.store(MG_INIT_STATE_INITIALIZING, std::memory_order_release);
    lock.unlock();

    bool ok = false;
    mg_init_error_v1 error = MG_INIT_ERROR_NONE;
    const char* stage = "initializing";
    try {
        ok = initialize_impl(&error, &stage);
    } catch (const std::exception& exception) {
        LOG_E("MobileGlues initialization threw: %s", exception.what());
        error = MG_INIT_ERROR_EXCEPTION;
        stage = "exception";
    } catch (...) {
        LOG_E("MobileGlues initialization threw an unknown exception");
        error = MG_INIT_ERROR_EXCEPTION;
        stage = "exception";
    }

    lock.lock();
    g_init_error = error;
    g_init_stage = stage;
    g_init_owner = std::thread::id{};
    g_init_state.store(ok ? MG_INIT_STATE_READY : MG_INIT_STATE_FAILED, std::memory_order_release);
    fill_init_report(report, g_init_state.load(std::memory_order_relaxed), g_init_error, g_init_stage);
    lock.unlock();
    g_init_cv.notify_all();
    return ok ? 1 : 0;
}

extern "C" int mg_get_init_report_v1(mg_init_report_v1* report) {
    std::lock_guard<std::mutex> lock(g_init_mutex);
    const int state = g_init_state.load(std::memory_order_acquire);
    fill_init_report(report, state, g_init_error, g_init_stage);
    return state;
}

void proc_init() {
    mg_init_report_v1 report{sizeof(mg_init_report_v1), MG_INIT_ABI_VERSION, MG_INIT_STATE_COLD,
                             MG_INIT_ERROR_NONE, {0}};
    (void)mg_initialize_v1(&report);
}
