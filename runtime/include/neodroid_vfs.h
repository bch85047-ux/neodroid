#pragma once

#include "neodroid_types.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace neodroid {

enum class VfsNodeType { Directory, RegularFile, Symlink, CharacterDevice, Socket };

struct VfsStat {
    std::uint64_t inode{};
    std::uint32_t mode{};
    std::uint32_t uid{};
    std::uint32_t gid{};
    std::uint64_t size{};
    std::uint64_t mtime_ns{};
    VfsNodeType type{VfsNodeType::RegularFile};
};

class VfsFile {
public:
    virtual ~VfsFile() = default;
    virtual SyscallResult read(GuestAddress buffer, GuestSize count) = 0;
    virtual SyscallResult write(GuestAddress buffer, GuestSize count) = 0;
    virtual SyscallResult seek(std::int64_t offset, int whence) = 0;
    virtual VfsStat stat() const = 0;
};

class VfsMount {
public:
    virtual ~VfsMount() = default;
    virtual std::unique_ptr<VfsFile> open(std::string_view relative_path,
                                          int flags, std::uint32_t mode) = 0;
    virtual std::optional<VfsStat> stat(std::string_view relative_path) = 0;
    virtual std::vector<std::string> list(std::string_view relative_path) = 0;
};

// Layered mount: update > writable overlay > read-only base.
struct LayeredRootfsConfig {
    std::string base_manifest;
    std::string base_root;
    std::string update_root;
    std::string writable_root;
    bool verify_manifest{true};
};

class VirtualFileSystem {
public:
    void mount(std::string guest_path, std::unique_ptr<VfsMount> mount);
    std::unique_ptr<VfsFile> open(std::string_view guest_path, int flags,
                                  std::uint32_t mode = 0);
    std::optional<VfsStat> stat(std::string_view guest_path);
    std::vector<std::string> list(std::string_view guest_path);

    // Standard Android mount topology.
    void install_android_mounts(const LayeredRootfsConfig& config);

private:
    struct MountPoint {
        std::string path;
        std::unique_ptr<VfsMount> mount;
    };
    std::vector<MountPoint> mounts_;
    std::pair<VfsMount*, std::string> resolve(std::string_view guest_path);
};

}  // namespace neodroid
