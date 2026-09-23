// MobileGlues - 有界buffer读回与借用状态收尾。
// SPDX-License-Identifier: LGPL-2.1-only
#pragma once
#include <algorithm>
#include <cstdint>
#include <cstddef>

namespace mg::buffer_readback {
enum class Result { Success, InvalidValue, InvalidOperation, DriverFailure };
constexpr std::int64_t ScratchLimit = 1024 * 1024;

/**
 * 读取真实GPU存储。Driver负责GLES调用、错误保留、临时buffer及绑定恢复；本层只决定
 * 参数边界、映射合法性与有限分块。persistent源不允许临时unmap或直接读只写指针，
 * Driver在独立scratch中读回；普通未映射源可直接作临时只读映射。
 * Begin即使部分失败也必须可End，所有返回路径都退休本次资源；零字节不触碰driver。
 */
template<class Driver>
Result Read(Driver& driver, std::int64_t capacity, std::int64_t offset, std::int64_t size,
            void* destination, bool mapped, bool persistent) {
    if (capacity < 0 || offset < 0 || size < 0 || offset > capacity || size > capacity - offset ||
        (size > 0 && destination == nullptr)) return Result::InvalidValue;
    if (mapped && !persistent) return Result::InvalidOperation;
    if (size == 0) return Result::Success;
    if (!driver.Begin(mapped, std::min(size, ScratchLimit))) {
        driver.End();
        return Result::DriverFailure;
    }
    bool complete = true;
    auto* output = static_cast<unsigned char*>(destination);
    for (std::int64_t done = 0; done < size;) {
        const std::int64_t count = std::min(size - done, ScratchLimit);
        if (!driver.Transfer(offset + done, count, output + static_cast<std::size_t>(done), mapped)) {
            complete = false;
            break;
        }
        done += count;
    }
    const bool restored = driver.End();
    return complete && restored ? Result::Success : Result::DriverFailure;
}
} // namespace mg::buffer_readback
