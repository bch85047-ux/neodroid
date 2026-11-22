#include "neodroid_elf.h"
#include "neodroid_runtime.h"

#include <cstring>
#include <stdexcept>

namespace neodroid {
namespace {

template <typename T>
T read_at(std::span<const std::byte> image, std::size_t offset) {
    if (offset + sizeof(T) > image.size()) {
        throw std::runtime_error("ELF parse out of bounds");
    }
    T value{};
    std::memcpy(&value, image.data() + offset, sizeof(T));
    return value;
}

MemoryProtection protection_from_flags(std::uint32_t flags) {
    // ELF PF_X=1 PF_W=2 PF_R=4
    auto protection = MemoryProtection::None;
    if (flags & 0x4) {
        protection = protection | MemoryProtection::Read;
    }
    if (flags & 0x2) {
        protection = protection | MemoryProtection::Write;
    }
    if (flags & 0x1) {
        protection = protection | MemoryProtection::Execute;
    }
    return protection;
}

}  // namespace

Elf64Header ElfLoader::parse_header(std::span<const std::byte> image) {
    Elf64Header header{};
    std::memcpy(header.ident.data(), image.data(), header.ident.size());

    if (!std::equal(kElfMagic.begin(), kElfMagic.end(), header.ident.begin())) {
        throw std::runtime_error("not an ELF image");
    }
    if (header.ident[4] != 2) {  // ELFCLASS64
        throw std::runtime_error("only ELF64 supported");
    }
    if (header.ident[5] != 1) {  // ELFDATA2LSB
        throw std::runtime_error("only little-endian ELF supported");
    }

    header.type = read_at<std::uint16_t>(image, 0x10);
    header.machine = read_at<std::uint16_t>(image, 0x12);
    header.version = read_at<std::uint32_t>(image, 0x14);
    header.entry = read_at<std::uint64_t>(image, 0x18);
    header.program_header_offset = read_at<std::uint64_t>(image, 0x20);
    header.section_header_offset = read_at<std::uint64_t>(image, 0x28);
    header.flags = read_at<std::uint32_t>(image, 0x30);
    header.header_size = read_at<std::uint16_t>(image, 0x34);
    header.program_header_size = read_at<std::uint16_t>(image, 0x36);
    header.program_header_count = read_at<std::uint16_t>(image, 0x38);

    if (header.machine != kElfMachineAarch64) {
        throw std::runtime_error("only AArch64 ELF supported");
    }
    return header;
}

Elf64ProgramHeader ElfLoader::parse_program_header(std::span<const std::byte> image,
                                                   std::size_t offset) {
    Elf64ProgramHeader ph{};
    ph.type = read_at<std::uint32_t>(image, offset + 0x00);
    ph.flags = read_at<std::uint32_t>(image, offset + 0x04);
    ph.offset = read_at<std::uint64_t>(image, offset + 0x08);
    ph.virtual_address = read_at<std::uint64_t>(image, offset + 0x10);
    ph.physical_address = read_at<std::uint64_t>(image, offset + 0x18);
    ph.file_size = read_at<std::uint64_t>(image, offset + 0x20);
    ph.memory_size = read_at<std::uint64_t>(image, offset + 0x28);
    ph.alignment = read_at<std::uint64_t>(image, offset + 0x30);
    return ph;
}

LoadedElf ElfLoader::inspect(std::string_view guest_path,
                             std::span<const std::byte> image) const {
    const auto header = parse_header(image);
    LoadedElf loaded{};
    loaded.path = std::string(guest_path);
    loaded.entry = header.entry;

    for (std::uint16_t index = 0; index < header.program_header_count; ++index) {
        const auto offset = header.program_header_offset +
                            static_cast<std::uint64_t>(index) * header.program_header_size;
        const auto ph = parse_program_header(image, static_cast<std::size_t>(offset));

        if (ph.type == kElfPtLoad) {
            ElfLoadSegment segment{};
            segment.guest_range.start = ph.virtual_address;
            segment.guest_range.size = ph.memory_size;
            segment.file_offset = ph.offset;
            segment.file_size = ph.file_size;
            segment.protection = protection_from_flags(ph.flags);
            loaded.segments.push_back(segment);
        } else if (ph.type == kElfPtInterp && ph.file_size > 0) {
            const auto begin = image.data() + ph.offset;
            loaded.interpreter.assign(
                reinterpret_cast<const char*>(begin),
                reinterpret_cast<const char*>(begin + ph.file_size));
            if (!loaded.interpreter.empty() && loaded.interpreter.back() == '\0') {
                loaded.interpreter.pop_back();
            }
        }
    }
    return loaded;
}

LoadedElf ElfLoader::load(std::string_view guest_path,
                          std::span<const std::byte> image,
                          GuestMemory& memory) const {
    auto loaded = inspect(guest_path, image);

    // Map every PT_LOAD segment and copy file bytes into guest memory.
    for (const auto& segment : loaded.segments) {
        auto* host = memory.map(segment.guest_range, segment.protection);
        if (host == nullptr) {
            throw std::runtime_error("guest mmap failed for PT_LOAD segment");
        }
        if (segment.file_size > 0) {
            if (segment.file_offset + segment.file_size > image.size()) {
                throw std::runtime_error("ELF segment file range out of bounds");
            }
            std::memcpy(host, image.data() + segment.file_offset,
                        static_cast<std::size_t>(segment.file_size));
        }
    }
    return loaded;
}

}  // namespace neodroid
