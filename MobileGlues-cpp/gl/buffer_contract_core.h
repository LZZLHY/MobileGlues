// MobileGlues - platform-neutral buffer storage/mapping contract policy
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only

#ifndef MOBILEGLUES_BUFFER_CONTRACT_CORE_H
#define MOBILEGLUES_BUFFER_CONTRACT_CORE_H

#include <cstdint>

namespace mg::buffer_contract {

using Flags = std::uint32_t;
using Offset = std::int64_t;

// OpenGL's numeric values are stable API tokens. Keeping this policy header free
// of GL headers lets the same decision code run in small host tests. buffer.cpp
// static-asserts every value against the headers used for the real build.
constexpr Flags MapWrite = 0x0002U;
constexpr Flags MapRead = 0x0001U;
constexpr Flags MapFlushExplicit = 0x0010U;
constexpr Flags MapPersistent = 0x0040U;
constexpr Flags MapCoherent = 0x0080U;
constexpr Flags DynamicStorage = 0x0100U;

struct StorageState {
    Flags requested_flags{0};
    Flags effective_flags{0};
    bool immutable{false};
    bool coherent_substitution{false};
};

struct MappingDecision {
    Flags requested_access{0};
    Flags effective_access{0};
    bool coherent_substitution{false};
};

struct MappingState {
    Offset offset{0};
    Offset length{0};
    Flags requested_access{0};
    Flags effective_access{0};
    bool active{false};
    bool coherent_substitution{false};
};

enum class FlushDisposition : std::uint8_t {
    Forward,
    Suppress,
    InvalidValue,
};

constexpr StorageState MutableStorage() noexcept {
    return {};
}

// BC-01..03: coherent substitution is a stronger backend implementation only
// for a store the application actually made writable and persistent. In
// particular, DYNAMIC_STORAGE alone is a BufferSubData permission and must not
// become a mapping permission.
constexpr StorageState ImmutableStorage(Flags requested, bool enable_coherent_substitution) noexcept {
    StorageState state{requested, requested, true, false};
    const Flags persistent_write = MapWrite | MapPersistent;
    if (enable_coherent_substitution && (requested & persistent_write) == persistent_write &&
        (requested & MapCoherent) == 0) {
        state.effective_flags |= MapCoherent;
        state.coherent_substitution = true;
    }
    return state;
}

// BC-04: adding COHERENT to the store does not make a later mapping coherent.
// The mapping access has to request it too. This rewrite is limited to the one
// contract it replaces: WRITE + PERSISTENT + FLUSH_EXPLICIT.
constexpr MappingDecision DecideMapping(const StorageState& storage, Flags requested_access) noexcept {
    MappingDecision decision{requested_access, requested_access, false};
    const Flags persistent_write_flush = MapWrite | MapPersistent | MapFlushExplicit;
    if (storage.immutable && storage.coherent_substitution &&
        (storage.effective_flags & MapCoherent) != 0 &&
        (requested_access & persistent_write_flush) == persistent_write_flush &&
        (requested_access & MapCoherent) == 0) {
        decision.effective_access = (requested_access & ~MapFlushExplicit) | MapCoherent;
        decision.coherent_substitution = true;
    }
    return decision;
}

// A backend store may contain permissions that MG added for its own internal
// implementation, but those permissions are not part of the application's GL
// contract. Reject an application mapping that the originally requested
// immutable store would not have accepted.
constexpr bool ApplicationMappingAllowed(const StorageState& storage, Flags requested_access) noexcept {
    if (!storage.immutable) return true;
    const Flags storage_permissions = MapRead | MapWrite | MapPersistent | MapCoherent;
    const Flags required = requested_access & storage_permissions;
    return (required & ~storage.requested_flags) == 0;
}

constexpr MappingState SuccessfulMapping(Offset offset, Offset length, const MappingDecision& decision) noexcept {
    return {offset, length, decision.requested_access, decision.effective_access, true,
            decision.coherent_substitution};
}

// BC-05: only a valid flush from an application mapping that MG successfully
// replaced with a coherent mapping may become a no-op. Offsets are relative to
// the mapped range, per glFlushMappedBufferRange.
constexpr FlushDisposition DecideFlush(const MappingState& mapping, Offset relative_offset,
                                       Offset length) noexcept {
    if (!mapping.active || !mapping.coherent_substitution ||
        (mapping.requested_access & MapFlushExplicit) == 0) {
        return FlushDisposition::Forward;
    }
    if (relative_offset < 0 || length < 0 || relative_offset > mapping.length ||
        length > mapping.length - relative_offset) {
        return FlushDisposition::InvalidValue;
    }
    return FlushDisposition::Suppress;
}

} // namespace mg::buffer_contract

#endif // MOBILEGLUES_BUFFER_CONTRACT_CORE_H
