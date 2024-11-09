/*
 * Copyright (c) 2023, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/DistinctNumeric.h>
#include <AK/Endian.h>
#include <AK/Function.h>
#include <AK/LexicalPath.h>
#include <AK/RedBlackTree.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>
#include <LibWasm/Forward.h>

namespace Wasm::Wasi::ABI {

// NOTE: The "real" ABI used in the wild is described by [api.h from libc-bottom-half](https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h)
//       This is *not* the same ABI as the one described in the WASI spec, nor is it the same ABI as api.h on wasi-libc/master.
// The highlights of the ABI are:
// - (most) structs are passed as pointers to heap.
// - arrays are fat pointers splat across two arguments
// - return object locations are also passed as arguments, the number of arguments depends on the return type itself:
//    - ArgsSizes / EnvironSizes / the return type of sock_recv use two arguments
//    - everything else is passed like a normal struct

template<auto impl>
struct InvocationOf {
    HostFunction operator()(Implementation&, StringView name);
};

template<typename T, size_t N>
void serialize(T const&, Array<Bytes, N>);

template<typename T, size_t N>
T deserialize(Array<ReadonlyBytes, N> const&);

template<typename T>
struct ToCompatibleValue {
    using Type = void;
};

template<typename T>
struct CompatibleValue {
    typename ToCompatibleValue<T>::Type value;

    Wasm::Value to_wasm_value() const;
};

template<typename T>
CompatibleValue<T> to_compatible_value(Wasm::Value const&);

template<typename T>
T deserialize(CompatibleValue<T> const&);

}

namespace Wasm::Wasi {

// NOTE: This is a copy of LittleEndian from Endian.h,
//       we can't use those because they have a packed attribute, and depend on it;
//       but we want proper alignment on these types.
template<typename T>
class alignas(T) LittleEndian {
public:
    constexpr LittleEndian() = default;

    constexpr LittleEndian(T value)
        : m_value(AK::convert_between_host_and_little_endian(value))
    {
    }

    constexpr operator T() const { return AK::convert_between_host_and_little_endian(m_value); }
    constexpr T value() const { return AK::convert_between_host_and_little_endian(m_value); }

    LittleEndian& operator+=(T other)
    {
        m_value = AK::convert_between_host_and_little_endian(AK::convert_between_host_and_little_endian(m_value) + other);
        return *this;
    }

    // This returns the internal representation. In this case, that is the value stored in little endian format.
    constexpr Bytes bytes() { return Bytes { &m_value, sizeof(m_value) }; }
    constexpr ReadonlyBytes bytes() const { return ReadonlyBytes { &m_value, sizeof(m_value) }; }

    void serialize_into(Array<Bytes, 1> bytes) const;
    static LittleEndian read_from(Array<ReadonlyBytes, 1> const& bytes);

private:
    T m_value { 0 };
};

using Size = LittleEndian<u32>;
using FileSize = LittleEndian<u64>;
using Timestamp = LittleEndian<u64>;

namespace Detail {
template<typename>
struct __Pointer_tag;
template<typename>
struct __ConstPointer_tag;
}

// NOTE: Might need to be updated if WASI ever supports memory64.
using UnderlyingPointerType = u32;

template<typename T>
using Pointer = DistinctNumeric<LittleEndian<UnderlyingPointerType>, Detail::__Pointer_tag<T>, AK::DistinctNumericFeature::Comparison>;
template<typename T>
using ConstPointer = DistinctNumeric<LittleEndian<UnderlyingPointerType>, Detail::__ConstPointer_tag<T>, AK::DistinctNumericFeature::Comparison>;

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L70
enum class ClockID : u32 {
    Realtime,
    Monotonic,
    ProcessCPUTimeID,
    ThreadCPUTimeID,
};

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L105
enum class Errno : u16 {
    Success,
    TooBig,
    Access,
    AddressInUse,
    AddressNotAvailable,
    AFNotSupported,
    Again,
    Already,
    BadF,
    BadMessage,
    Busy,
    Canceled,
    Child,
    ConnectionAborted,
    ConnectionRefused,
    ConnectionReset,
    Deadlock,
    DestinationAddressRequired,
    Domain,
    DQuot, // Reserved, Unused.
    Exist,
    Fault,
    FBig,
    HostUnreachable,
    IdentifierRemoved,
    IllegalSequence,
    InProgress,
    Interrupted,
    Invalid,
    IO,
    IsConnected,
    IsDirectory,
    Loop,
    MFile,
    MLink,
    MessageSize,
    MultiHop, // Reserved, Unused.
    NameTooLong,
    NetworkDown,
    NetworkReset,
    NetworkUnreachable,
    NFile,
    NoBufferSpace,
    NoDevice,
    NoEntry,
    NoExec,
    NoLock,
    NoLink,
    NoMemory,
    NoMessage,
    NoProtocolOption,
    NoSpace,
    NoSys,
    NotConnected,
    NotDirectory,
    NotEmpty,
    NotRecoverable,
    NotSocket,
    NotSupported,
    NoTTY,
    NXIO,
    Overflow,
    OwnerDead,
    Permission,
    Pipe,
    Protocol,
    ProtocolNotSupported,
    ProtocolType,
    Range,
    ReadOnlyFS,
    SPipe,
    SRCH,
    Stale,
    TimedOut,
    TextBusy,
    XDev,
    NotCapable,
};

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L498
struct Rights {
    using CompatibleType = u64;

    struct Bits {
        bool fd_datasync : 1;
        bool fd_read : 1;
        bool fd_seek : 1;
        bool fd_fdstat_set_flags : 1;
        bool fd_sync : 1;
        bool fd_tell : 1;
        bool fd_write : 1;
        bool fd_advise : 1;
        bool fd_allocate : 1;
        bool path_create_directory : 1;
        bool path_create_file : 1;
        bool path_link_source : 1;
        bool path_link_target : 1;
        bool path_open : 1;
        bool fd_readdir : 1;
        bool path_readlink : 1;
        bool path_rename_source : 1;
        bool path_rename_target : 1;
        bool path_filestat_get : 1;
        bool path_filestat_set_size : 1;
        bool path_filestat_set_times : 1;
        bool fd_filestat_get : 1;
        bool fd_filestat_set_size : 1;
        bool fd_filestat_set_times : 1;
        bool path_symlink : 1;
        bool path_remove_directory : 1;
        bool path_unlink_file : 1;
        bool poll_fd_readwrite : 1;
        bool sock_shutdown : 1;
        bool sock_accept : 1;

        u8 _unused1 : 2;
        u32 _unused2 : 32;
    };

    static_assert(sizeof(Bits) == sizeof(CompatibleType));

    union {
        Bits bits;
        LittleEndian<CompatibleType> data;
    };

    void serialize_into(Array<Bytes, 1> bytes) const;
    static Rights read_from(Array<ReadonlyBytes, 1> const& bytes);
};

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L663
using FD = DistinctNumeric<LittleEndian<u32>, struct __FD_tag, AK::DistinctNumericFeature::Comparison>;

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L671
struct IOVec {
    Pointer<u8> buf;
    Size buf_len;

    static IOVec read_from(Array<ReadonlyBytes, 1> const& bytes);
};

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L692
struct CIOVec {
    ConstPointer<u8> buf;
    Size buf_len;

    static CIOVec read_from(Array<ReadonlyBytes, 1> const& bytes);
};

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L713
using FileDelta = LittleEndian<i64>;

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L721
enum class Whence : u8 {
    Set,
    Cur,
    End,
};

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L746
using DirCookie = LittleEndian<u64>;

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L754
using DirNameLen = LittleEndian<u32>;

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L762
using INode = LittleEndian<u64>;

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L770
enum class FileType : u8 {
    Unknown,
    BlockDevice,
    CharacterDevice,
    Directory,
    RegularFile,
    SocketDGram,
    SocketStream,
    SymbolicLink,
};

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L818
struct DirEnt {
    DirCookie d_next;
    INode d_ino;
    DirNameLen d_namlen;
    FileType d_type;
    u8 _padding[3] { 0 }; // Not part of the API, but the struct is required to be 24 bytes - even though it has no explicit padding.
};
static_assert(sizeof(DirEnt) == 24);

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L851
enum class Advice : u8 {
    Normal,
    Sequential,
    Random,
    WillNeed,
    DontNeed,
    NoReuse,
};

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L889
struct FDFlags {
    using CompatibleType = u16;

    struct Bits {
        bool append : 1;
        bool dsync : 1;
        bool nonblock : 1;
        bool rsync : 1;
        bool sync : 1;

        u8 _unused1 : 3;
        u8 _unused2 : 8;
    };
    static_assert(sizeof(Bits) == sizeof(CompatibleType));

    union {
        Bits bits;
        LittleEndian<CompatibleType> data;
    };

    void serialize_into(Array<Bytes, 1> bytes) const;
    static FDFlags read_from(Array<ReadonlyBytes, 1> const& bytes);
};

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L924
struct FDStat {
    FileType fs_filetype;
    u8 _padding1 { 0 }; // Not part of the API.
    FDFlags fs_flags;
    u8 _padding2[4] { 0 }; // Not part of the API.
    Rights fs_rights_base;
    Rights fs_rights_inheriting;

    void serialize_into(Array<Bytes, 1> bytes) const;
};
static_assert(sizeof(FDStat) == 24);

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L959
using Device = LittleEndian<u64>;

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L967
struct FSTFlags {
    using CompatibleType = u16;

    struct Bits {
        bool atim : 1;
        bool atim_now : 1;
        bool mtim : 1;
        bool mtim_now : 1;

        u8 _unused1 : 4;
        u8 _unused2 : 8;
    };

    static_assert(sizeof(Bits) == sizeof(CompatibleType));

    union {
        Bits bits;
        LittleEndian<CompatibleType> data;
    };

    void serialize_into(Array<Bytes, 1> bytes) const;
    static FSTFlags read_from(Array<ReadonlyBytes, 1> const& bytes);
};
static_assert(sizeof(FSTFlags) == 2);

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L995
struct LookupFlags {
    using CompatibleType = u32;

    struct Bits {
        bool symlink_follow : 1;

        u8 _unused1 : 7;
        u8 _unused2 : 8;
        u16 _unused3 : 16;
    };

    static_assert(sizeof(Bits) == sizeof(CompatibleType));

    union {
        Bits bits;
        LittleEndian<CompatibleType> data;
    };

    void serialize_into(Array<Bytes, 1> bytes) const;
    static LookupFlags read_from(Array<ReadonlyBytes, 1> const& bytes);
};
static_assert(sizeof(LookupFlags) == 4);

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L1008
struct OFlags {
    using CompatibleType = u16;

    struct Bits {
        bool creat : 1;
        bool directory : 1;
        bool excl : 1;
        bool trunc : 1;

        u8 _unused1 : 4;
        u8 _unused2 : 8;
    };

    static_assert(sizeof(Bits) == sizeof(CompatibleType));

    union {
        Bits bits;
        LittleEndian<CompatibleType> data;
    };

    static OFlags read_from(Array<ReadonlyBytes, 1> const& bytes);
};
static_assert(sizeof(OFlags) == 2);

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L1036
using LinkCount = LittleEndian<u64>;

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L1044
struct FileStat {
    Device dev;
    INode ino;
    FileType filetype;
    u8 _padding1[7] { 0 }; // Not part of the API.
    LinkCount nlink;
    FileSize size;
    Timestamp atim;
    Timestamp mtim;
    Timestamp ctim;

    void serialize_into(Array<Bytes, 1> bytes) const;
};
static_assert(sizeof(FileStat) == 64);

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L1102
using UserData = LittleEndian<u64>;

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L1110
enum class EventType : u8 {
    Clock,
    FDRead,
    FDWrite,
};

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L1137
struct EventRWFlags {
    using CompatibleType = u16;

    struct Bits {
        bool fd_readwrite_hangup : 1;

        u8 _unused1 : 7;
        u8 _unused2 : 8;
    };

    static_assert(sizeof(Bits) == sizeof(CompatibleType));

    union {
        Bits bits;
        LittleEndian<CompatibleType> data;
    };

    void serialize_into(Array<Bytes, 1> bytes) const;
    static EventRWFlags read_from(Array<ReadonlyBytes, 1> const& bytes);
};

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L1151
struct EventFDReadWrite {
    FileSize nbytes;
    u8 _padding[4] { 0 }; // Not part of the API.
    EventRWFlags flags;

    void serialize_into(Array<Bytes, 1> bytes) const;
    static EventFDReadWrite read_from(Array<ReadonlyBytes, 1> const& bytes);
};
static_assert(sizeof(EventFDReadWrite) == 16);

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L1186
struct Event {
    UserData userdata;
    Errno errno_;
    EventType type;
    u8 _padding[5] { 0 }; // Not part of the API.
    EventFDReadWrite fd_readwrite;

    void serialize_into(Array<Bytes, 1> bytes) const;
    static Event read_from(Array<ReadonlyBytes, 1> const& bytes);
};
static_assert(sizeof(Event) == 32);

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L1220
struct SubClockFlags {
    using CompatibleType = u16;

    struct Bits {
        bool subscription_clock_abstime : 1;

        u8 _unused1 : 7;
        u8 _unused2 : 8;
    };
    static_assert(sizeof(Bits) == sizeof(CompatibleType));

    union {
        Bits bits;
        LittleEndian<CompatibleType> data;
    };

    void serialize_into(Array<Bytes, 1> bytes) const;
    static SubClockFlags read_from(Array<ReadonlyBytes, 1> const& bytes);
};

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L1237
struct SubscriptionClock {
    ClockID id;
    u8 _padding1[4] { 0 }; // Not part of the API.
    Timestamp timeout;
    Timestamp precision;
    SubClockFlags flags;
    u8 _padding2[4] { 0 }; // Not part of the API.

    void serialize_into(Array<Bytes, 1> bytes) const;
    static SubscriptionClock read_from(Array<ReadonlyBytes, 1> const& bytes);
};
static_assert(sizeof(SubscriptionClock) == 32);

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L1272
struct SubscriptionFDReadWrite {
    FD file_descriptor;

    void serialize_into(Array<Bytes, 1> bytes) const;
    static SubscriptionFDReadWrite read_from(Array<ReadonlyBytes, 1> const& bytes);
};

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L1287
union SubscriptionU {
    SubscriptionClock clock;
    SubscriptionFDReadWrite fd_read;
    SubscriptionFDReadWrite fd_write;
};

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L1306
struct Subscription {
    UserData userdata;
    EventType type;
    u8 _padding[7] { 0 }; // Not part of the API.
    SubscriptionU u;

    void serialize_into(Array<Bytes, 1> bytes) const;
    static Subscription read_from(Array<ReadonlyBytes, 1> const& bytes);
};
static_assert(sizeof(Subscription) == 48);

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L1334
using ExitCode = LittleEndian<u32>;

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L1342
enum class Signal : u8 {
    None,
    HUP,
    INT,
    QUIT,
    ILL,
    TRAP,
    ABRT,
    BUS,
    FPE,
    KILL,
    USR1,
    SEGV,
    USR2,
    PIPE,
    ALRM,
    TERM,
    CHLD,
    CONT,
    STOP,
    TSTP,
    TTIN,
    TTOU,
    URG,
    XCPU,
    XFSZ,
    VTALRM,
    PROF,
    WINCH,
    POLL,
    PWR,
    SYS,
};

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L1536
struct RIFlags {
    using CompatibleType = u16;

    struct Bits {
        bool recv_peek : 1;
        bool recv_waitall : 1;

        u8 _unused1 : 6;
        u8 _unused2 : 8;
    };
    static_assert(sizeof(Bits) == sizeof(CompatibleType));

    union {
        Bits bits;
        LittleEndian<CompatibleType> data;
    };

    static RIFlags read_from(Array<ReadonlyBytes, 1> const& bytes);
};

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L1554
struct ROFlags {
    using CompatibleType = u16;

    struct Bits {
        bool recv_data_truncated : 1;

        u8 _unused1 : 7;
        u8 _unused2 : 8;
    };
    static_assert(sizeof(Bits) == sizeof(CompatibleType));

    union {
        Bits bits;
        LittleEndian<CompatibleType> data;
    };

    void serialize_into(Array<Bytes, 1> bytes) const;
};

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L1568
using SIFlags = LittleEndian<u16>;

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L1576
struct SDFlags {
    using CompatibleType = u8;

    struct Bits {
        bool rd : 1;
        bool wr : 1;

        u8 _unused : 6;
    };
    static_assert(sizeof(Bits) == sizeof(CompatibleType));

    union {
        Bits bits;
        LittleEndian<CompatibleType> data;
    };

    void serialize_into(Array<Bytes, 1> bytes) const;
    static SDFlags read_from(Array<ReadonlyBytes, 1> const& bytes);
};

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L1594
enum class PreOpenType : u8 {
    Dir,
};

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L1607
struct PreStatDir {
    Size pr_name_len;

    void serialize_into(Array<Bytes, 1> bytes) const;
    static PreStatDir read_from(Array<ReadonlyBytes, 1> const& bytes);
};

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L1636
struct PreStat {
    PreOpenType type;
    u8 _padding[3] { 0 }; // Not part of the API.
    union {
        PreStatDir dir;
    };

    void serialize_into(Array<Bytes, 1> bytes) const;
};
static_assert(sizeof(PreStat) == 8);

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L1676
struct ArgsSizes {
    Size count;
    Size size;

    using SerializationComponents = TypeList<Size, Size>;

    void serialize_into(Array<Bytes, 2> bytes) const;
};

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L1708
struct EnvironSizes {
    Size count;
    Size size;

    using SerializationComponents = TypeList<Size, Size>;

    void serialize_into(Array<Bytes, 2> bytes) const;
};

// https://github.com/WebAssembly/wasi-libc/blob/2c2fc9a2fddd0927a66f1c142e65c8dab6f5c5d7/libc-bottom-half/headers/public/wasi/api.h#L2664
struct SockRecvResult {
    Size size;
    ROFlags roflags;
    u8 _padding[2] { 0 }; // Not part of the API.

    using SerializationComponents = TypeList<Size, ROFlags>;

    void serialize_into(Array<Bytes, 2> bytes) const;
};
static_assert(sizeof(SockRecvResult) == 8);

template<typename TResult, typename Tag = u32>
struct Result {
    Result(TResult&& result)
        : bits {}
        , tag(0)
    {
        new (&bits) TResult(move(result));
    }

    Result(Errno&& error)
        : bits {}
        , tag(1)
    {
        new (&bits) Errno(error);
    }

    Optional<TResult&> result() const
    {
        if (tag == 0)
            return *bit_cast<TResult*>(&bits[0]);
        return {};
    }

    Optional<Errno&> error() const
    {
        if (tag == 1)
            return *bit_cast<Errno*>(&bits[0]);
        return {};
    }

    bool is_error() const { return tag == 1; }

    template<size_t N>
    Errno serialize_into(Array<Bytes, N>&& spans) const
    {
        if (tag == 1)
            return error().value();

        ABI::serialize(*result(), move(spans));
        return Errno::Success;
    }

private:
    alignas(max(alignof(TResult), alignof(Errno))) u8 bits[max(sizeof(TResult), sizeof(Errno))];
    LittleEndian<Tag> tag;
};

template<typename Tag>
struct Result<void, Tag> {
    Result()
        : error_bits {}
        , tag(0)
    {
    }

    Result(Errno&& error)
        : error_bits {}
        , tag(1)
    {
        new (&error_bits) Errno(error);
    }

    Optional<Empty> result() const
    {
        if (tag == 0)
            return { Empty {} };
        return {};
    }
    Optional<Errno&> error() const
    {
        if (tag == 1)
            return *bit_cast<Errno*>(&error_bits[0]);
        return {};
    }
    bool is_error() const { return tag == 1; }

private:
    alignas(Errno) u8 error_bits[sizeof(Errno)];
    LittleEndian<Tag> tag;
};

struct Implementation {
    struct MappedPath {
        LexicalPath host_path;
        LexicalPath mapped_path;
        mutable Optional<int> opened_fd {};
    };

    struct Details {
        Function<Vector<AK::String>()> provide_arguments;
        Function<Vector<AK::String>()> provide_environment;
        Function<Vector<MappedPath>()> provide_preopened_directories;
        int stdin_fd { 0 };
        int stdout_fd { 1 };
        int stderr_fd { 2 };
    };

    explicit Implementation(Details&& details)
        : provide_arguments(move(details.provide_arguments))
        , provide_environment(move(details.provide_environment))
        , provide_preopened_directories(move(details.provide_preopened_directories))
    {
        // Map all of std{in,out,err} by default.
        m_fd_map.insert(0, details.stdin_fd);
        m_fd_map.insert(1, details.stdout_fd);
        m_fd_map.insert(2, details.stderr_fd);
    }

    ErrorOr<HostFunction> function_by_name(StringView);

private:
    template<auto impl>
    HostFunction invocation_of(StringView name) { return ABI::InvocationOf<impl> {}(*this, name); }

    ErrorOr<Result<void>> impl$args_get(Configuration&, Pointer<Pointer<u8>> argv, Pointer<u8> argv_buf);
    ErrorOr<Result<ArgsSizes>> impl$args_sizes_get(Configuration&);
    ErrorOr<Result<void>> impl$environ_get(Configuration&, Pointer<Pointer<u8>> environ, Pointer<u8> environ_buf);
    ErrorOr<Result<EnvironSizes>> impl$environ_sizes_get(Configuration&);
    ErrorOr<Result<Timestamp>> impl$clock_res_get(Configuration&, ClockID id);
    ErrorOr<Result<Timestamp>> impl$clock_time_get(Configuration&, ClockID id, Timestamp precision);
    ErrorOr<Result<void>> impl$fd_advise(Configuration&, FD, FileSize offset, FileSize len, Advice);
    ErrorOr<Result<void>> impl$fd_allocate(Configuration&, FD, FileSize offset, FileSize len);
    ErrorOr<Result<void>> impl$fd_close(Configuration&, FD);
    ErrorOr<Result<void>> impl$fd_datasync(Configuration&, FD);
    ErrorOr<Result<FDStat>> impl$fd_fdstat_get(Configuration&, FD);
    ErrorOr<Result<void>> impl$fd_fdstat_set_flags(Configuration&, FD, FDFlags);
    ErrorOr<Result<void>> impl$fd_fdstat_set_rights(Configuration&, FD, Rights fs_rights_base, Rights fs_rights_inheriting);
    ErrorOr<Result<FileStat>> impl$fd_filestat_get(Configuration&, FD);
    ErrorOr<Result<void>> impl$fd_filestat_set_size(Configuration&, FD, FileSize);
    ErrorOr<Result<void>> impl$fd_filestat_set_times(Configuration&, FD, Timestamp atim, Timestamp mtim, FSTFlags);
    ErrorOr<Result<Size>> impl$fd_pread(Configuration&, FD, Pointer<IOVec> iovs, Size iovs_len, FileSize offset);
    ErrorOr<Result<PreStat>> impl$fd_prestat_get(Configuration&, FD);
    ErrorOr<Result<void>> impl$fd_prestat_dir_name(Configuration&, FD, Pointer<u8> path, Size path_len);
    ErrorOr<Result<Size>> impl$fd_pwrite(Configuration&, FD, Pointer<CIOVec> iovs, Size iovs_len, FileSize offset);
    ErrorOr<Result<Size>> impl$fd_read(Configuration&, FD, Pointer<IOVec> iovs, Size iovs_len);
    ErrorOr<Result<Size>> impl$fd_readdir(Configuration&, FD, Pointer<u8> buf, Size buf_len, DirCookie cookie);
    ErrorOr<Result<void>> impl$fd_renumber(Configuration&, FD from, FD to);
    ErrorOr<Result<FileSize>> impl$fd_seek(Configuration&, FD, FileDelta offset, Whence whence);
    ErrorOr<Result<void>> impl$fd_sync(Configuration&, FD);
    ErrorOr<Result<FileSize>> impl$fd_tell(Configuration&, FD);
    ErrorOr<Result<Size>> impl$fd_write(Configuration&, FD, Pointer<CIOVec> iovs, Size iovs_len);
    ErrorOr<Result<void>> impl$path_create_directory(Configuration&, FD, Pointer<u8> path, Size path_len);
    ErrorOr<Result<FileStat>> impl$path_filestat_get(Configuration&, FD, LookupFlags, ConstPointer<u8> path, Size path_len);
    ErrorOr<Result<void>> impl$path_filestat_set_times(Configuration&, FD, LookupFlags, Pointer<u8> path, Size path_len, Timestamp atim, Timestamp mtim, FSTFlags);
    ErrorOr<Result<void>> impl$path_link(Configuration&, FD, LookupFlags, Pointer<u8> old_path, Size old_path_len, FD, Pointer<u8> new_path, Size new_path_len);
    ErrorOr<Result<FD>> impl$path_open(Configuration&, FD, LookupFlags, Pointer<u8> path, Size path_len, OFlags, Rights fs_rights_base, Rights fs_rights_inheriting, FDFlags fd_flags);
    ErrorOr<Result<Size>> impl$path_readlink(Configuration&, FD, LookupFlags, Pointer<u8> path, Size path_len, Pointer<u8> buf, Size buf_len);
    ErrorOr<Result<void>> impl$path_remove_directory(Configuration&, FD, Pointer<u8> path, Size path_len);
    ErrorOr<Result<void>> impl$path_rename(Configuration&, FD, Pointer<u8> old_path, Size old_path_len, FD, Pointer<u8> new_path, Size new_path_len);
    ErrorOr<Result<void>> impl$path_symlink(Configuration&, Pointer<u8> old_path, Size old_path_len, FD, Pointer<u8> new_path, Size new_path_len);
    ErrorOr<Result<void>> impl$path_unlink_file(Configuration&, FD, Pointer<u8> path, Size path_len);
    ErrorOr<Result<Size>> impl$poll_oneoff(Configuration&, ConstPointer<Subscription> in, Pointer<Event> out, Size nsubscriptions);
    ErrorOr<void> impl$proc_exit(Configuration&, ExitCode);
    ErrorOr<Result<void>> impl$proc_raise(Configuration&, Signal);
    ErrorOr<Result<void>> impl$sched_yield(Configuration&);
    ErrorOr<Result<void>> impl$random_get(Configuration&, Pointer<u8> buf, Size buf_len);
    ErrorOr<Result<FD>> impl$sock_accept(Configuration&, FD fd, FDFlags fd_flags);
    ErrorOr<Result<SockRecvResult>> impl$sock_recv(Configuration&, FD fd, Pointer<IOVec> ri_data, Size ri_data_len, RIFlags ri_flags);
    ErrorOr<Result<Size>> impl$sock_send(Configuration&, FD fd, Pointer<CIOVec> si_data, Size ri_data_len, SIFlags si_flags);
    ErrorOr<Result<void>> impl$sock_shutdown(Configuration&, FD fd, SDFlags how);

    Vector<AK::String> const& arguments() const;
    Vector<AK::String> const& environment() const;
    Vector<MappedPath> const& preopened_directories() const;

    using PreopenedDirectoryDescriptor = DistinctNumeric<LittleEndian<size_t>, struct PreopenedDirectoryDescriptor_tag, AK::DistinctNumericFeature::Comparison, AK::DistinctNumericFeature::CastToUnderlying, AK::DistinctNumericFeature::Increment>;
    using UnmappedDescriptor = DistinctNumeric<LittleEndian<size_t>, struct UnmappedDescriptor_tag, AK::DistinctNumericFeature::Comparison, AK::DistinctNumericFeature::CastToUnderlying>;
    using MappedDescriptor = Variant<u32, PreopenedDirectoryDescriptor>;
    using Descriptor = Variant<u32, PreopenedDirectoryDescriptor, UnmappedDescriptor>;

    Descriptor map_fd(FD);

public:
    Function<Vector<AK::String>()> provide_arguments;
    Function<Vector<AK::String>()> provide_environment;
    Function<Vector<MappedPath>()> provide_preopened_directories;

private:
    struct Cache {
        Optional<Vector<AK::String>> cached_arguments;
        Optional<Vector<AK::String>> cached_environment;
        Optional<Vector<MappedPath>> cached_preopened_directories;
    };

    mutable Cache cache {};

    RedBlackTree<u32, MappedDescriptor> m_fd_map;
    size_t m_first_unmapped_preopened_directory_index { 0 };
};

#undef IMPL

}

namespace Wasm::Wasi::ABI {

template<typename T, typename... Args>
struct ToCompatibleValue<DistinctNumeric<T, Args...>> {
    using Type = typename ToCompatibleValue<T>::Type;
};

template<typename T>
struct ToCompatibleValue<LittleEndian<T>> {
    using Type = MakeSigned<T>;
};

template<typename T>
requires(requires { declval<typename T::CompatibleType>(); })
struct ToCompatibleValue<T> {
    using Type = MakeSigned<typename T::CompatibleType>;
};

template<Integral T>
struct ToCompatibleValue<T> {
    using Type = MakeSigned<T>;
};

template<Enum T>
struct ToCompatibleValue<T> {
    using Type = MakeSigned<UnderlyingType<T>>;
};

}

template<typename T>
struct AK::Formatter<Wasm::Wasi::LittleEndian<T>> : AK::Formatter<T> {
    ErrorOr<void> format(FormatBuilder& builder, Wasm::Wasi::LittleEndian<T> value)
    {
        return Formatter<T>::format(builder, value.operator T());
    }
};
