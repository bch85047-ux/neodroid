#pragma once

#include "neodroid_types.h"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace neodroid {

constexpr std::array<std::uint8_t, 4> kElfMagic{0x7f, 'E', 'L', 'F'};
constexpr std::uint16_t kElfMachineAarch64 = 183;
constexpr std::uint32_t kElfPtLoad = 1;
constexpr std::uint32_t kElfPtInterp = 3;
constexpr std::uint32_t kElfPtPhdr = 6;
constexpr std::uint32_t kElfPtGnuStack = 0x6474e551;

struct Elf64Header {
    std::array<std::uint8_t, 16> ident{};
    std::uint16_t type{};
    std::uint16_t machine{};
    std::uint32_t version{};
    GuestAddress entry{};
    std::uint64_t program_header_offset{};
    std::uint64_t section_header_offset{};
    std::uint32_t flags{};
    std::uint16_t header_size{};
    std::uint16_t program_header_size{};
    std::uint16_t program_header_count{};
};

struct Elf64ProgramHeader {
    std::uint32_t type{};
    std::uint32_t flags{};
    std::uint64_t offset{};
    GuestAddress virtual_address{};
    GuestAddress physical_address{};
    std::uint64_t file_size{};
    std::uint64_t memory_size{};
    std::uint64_t alignment{};
};

struct ElfLoadSegment {
    GuestRange guest_range;
    std::uint64_t file_offset{};
    std::uint64_t file_size{};
    MemoryProtection protection{MemoryProtection::None};
};

struct LoadedElf {
    std::string path;
    GuestAddress entry{};
    GuestAddress load_bias{};
    std::string interpreter;
    std::vector<ElfLoadSegment> segments;
};

class GuestMemory;

class ElfLoader {
public:
    LoadedElf inspect(std::string_view guest_path,
                      std::span<const std::byte> image) const;
    LoadedElf load(std::string_view guest_path,
                   std::span<const std::byte> image,
                   GuestMemory& memory) const;

private:
    static Elf64Header parse_header(std::span<const std::byte> image);
    static Elf64ProgramHeader parse_program_header(std::span<const std::byte> image,
                                                   std::size_t offset);
};

}  // namespace neodroid
