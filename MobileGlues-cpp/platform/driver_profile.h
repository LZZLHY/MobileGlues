// MobileGlues - runtime driver profile capture
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only

#ifndef MOBILEGLUES_DRIVER_PROFILE_H
#define MOBILEGLUES_DRIVER_PROFILE_H

#include "driver_profile_core.h"

namespace mg::platform {

void RefreshDriverProfile();
const DriverProfile& CurrentDriverProfile() noexcept;

} // namespace mg::platform

#endif // MOBILEGLUES_DRIVER_PROFILE_H
