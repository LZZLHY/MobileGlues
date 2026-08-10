// MobileGlues - glx/lookup.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#include "lookup.h"

#include "../config/settings.h"
#include "../gl/envvars.h"
#include "../gl/log.h"
#include "../gl/mg.h"
#include "../includes.h"
#include <MG/init.h>
#include <EGL/egl.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#include <mutex>

#define DEBUG 0

#if !defined(__APPLE__)
namespace {

// Resolve the handle for the image that owns this resolver. RTLD_DEFAULT is not
// sufficient in Android/OHOS-style processes where a system GLES implementation
// may already be global: it can return the backend function and bypass the
// MobileGlues frontend completely. A failed lookup is deliberately not cached,
// so a loader namespace that becomes ready later can retry safely.
void* own_image_handle() {
    static std::atomic<void*> cached{nullptr};
    static std::mutex resolve_mutex;

    if (void* handle = cached.load(std::memory_order_acquire)) return handle;

    std::lock_guard<std::mutex> lock(resolve_mutex);
    if (void* handle = cached.load(std::memory_order_relaxed)) return handle;

    Dl_info info{};
    if (!dladdr(reinterpret_cast<const void*>(&glXGetProcAddress), &info) || !info.dli_fname) return nullptr;

    void* handle = dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD);
    if (!handle) handle = dlopen(info.dli_fname, RTLD_LAZY | RTLD_NOLOAD);
    if (handle) cached.store(handle, std::memory_order_release);
    return handle;
}

} // namespace
#endif

// The application can ask for a multi-draw entry point by name and call the
// result directly, bypassing the dispatcher in gl/multidraw.cpp. Hand back the
// symbol implementing whatever backend that entry point resolved to, so both
// routes agree. The suffix table lives in config/settings.cpp for exactly that
// reason.
std::string handle_multidraw_func_name(std::string name) {
    md_entry_t entry;
    if (name == "glMultiDrawElements") {
        entry = md_entry_t::Elements;
    } else if (name == "glMultiDrawElementsBaseVertex") {
        entry = md_entry_t::ElementsBaseVertex;
    } else {
        // Everything else -- glMultiDrawArrays, the two *Indirect entry points and
        // every EXT/ARB alias -- is a single exported definition that selects its
        // own backend internally, so the plain name is already correct.
        return name;
    }

    const char* suffix = md_backend_suffix(multidraw_backend_of(entry));
    if (!suffix) {
        // Auto should never survive init_settings_post. Fall back to the
        // dispatcher rather than to dlsym of a name that does not exist.
        LOG_W_FORCE("handle_multidraw_func_name: %s has no resolved backend, using the dispatcher", name.c_str())
        return name;
    }
    return "mg_" + name + suffix;
}

void* glXGetProcAddress(const char* name) {
    LOG()
    if (!name) return nullptr;
    mg_init_report_v1 init_report{sizeof(mg_init_report_v1), MG_INIT_ABI_VERSION,
                                  MG_INIT_STATE_COLD, MG_INIT_ERROR_NONE, {0}};
    if (!mg_initialize_v1(&init_report)) {
        LOG_W_FORCE("glXGetProcAddress rejected before MobileGlues READY (state=%d error=%d stage=%s)",
                    init_report.state, init_report.error, init_report.stage)
        return nullptr;
    }
    std::string real_func_name = handle_multidraw_func_name(std::string(name));
#ifdef __APPLE__
    return dlsym((void*)(~(uintptr_t)0), real_func_name.c_str());
#else

    void* proc = nullptr;

    if (void* handle = own_image_handle()) {
        proc = dlsym(handle, real_func_name.c_str());
    }
    if (!proc) {
        // Preserve upstream's extension fallback: MobileGlues intentionally
        // does not wrap every vendor entry point exposed by the GLES backend.
        proc = dlsym(RTLD_DEFAULT, real_func_name.c_str());
    }

    if (!proc) {
        LOG_W("Failed to get OpenGL function: %s", real_func_name.c_str())
        return nullptr;
    }

    return proc;
#endif
}

void* glXGetProcAddressARB(const char* name) {
    return glXGetProcAddress(name);
}
