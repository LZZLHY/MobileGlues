// MobileGlues - init.cpp
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#include "includes.h"

#if MOBILEGLUES_AUTO_INIT
struct static_block_t {
    static_block_t() { (void)mg_initialize_v1(nullptr); }
};

static static_block_t static_block;
#endif
