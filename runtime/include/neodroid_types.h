#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace neodroid {

using GuestAddress = std::uint64_t;
using GuestSize = std::uint64_t;
using GuestPid = std::int32_t;
using GuestTid = std::int32_t;
using GuestFd = std::int32_t;
using SyscallResult = std::int64_t;

constexpr GuestAddress kGuestPageSize = 4096;
constexpr GuestAddress kGuestUserTop = 0x0000'7fff'ffff'f000ULL;
constexpr GuestAddress kGuestVdsoBase = 0x0000'7fff'fffe'0000ULL;
constexpr GuestAddress kGuestStackTop = 0x0000'7fff'ff00'0000ULL;
constexpr GuestSize kDefaultGuestRam = 6ULL * 1024 * 1024 * 1024;

struct GuestRange {
    GuestAddress start{};
    GuestSize size{};

    [[nodiscard]] constexpr GuestAddress end() const { return start + size; }
    [[nodiscard]] constexpr bool contains(GuestAddress value) const {
        return value >= start && value < end();
    }
};

enum class MemoryProtection : std::uint32_t {
    None = 0,
    Read = 1u << 0,
    Write = 1u << 1,
    Execute = 1u << 2,
};

constexpr MemoryProtection operator|(MemoryProtection a, MemoryProtection b) {
    return static_cast<MemoryProtection>(static_cast<std::uint32_t>(a) |
                                         static_cast<std::uint32_t>(b));
}

constexpr bool has_protection(MemoryProtection value, MemoryProtection flag) {
    return (static_cast<std::uint32_t>(value) & static_cast<std::uint32_t>(flag)) != 0;
}

struct GuestRegisters {
    std::array<std::uint64_t, 31> x{};
    GuestAddress sp{};
    GuestAddress pc{};
    std::uint64_t pstate{};

    [[nodiscard]] std::uint64_t syscall_number() const { return x[8]; }
    [[nodiscard]] std::uint64_t argument(std::size_t index) const { return x[index]; }
    void set_result(SyscallResult value) { x[0] = static_cast<std::uint64_t>(value); }
};

struct GuestProcessInfo {
    GuestPid pid{};
    GuestPid parent_pid{};
    std::string executable;
    GuestRange stack;
    GuestRange vdso;
};

}  // namespace neodroid
