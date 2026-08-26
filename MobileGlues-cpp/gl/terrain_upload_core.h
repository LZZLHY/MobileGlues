// MobileGlues - targeted terrain upload policy for large dynamic stores
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only

#ifndef MOBILEGLUES_TERRAIN_UPLOAD_CORE_H
#define MOBILEGLUES_TERRAIN_UPLOAD_CORE_H

#include <cstddef>

namespace mg::terrain_upload {

constexpr std::size_t Store32MiB = 32U * 1024U * 1024U;
constexpr std::size_t Store128MiB = 128U * 1024U * 1024U;

constexpr bool IsRenderPearlTerrainStore(std::size_t bytes) noexcept {
    return bytes == Store32MiB || bytes == Store128MiB;
}

} // namespace mg::terrain_upload

#endif // MOBILEGLUES_TERRAIN_UPLOAD_CORE_H
