// MobileGlues - platform/provider/GPU profile classification
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only

#ifndef MOBILEGLUES_DRIVER_PROFILE_CORE_H
#define MOBILEGLUES_DRIVER_PROFILE_CORE_H

#include <cctype>
#include <string>
#include <string_view>
#include <utility>

namespace mg::platform {

enum class PlatformKind {
    Generic,
    Android,
    Ohos,
    Apple,
};

enum class ProviderKind {
    Unknown,
    Native,
    Angle,
    Zink,
    MetalAngle,
};

enum class GpuFamily {
    Unknown,
    Maleoon,
    Adreno,
    Mali,
    Xclipse,
    Apple,
};

struct DriverProfile {
    PlatformKind platform{PlatformKind::Generic};
    ProviderKind provider{ProviderKind::Unknown};
    GpuFamily gpu_family{GpuFamily::Unknown};
    std::string provider_image;
    std::string vendor;
    std::string renderer;
    std::string version;
};

inline std::string Lower(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const unsigned char ch : text) result.push_back(static_cast<char>(std::tolower(ch)));
    return result;
}

inline bool Contains(std::string_view haystack, std::string_view needle) {
    return Lower(haystack).find(Lower(needle)) != std::string::npos;
}

constexpr PlatformKind CompileTimePlatform() noexcept {
#if defined(MG_PLATFORM_OHOS)
    return PlatformKind::Ohos;
#elif defined(__ANDROID__)
    return PlatformKind::Android;
#elif defined(__APPLE__)
    return PlatformKind::Apple;
#else
    return PlatformKind::Generic;
#endif
}

inline ProviderKind ClassifyProvider(PlatformKind platform, std::string_view provider_image,
                                     std::string_view vendor, std::string_view renderer,
                                     bool angle_loader_selected) {
    const bool angle = angle_loader_selected || Contains(provider_image, "angle") || Contains(vendor, "angle") ||
                       Contains(renderer, "angle");
    if (Contains(provider_image, "zink") || Contains(renderer, "zink")) {
        return ProviderKind::Zink;
    }
    if (angle && platform == PlatformKind::Apple) return ProviderKind::MetalAngle;
    if (angle) return ProviderKind::Angle;
    if (!provider_image.empty() || !vendor.empty() || !renderer.empty()) return ProviderKind::Native;
    return ProviderKind::Unknown;
}

inline GpuFamily ClassifyGpu(std::string_view vendor, std::string_view renderer) {
    const std::string joined = Lower(std::string(vendor) + " " + std::string(renderer));
    if (joined.find("maleoon") != std::string::npos) return GpuFamily::Maleoon;
    if (joined.find("adreno") != std::string::npos || joined.find("qualcomm") != std::string::npos)
        return GpuFamily::Adreno;
    if (joined.find("xclipse") != std::string::npos) return GpuFamily::Xclipse;
    if (joined.find("mali") != std::string::npos || joined.find("arm") != std::string::npos)
        return GpuFamily::Mali;
    if (joined.find("apple") != std::string::npos) return GpuFamily::Apple;
    return GpuFamily::Unknown;
}

inline DriverProfile BuildProfile(PlatformKind platform, std::string provider_image, std::string vendor,
                                  std::string renderer, std::string version, bool angle_loader_selected) {
    DriverProfile result;
    result.platform = platform;
    result.provider = ClassifyProvider(platform, provider_image, vendor, renderer, angle_loader_selected);
    result.gpu_family = ClassifyGpu(vendor, renderer);
    result.provider_image = std::move(provider_image);
    result.vendor = std::move(vendor);
    result.renderer = std::move(renderer);
    result.version = std::move(version);
    return result;
}

constexpr const char* PlatformName(PlatformKind platform) noexcept {
    switch (platform) {
    case PlatformKind::Android: return "android";
    case PlatformKind::Ohos: return "ohos";
    case PlatformKind::Apple: return "apple";
    case PlatformKind::Generic:
    default: return "generic";
    }
}

constexpr const char* ProviderName(ProviderKind provider) noexcept {
    switch (provider) {
    case ProviderKind::Native: return "native";
    case ProviderKind::Angle: return "angle";
    case ProviderKind::Zink: return "zink";
    case ProviderKind::MetalAngle: return "metal-angle";
    case ProviderKind::Unknown:
    default: return "unknown";
    }
}

constexpr const char* GpuFamilyName(GpuFamily family) noexcept {
    switch (family) {
    case GpuFamily::Maleoon: return "maleoon";
    case GpuFamily::Adreno: return "adreno";
    case GpuFamily::Mali: return "mali";
    case GpuFamily::Xclipse: return "xclipse";
    case GpuFamily::Apple: return "apple";
    case GpuFamily::Unknown:
    default: return "unknown";
    }
}

} // namespace mg::platform

#endif // MOBILEGLUES_DRIVER_PROFILE_CORE_H
