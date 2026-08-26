// MobileGlues - gl/upload_scheduler_core.h
// Copyright (c) 2025-2026 MobileGL-Dev
// Licensed under the GNU Lesser General Public License v2.1:
//   https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt
// SPDX-License-Identifier: LGPL-2.1-only
// End of Source File Header

#ifndef MOBILEGLUES_UPLOAD_SCHEDULER_CORE_H
#define MOBILEGLUES_UPLOAD_SCHEDULER_CORE_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

namespace mg::upload {

// A GL object name is not an identity: the frontend name can be recycled after
// glDeleteBuffers, and glBufferData/glBufferStorage replace the storage behind a
// still-live object. Every deferred record therefore carries both generations.
struct BufferIdentity {
    std::uint64_t share_group{0};
    std::uint64_t object_generation{0};
    std::uint64_t storage_generation{0};
    std::uint32_t frontend_name{0};
    std::uint32_t backend_name{0};

    friend bool operator==(const BufferIdentity& left, const BufferIdentity& right) noexcept {
        return left.share_group == right.share_group && left.object_generation == right.object_generation &&
               left.storage_generation == right.storage_generation && left.frontend_name == right.frontend_name &&
               left.backend_name == right.backend_name;
    }
};

struct Limits {
    std::size_t max_records{2048};
    std::size_t max_payload_bytes{8U * 1024U * 1024U};
};

struct Record {
    BufferIdentity identity{};
    std::uint64_t offset{0};
    std::uint64_t size{0};
    std::size_t payload_offset{0};
    std::uint64_t source_calls{0};
    std::uint64_t source_bytes{0};
};

enum class EnqueueResult : std::uint8_t {
    Queued,
    Merged,
    Invalid,
    RecordLimit,
    PayloadLimit,
    AllocationFailure,
};

// Host-owned payload queue. It never stores an application pointer beyond the
// call: successful enqueue copies the bytes before returning. Only the last
// record can merge, so no GL command can be reordered across another upload.
//
// The merge is intentionally directional. A later write whose start lies in or
// exactly after the previous range can overwrite/extend that range. A write that
// extends to the left remains a separate record; supporting it would require a
// second allocation or moving the previous payload and has no correctness or
// observed-workload benefit. The final bytes are identical either way.
class Queue {
public:
    explicit Queue(Limits limits = {}) noexcept : limits_(limits) {}

    EnqueueResult enqueue(const BufferIdentity& identity, std::uint64_t offset, std::size_t size,
                          const void* data) noexcept {
        if (identity.share_group == 0 || identity.object_generation == 0 || identity.storage_generation == 0 ||
            identity.frontend_name == 0 || identity.backend_name == 0 || size == 0 || data == nullptr ||
            offset > std::numeric_limits<std::uint64_t>::max() - static_cast<std::uint64_t>(size)) {
            return EnqueueResult::Invalid;
        }

        if (!records_.empty()) {
            Record& last = records_.back();
            const std::uint64_t last_end = last.offset + last.size;
            const std::uint64_t new_end = offset + static_cast<std::uint64_t>(size);
            if (last.identity == identity && offset >= last.offset && offset <= last_end) {
                const std::uint64_t merged_end = std::max(last_end, new_end);
                const std::uint64_t merged_size_u64 = merged_end - last.offset;
                if (merged_size_u64 > limits_.max_payload_bytes ||
                    merged_size_u64 > std::numeric_limits<std::size_t>::max()) {
                    return EnqueueResult::PayloadLimit;
                }
                const std::size_t merged_size = static_cast<std::size_t>(merged_size_u64);
                const std::size_t old_payload_size = payload_.size();
                const std::size_t last_end_in_payload = last.payload_offset + static_cast<std::size_t>(last.size);
                if (last_end_in_payload != old_payload_size) return EnqueueResult::Invalid;
                const std::size_t growth = merged_size - static_cast<std::size_t>(last.size);
                if (old_payload_size > limits_.max_payload_bytes - growth) return EnqueueResult::PayloadLimit;
                try {
                    payload_.resize(old_payload_size + growth);
                } catch (const std::bad_alloc&) {
                    return EnqueueResult::AllocationFailure;
                } catch (const std::length_error&) {
                    return EnqueueResult::AllocationFailure;
                }
                std::memcpy(payload_.data() + last.payload_offset + static_cast<std::size_t>(offset - last.offset),
                            data, size);
                last.size = merged_size_u64;
                ++last.source_calls;
                last.source_bytes += size;
                return EnqueueResult::Merged;
            }
        }

        if (records_.size() >= limits_.max_records) return EnqueueResult::RecordLimit;
        if (size > limits_.max_payload_bytes || payload_.size() > limits_.max_payload_bytes - size) {
            return EnqueueResult::PayloadLimit;
        }

        const std::size_t old_payload_size = payload_.size();
        try {
            payload_.resize(old_payload_size + size);
            std::memcpy(payload_.data() + old_payload_size, data, size);
            records_.push_back(Record{identity, offset, static_cast<std::uint64_t>(size), old_payload_size, 1, size});
        } catch (const std::bad_alloc&) {
            payload_.resize(old_payload_size);
            return EnqueueResult::AllocationFailure;
        } catch (const std::length_error&) {
            payload_.resize(old_payload_size);
            return EnqueueResult::AllocationFailure;
        }
        return EnqueueResult::Queued;
    }

    [[nodiscard]] bool empty() const noexcept { return records_.empty(); }
    [[nodiscard]] std::size_t recordCount() const noexcept { return records_.size(); }
    [[nodiscard]] std::size_t payloadBytes() const noexcept { return payload_.size(); }
    [[nodiscard]] const std::vector<Record>& records() const noexcept { return records_; }
    [[nodiscard]] const std::byte* payload(const Record& record) const noexcept {
        return reinterpret_cast<const std::byte*>(payload_.data() + record.payload_offset);
    }

    // Clear retains capacity. Queue memory is bounded by Limits and is reused by
    // the owning GL context rather than allocated again every frame.
    void clear() noexcept {
        records_.clear();
        payload_.clear();
    }

private:
    Limits limits_{};
    std::vector<unsigned char> payload_{};
    std::vector<Record> records_{};
};

struct OpportunityTotals {
    std::uint64_t eligible_calls{0};
    std::uint64_t eligible_bytes{0};
    std::uint64_t would_submit_calls{0};
    std::uint64_t mergeable_calls{0};
    std::uint64_t max_run_calls{0};
    std::uint64_t max_run_bytes{0};
};

// Allocation-free model used by observe mode. A barrier closes the current run;
// therefore the reported merge opportunity matches the conservative runtime
// algorithm instead of assuming writes can move across arbitrary GL calls.
class OpportunityModel {
public:
    void upload(const BufferIdentity& identity, std::uint64_t offset, std::size_t size) noexcept {
        if (size == 0 || offset > std::numeric_limits<std::uint64_t>::max() - size) return;
        ++totals_.eligible_calls;
        totals_.eligible_bytes += size;

        const std::uint64_t end = offset + size;
        if (active_ && identity == identity_ && offset >= offset_ && offset <= end_) {
            ++totals_.mergeable_calls;
            end_ = std::max(end_, end);
        } else {
            closeRun();
            active_ = true;
            identity_ = identity;
            offset_ = offset;
            end_ = end;
            run_calls_ = 0;
            run_bytes_ = 0;
            ++totals_.would_submit_calls;
        }
        ++run_calls_;
        run_bytes_ += size;
        totals_.max_run_calls = std::max(totals_.max_run_calls, run_calls_);
        totals_.max_run_bytes = std::max(totals_.max_run_bytes, run_bytes_);
    }

    void barrier() noexcept { closeRun(); }
    [[nodiscard]] OpportunityTotals totals() const noexcept { return totals_; }
    void resetTotals() noexcept { totals_ = {}; }

private:
    void closeRun() noexcept {
        active_ = false;
        run_calls_ = 0;
        run_bytes_ = 0;
    }

    bool active_{false};
    BufferIdentity identity_{};
    std::uint64_t offset_{0};
    std::uint64_t end_{0};
    std::uint64_t run_calls_{0};
    std::uint64_t run_bytes_{0};
    OpportunityTotals totals_{};
};

} // namespace mg::upload

#endif // MOBILEGLUES_UPLOAD_SCHEDULER_CORE_H
