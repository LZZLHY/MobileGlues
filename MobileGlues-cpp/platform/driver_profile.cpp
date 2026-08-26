// MobileGlues - runtime driver profile capture
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only

#include "driver_profile.h"

#include "../gl/mg.h"
#include "../gl/log.h"
#include "../gles/loader.h"

#include <dlfcn.h>

namespace mg::platform {
namespace {

DriverProfile g_profile;

std::string GlString(GLenum name) {
    if (!GLES.glGetString) return {};
    const GLubyte* value = GLES.glGetString(name);
    return value ? reinterpret_cast<const char*>(value) : std::string{};
}

std::string ProviderImage() {
    if (!GLES.glGetString) return {};
    Dl_info info{};
    if (dladdr(reinterpret_cast<void*>(GLES.glGetString), &info) == 0 || !info.dli_fname) return {};
    return info.dli_fname;
}

} // namespace

void RefreshDriverProfile() {
    g_profile = BuildProfile(CompileTimePlatform(), ProviderImage(), GlString(GL_VENDOR), GlString(GL_RENDERER),
                             GlString(GL_VERSION), g_angle_in_use);
    // PF-02: one allocation-time/bootstrap log, not a frame-path probe. This is
    // identity only; no GL behavior is selected from platform alone.
    LOG_I("[MG-PROFILE] schema=1 platform=%s provider=%s gpu_family=%s image=%s vendor=%s renderer=%s version=%s",
          PlatformName(g_profile.platform), ProviderName(g_profile.provider), GpuFamilyName(g_profile.gpu_family),
          g_profile.provider_image.empty() ? "(unknown)" : g_profile.provider_image.c_str(),
          g_profile.vendor.empty() ? "(unknown)" : g_profile.vendor.c_str(),
          g_profile.renderer.empty() ? "(unknown)" : g_profile.renderer.c_str(),
          g_profile.version.empty() ? "(unknown)" : g_profile.version.c_str())
}

const DriverProfile& CurrentDriverProfile() noexcept {
    return g_profile;
}

} // namespace mg::platform
