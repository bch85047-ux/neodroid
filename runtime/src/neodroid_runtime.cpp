#include "neodroid_runtime.h"

#include <chrono>
#include <stdexcept>

namespace neodroid {
namespace {

class HostClock final : public ClockSource {
public:
    std::uint64_t monotonic_ns() const override {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    }

    std::uint64_t realtime_ns() const override {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    }
};

SyscallResult syscall_clock_gettime(Runtime& runtime, GuestRegisters& regs) {
    return runtime.fast_clock_gettime(regs);
}

SyscallResult syscall_gettid(Runtime& runtime, GuestRegisters& regs) {
    return runtime.fast_gettid(regs);
}

SyscallResult syscall_futex(Runtime& runtime, GuestRegisters& regs) {
    return runtime.fast_futex(regs);
}

SyscallResult syscall_epoll(Runtime& runtime, GuestRegisters& regs) {
    return runtime.fast_epoll(regs);
}

}  // namespace

Runtime::Runtime(Config config) : config_(config), clock_(std::make_unique<HostClock>()) {
    install_fast_syscalls();
}

void Runtime::install_fast_syscalls() {
    const auto add = [this](Syscall syscall, std::string_view name,
                            SyscallHandler handler) {
        syscalls_.emplace(static_cast<std::uint32_t>(syscall),
                          SyscallDescriptor{static_cast<std::uint32_t>(syscall),
                                            name, handler, true});
    };

    add(Syscall::ClockGetTime, "clock_gettime", syscall_clock_gettime);
    add(Syscall::GetTid, "gettid", syscall_gettid);
    add(Syscall::Futex, "futex", syscall_futex);
    add(Syscall::EpollPWait, "epoll_pwait", syscall_epoll);
}

SyscallResult Runtime::dispatch_syscall(GuestRegisters& registers) {
    const auto number = static_cast<std::uint32_t>(registers.syscall_number());
    const auto it = syscalls_.find(number);
    if (it == syscalls_.end()) {
        registers.set_result(kGuestEnosys);
        return kGuestEnosys;
    }

    const auto result = it->second.handler(*this, registers);
    registers.set_result(result);
    return result;
}

SyscallResult Runtime::fast_clock_gettime(GuestRegisters& registers) {
    // x0: clockid, x1: struct timespec* guest pointer
    const auto guest_pointer = registers.argument(1);
    struct GuestTimespec {
        std::int64_t seconds;
        std::int64_t nanoseconds;
    };

    const auto nanoseconds = clock_->monotonic_ns();
    const GuestTimespec output{
        static_cast<std::int64_t>(nanoseconds / 1'000'000'000ULL),
        static_cast<std::int64_t>(nanoseconds % 1'000'000'000ULL),
    };

    if (!memory_ || !memory_->copy_to_guest(guest_pointer, &output, sizeof(output))) {
        return kGuestEfault;
    }
    return 0;
}

SyscallResult Runtime::fast_gettid(GuestRegisters&) {
    return current_tid_;
}

SyscallResult Runtime::fast_futex(GuestRegisters& registers) {
    if (!futexes_) {
        return kGuestEnosys;
    }

    constexpr std::uint32_t kFutexWake = 1;
    const auto address = registers.argument(0);
    const auto operation = static_cast<std::uint32_t>(registers.argument(1)) & 0x7fU;
    const auto value = static_cast<std::uint32_t>(registers.argument(2));

    if (operation == kFutexWake) {
        return futexes_->wake(address, static_cast<std::int32_t>(value));
    }
    return kGuestEnosys;
}

SyscallResult Runtime::fast_epoll(GuestRegisters&) {
    // Implemented by the iOS host with dispatch sources / kqueue-equivalent.
    return kGuestEnosys;
}

LoadedElf Runtime::load_program(std::string_view) {
    throw std::runtime_error("NeoDroid P0: VFS-backed ELF loading pending");
}

GuestPid Runtime::spawn(std::string_view, std::span<const std::string>,
                        std::span<const std::string>) {
    throw std::runtime_error("NeoDroid P0: guest process spawning pending");
}

void Runtime::set_memory(std::unique_ptr<GuestMemory> memory) {
    memory_ = std::move(memory);
}

void Runtime::set_clock(std::unique_ptr<ClockSource> clock) {
    clock_ = std::move(clock);
}

void Runtime::set_futexes(std::unique_ptr<FutexTable> futexes) {
    futexes_ = std::move(futexes);
}

}  // namespace neodroid
