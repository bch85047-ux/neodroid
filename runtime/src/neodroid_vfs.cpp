#include "neodroid_vfs.h"

#include <algorithm>
#include <stdexcept>

namespace neodroid {

void VirtualFileSystem::mount(std::string guest_path, std::unique_ptr<VfsMount> mount) {
    if (!guest_path.empty() && guest_path.back() == '/') {
        guest_path.pop_back();
    }
    mounts_.push_back(MountPoint{std::move(guest_path), std::move(mount)});
    // Longest-prefix match first.
    std::sort(mounts_.begin(), mounts_.end(), [](const MountPoint& a, const MountPoint& b) {
        return a.path.size() > b.path.size();
    });
}

std::pair<VfsMount*, std::string> VirtualFileSystem::resolve(std::string_view guest_path) {
    for (auto& mount_point : mounts_) {
        if (guest_path.starts_with(mount_point.path)) {
            auto remainder = guest_path.substr(mount_point.path.size());
            if (!remainder.empty() && remainder.front() == '/') {
                remainder.remove_prefix(1);
            }
            return {mount_point.mount.get(), std::string(remainder)};
        }
    }
    throw std::runtime_error("no mount for guest path");
}

std::unique_ptr<VfsFile> VirtualFileSystem::open(std::string_view guest_path, int flags,
                                                 std::uint32_t mode) {
    auto [mount, relative] = resolve(guest_path);
    return mount->open(relative, flags, mode);
}

std::optional<VfsStat> VirtualFileSystem::stat(std::string_view guest_path) {
    auto [mount, relative] = resolve(guest_path);
    return mount->stat(relative);
}

std::vector<std::string> VirtualFileSystem::list(std::string_view guest_path) {
    auto [mount, relative] = resolve(guest_path);
    return mount->list(relative);
}

void VirtualFileSystem::install_android_mounts(const LayeredRootfsConfig& config) {
    // P1 will install EROFS-backed read-only bases and a COW writable overlay.
    (void)config;
}

}  // namespace neodroid
