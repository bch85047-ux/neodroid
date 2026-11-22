#include "neodroid_runtime.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

namespace {

class TestMemory final : public neodroid::GuestMemory {
public:
    void* map(neodroid::GuestRange range, neodroid::MemoryProtection) override {
        auto& block = blocks_[range.start];
        block.assign(range.size, std::byte{0});
        return block.data();
    }
    bool unmap(neodroid::GuestRange range) override {
        return blocks_.erase(range.start) > 0;
    }
    bool protect(neodroid::GuestRange, neodroid::MemoryProtection) override {
        return true;
    }
    bool copy_to_guest(neodroid::GuestAddress address, const void* source,
                       neodroid::GuestSize size) override {
        for (auto& [start, block] : blocks_) {
            if (address >= start && address + size <= start + block.size()) {
                std::memcpy(block.data() + (address - start), source, size);
                return true;
            }
        }
        return false;
    }
    bool copy_from_guest(void* destination, neodroid::GuestAddress address,
                         neodroid::GuestSize size) const override {
        for (const auto& [start, block] : blocks_) {
            if (address >= start && address + size <= start + block.size()) {
                std::memcpy(destination, block.data() + (address - start), size);
                return true;
            }
        }
        return false;
    }
    bool is_mapped(neodroid::GuestRange range) const override {
        return blocks_.find(range.start) != blocks_.end();
    }

private:
    std::map<neodroid::GuestAddress, std::vector<std::byte>> blocks_;
};

}  // namespace

int main() {
    neodroid::Runtime runtime({});
    auto memory = std::make_unique<TestMemory>();
    auto* memory_raw = memory.get();
    runtime.set_memory(std::move(memory));

    neodroid::GuestRegisters regs{};
    regs.x[8] = static_cast<std::uint64_t>(neodroid::Syscall::GetTid);
    auto result = runtime.dispatch_syscall(regs);
    assert(result == 1);
    std::printf("[OK] gettid -> %lld\n", static_cast<long long>(result));

    constexpr std::uint64_t test_ts = 0x4000;
    memory_raw->map({test_ts, 16}, neodroid::MemoryProtection::Write);
    regs = {};
    regs.x[8] = static_cast<std::uint64_t>(neodroid::Syscall::ClockGetTime);
    regs.x[0] = 1;
    regs.x[1] = test_ts;
    result = runtime.dispatch_syscall(regs);
    assert(result == 0);
    struct Timespec { std::int64_t sec; std::int64_t nsec; } ts{};
    memory_raw->copy_from_guest(&ts, test_ts, sizeof(ts));
    std::printf("[OK] clock_gettime sec=%lld nsec=%lld\n",
                static_cast<long long>(ts.sec), static_cast<long long>(ts.nsec));

    regs = {};
    regs.x[8] = 9999;
    result = runtime.dispatch_syscall(regs);
    assert(result == neodroid::kGuestEnosys);
    std::printf("[OK] unknown syscall -> ENOSYS\n");

    std::printf("\nNeoDroid P0 runtime tests passed\n");
    return 0;
}
