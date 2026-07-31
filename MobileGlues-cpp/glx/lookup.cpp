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

// Optional host interface.
//
// A host application may drive a real desktop GL implementation, such as OSMesa with the Zink
// Gallium driver, instead of this translation layer. When it does, GL entry points must come from
// there rather than from here. The host opts in by exporting these two C symbols:
//
//   int   mg_host_osmesa_zink_mode(void);              non-zero while that mode is active
//   void* mg_host_osmesa_get_proc_address(const char*); resolves one GL entry point, or null
//
// They are looked up at runtime rather than referenced at link time, deliberately. MobileGlues
// cannot define them, and an undefined reference is not portable: ELF tolerates it in a shared
// object, Mach-O rejects it, and neither weak nor weak_import declarations made the standalone
// Apple build link. Resolving by name has no link-time dependency at all, so the library builds
// alone on every platform and the interface stays inert when nothing provides it.
extern "C"
{
    typedef int (*MgHostZinkModeFn)(void);
    typedef void* (*MgHostGetProcAddressFn)(const char*);
}

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

namespace {

    // Handle scoped to this shared object. Used for the host hooks below, and for the same reason
    // the GL lookup prefers it: the global scope can contain a different definition of a name this
    // library also provides.
    void* mg_self_handle() {
        static void* handle = nullptr;
        static bool resolved = false;
        if (!resolved) {
            resolved = true;
            Dl_info info{};
            if (dladdr(reinterpret_cast<const void*>(&glXGetProcAddress), &info) && info.dli_fname) {
                handle = dlopen(info.dli_fname, RTLD_NOW | RTLD_NOLOAD);
                if (!handle) handle = dlopen(info.dli_fname, RTLD_LAZY | RTLD_NOLOAD);
            }
        }
        return handle;
    }

    void* mg_lookup_optional_symbol(const char* name) {
        void* symbol = nullptr;
        if (void* self = mg_self_handle()) symbol = dlsym(self, name);
        if (!symbol) symbol = dlsym(RTLD_DEFAULT, name);
        return symbol;
    }

    // Resolved once. A host either provides both hooks or neither is usable, so they are treated
    // as one unit.
    bool host_drives_desktop_gl() {
        static MgHostZinkModeFn zink_mode = nullptr;
        static bool resolved = false;
        if (!resolved) {
            resolved = true;
            zink_mode = reinterpret_cast<MgHostZinkModeFn>(mg_lookup_optional_symbol("mg_host_osmesa_zink_mode"));
        }
        return zink_mode != nullptr && zink_mode() != 0;
    }

    void* host_get_proc_address(const char* name) {
        static MgHostGetProcAddressFn get_proc = nullptr;
        static bool resolved = false;
        if (!resolved) {
            resolved = true;
            get_proc =
                reinterpret_cast<MgHostGetProcAddressFn>(mg_lookup_optional_symbol("mg_host_osmesa_get_proc_address"));
        }
        return get_proc ? get_proc(name) : nullptr;
    }

} // namespace

void* glXGetProcAddress(const char* name) {
    LOG()
    // When the host drives a real desktop GL implementation, resolve the original function name
    // there before MobileGlues applies its GLES emulation name rewriting: that rewriting exists to
    // emulate entry points GLES lacks, which a desktop implementation provides natively.
    if (host_drives_desktop_gl()) {
        void* proc = host_get_proc_address(name);
        if (proc) return proc;
    }
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