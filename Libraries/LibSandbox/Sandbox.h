/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/EnumBits.h>
#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/Platform.h>
#include <AK/Span.h>
#include <AK/StringView.h>
#include <AK/Vector.h>

namespace Sandbox {

#if defined(AK_OS_LINUX)
struct LandlockPath {
    enum class Access {
        ReadOnly,
        ReadAndExecute,
        ReadWrite,
    };

    ByteString path;
    Access access { Access::ReadOnly };
    bool is_directory { false };
};
#endif

#if defined(AK_OS_MACOS)
struct SeatbeltPath {
    enum class Access {
        ReadOnly,
        ReadAndExecute,
        ReadWrite,
    };

    ByteString path;
    Access access { Access::ReadOnly };
    bool is_directory { false };
};

enum class NetworkAccess {
    Denied,
    Allowed,
};

// System services that a helper may use, in addition to its own Browser endpoint.
enum class SystemService : u8 {
    None = 0,
    Fonts = 1 << 0,
    Audio = 1 << 1,
    VideoDecoding = 1 << 2,
    GPU = 1 << 3,
    IOSurface = 1 << 4,
    // Mapping MAP_JIT memory, for WebAssembly code compiled by Cranelift.
    JIT = 1 << 5,
    CodecEnumeration = 1 << 6,
};
AK_ENUM_BITWISE_OPERATORS(SystemService);

struct SeatbeltProfile {
    ReadonlySpan<SeatbeltPath> paths {};
    NetworkAccess network_access { NetworkAccess::Denied };
    ReadonlySpan<ByteString> executable_paths {};
    ReadonlySpan<StringView> iokit_user_client_classes {};

    // The bootstrap name of the Browser endpoint that the helper connects to after installing the sandbox.
    StringView mach_server_name {};
    SystemService system_services { SystemService::None };
};
#endif

[[nodiscard]] ErrorOr<void> install_no_new_privileges();
[[nodiscard]] ErrorOr<void> configure_runtime();

#if defined(AK_OS_LINUX)
[[nodiscard]] ErrorOr<void> add_landlock_path_if_exists(Vector<LandlockPath>& paths, StringView path, LandlockPath::Access);
[[nodiscard]] ErrorOr<void> restrict_filesystem_with_landlock(ReadonlySpan<LandlockPath> = {});
#endif

#if defined(AK_OS_MACOS)
[[nodiscard]] ErrorOr<void> add_seatbelt_path_if_exists(Vector<SeatbeltPath>& paths, StringView path, SeatbeltPath::Access);
[[nodiscard]] ErrorOr<void> apply_macos_sandbox(SeatbeltProfile const&);

// Returns the .app bundle that contains the executable, if the executable is in the bundle's Contents/MacOS directory.
[[nodiscard]] Optional<ByteString> application_bundle_for_executable(StringView executable_path);
#endif

}
