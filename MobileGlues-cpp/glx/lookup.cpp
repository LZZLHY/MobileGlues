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
#include <EGL/egl.h>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>

#define DEBUG 0

std::string handle_multidraw_func_name(std::string name) {
    std::string namestr = name;
    if (namestr != "glMultiDrawElementsBaseVertex" && namestr != "glMultiDrawElements") {
        return name;
    } else {
        namestr = "mg_" + namestr;
    }

    switch (global_settings.multidraw_mode) {
    case multidraw_mode_t::PreferIndirect:
        namestr += "_indirect";
        break;
    case multidraw_mode_t::PreferBaseVertex:
        namestr += "_basevertex";
        break;
    case multidraw_mode_t::PreferMultidrawIndirect:
        namestr += "_multiindirect";
        break;
    case multidraw_mode_t::DrawElements:
        namestr += "_drawelements";
        break;
    case multidraw_mode_t::Compute:
        namestr += "_compute";
        break;
    default:
        LOG_W("get_multidraw_func() cannot determine multidraw emulation mode!")
        return {};
    }

    return namestr;
}

void* glXGetProcAddress(const char* name) {
    LOG()
    std::string real_func_name = handle_multidraw_func_name(std::string(name));
#ifdef __APPLE__
    return dlsym((void*)(~(uintptr_t)0), real_func_name.c_str());
#else

    void* proc = nullptr;

    // OHOS / amcl: prefer self handle over RTLD_DEFAULT.
    //
    // RTLD_DEFAULT walks the global symbol table in load order; on OHOS the
    // system GLES ICD is already present, so dlsym(RTLD_DEFAULT, "glGetString")
    // resolves to the GLES native symbol instead of the @@LIBGLFW wrapper that
    // MG exports - LWJGL then bypasses MG entirely, surfacing GLES-level errors
    // (GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT etc.) that MG would otherwise mask.
    //
    // Use dladdr(&glXGetProcAddress) -> dlopen(absolute_path, RTLD_NOLOAD) to
    // get a non-default handle that scopes dlsym to *this* SO, and try that
    // first. Fall back to RTLD_DEFAULT for entry points MG genuinely doesn't
    // wrap (most GL extensions).
    static void* self_handle = nullptr;
    static bool self_handle_resolved = false;
    if (!self_handle_resolved) {
        self_handle_resolved = true;
        Dl_info info{};
        if (dladdr(reinterpret_cast<const void*>(&glXGetProcAddress), &info) && info.dli_fname) {
            self_handle = dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD);
            if (!self_handle) {
                self_handle = dlopen(info.dli_fname, RTLD_LAZY | RTLD_NOLOAD);
            }
        }
    }
    if (self_handle) {
        proc = dlsym(self_handle, real_func_name.c_str());
    }
    if (!proc) {
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