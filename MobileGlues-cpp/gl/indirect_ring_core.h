// MobileGlues - allocation-free indirect command-buffer ring selection
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only

#ifndef MOBILEGLUES_INDIRECT_RING_CORE_H
#define MOBILEGLUES_INDIRECT_RING_CORE_H

#include <cstddef>

namespace mg::indirect_ring {

// The GL layer owns buffers and fences. This tiny core owns only fair,
// wrap-around selection, so host tests exercise the exact all-busy/fallback
// decision without a fake GLES implementation.
template <std::size_t SlotCount> class Cursor {
    static_assert(SlotCount > 0, "an indirect ring needs at least one slot");

  public:
    template <typename IsAvailable> int acquire(IsAvailable&& is_available) {
        for (std::size_t offset = 0; offset < SlotCount; ++offset) {
            const std::size_t slot = (next_ + offset) % SlotCount;
            if (!is_available(slot)) continue;
            next_ = (slot + 1) % SlotCount;
            return static_cast<int>(slot);
        }
        return -1;
    }

    void reset() noexcept {
        next_ = 0;
    }

    std::size_t next() const noexcept {
        return next_;
    }

  private:
    std::size_t next_{0};
};

} // namespace mg::indirect_ring

#endif // MOBILEGLUES_INDIRECT_RING_CORE_H
