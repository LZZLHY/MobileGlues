// MobileGlues - config/gpu_utils.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#ifndef MOBILEGLUES_PLUGIN_GPU_UTILS_H
#define MOBILEGLUES_PLUGIN_GPU_UTILS_H

#include <string.h>
#include <string>

std::string getGPUInfo();

// Filesystem path of the shared object that actually provides the GLES entry points, resolved
// from a loaded symbol rather than from a name guess.
//
// The renderer string alone does not say what is underneath: the same string appears whether GLES
// is served by the vendor driver, by a translation layer such as ANGLE, or by a software
// implementation, and those have very different performance characteristics. Knowing the library
// makes a device log self-describing. Returns an empty string when it cannot be determined.
std::string getGLDriverLibrary();

#ifdef __cplusplus
extern "C"
{
#endif

    int isAdreno(const char* gpu);

    int isAdreno730(const char* gpu);

    int isAdreno740(const char* gpu);

    int isAdreno830(const char* gpu);

    // Huawei Maleoon, the GPU in HarmonyOS NEXT devices. Reported so that behaviour which depends
    // on the driver underneath can be selected explicitly instead of being assumed.
    int isMaleoon(const char* gpu);

    int hasVulkan12();

    bool checkIfANGLESupported(const char* gpu);

#ifdef __cplusplus
}
#endif

#endif // MOBILEGLUES_PLUGIN_GPU_UTILS_H
