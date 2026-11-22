#pragma once

#include "neodroid_types.h"

#include <array>
#include <functional>
#include <string_view>

namespace neodroid {

// Linux AArch64 syscall numbers used by Android/bionic.
enum class Syscall : std::uint32_t {
    IoSetup = 0,
    GetXAttr = 8,
    SetXAttr = 5,
    Ioctl = 29,
    OpenAt = 56,
    Close = 57,
    Pipe2 = 59,
    GetDents64 = 61,
    Lseek = 62,
    Read = 63,
    Write = 64,
    ReadV = 65,
    WriteV = 66,
    PRead64 = 67,
    PWrite64 = 68,
    PSelect6 = 72,
    PPoll = 73,
    Signalfd4 = 74,
    Vmsplice = 75,
    Splice = 76,
    Tee = 77,
    ReadLinkAt = 78,
    NewFStatAt = 79,
    FStat = 80,
    Sync = 81,
    FSync = 82,
    FDataSync = 83,
    SyncFileRange = 84,
    TimerFdCreate = 85,
    TimerFdSetTime = 86,
    TimerFdGetTime = 87,
    UTimeNsAt = 88,
    Exit = 93,
    ExitGroup = 94,
    SetTidAddress = 96,
    Futex = 98,
    NanoSleep = 101,
    ClockGetTime = 113,
    ClockNanoSleep = 115,
    SchedYield = 124,
    Kill = 129,
    TGKill = 131,
    RtSigAction = 134,
    RtSigProcMask = 135,
    RtSigReturn = 139,
    Times = 153,
    UName = 160,
    GetRLimit = 163,
    GetTimeOfDay = 169,
    GetPid = 172,
    GetPpid = 173,
    GetUid = 174,
    GetEuid = 175,
    GetGid = 176,
    GetEgid = 177,
    GetTid = 178,
    SysInfo = 179,
    Socket = 198,
    SocketPair = 199,
    Bind = 200,
    Listen = 201,
    Accept = 202,
    Connect = 203,
    GetSockName = 204,
    GetPeerName = 205,
    SendTo = 206,
    RecvFrom = 207,
    SetSockOpt = 208,
    GetSockOpt = 209,
    Shutdown = 210,
    Brk = 214,
    Munmap = 215,
    MRemap = 216,
    Clone = 220,
    Execve = 221,
    Mmap = 222,
    MProtect = 226,
    MSync = 227,
    MLock = 228,
    MUnlock = 229,
    MAdvise = 233,
    Wait4 = 260,
    PrLimit64 = 261,
    GetRandom = 278,
    MemFdCreate = 279,
    Bpf = 280,
    ExecveAt = 281,
    Statx = 291,
    RSeq = 293,
    PidFdSendSignal = 424,
    IoUringSetup = 425,
    OpenAt2 = 437,
    EpollCreate1 = 20,
    EpollCtl = 21,
    EpollPWait = 22,
    Dup = 23,
    Dup3 = 24,
    Fcntl = 25,
};

constexpr SyscallResult kGuestEinval = -22;
constexpr SyscallResult kGuestEnosys = -38;
constexpr SyscallResult kGuestEbadf = -9;
constexpr SyscallResult kGuestEfault = -14;
constexpr SyscallResult kGuestEagain = -11;
constexpr SyscallResult kGuestEintr = -4;

class Runtime;
using SyscallHandler = SyscallResult (*)(Runtime&, GuestRegisters&);

struct SyscallDescriptor {
    std::uint32_t number;
    std::string_view name;
    SyscallHandler handler;
    bool fast_path;
};

}  // namespace neodroid
