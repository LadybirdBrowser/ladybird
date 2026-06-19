/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
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
#endif

[[nodiscard]] ErrorOr<void> install_no_new_privileges();
[[nodiscard]] ErrorOr<void> configure_runtime();

#if defined(AK_OS_LINUX)
[[nodiscard]] ErrorOr<void> add_landlock_path_if_exists(Vector<LandlockPath>& paths, StringView path, LandlockPath::Access);
[[nodiscard]] ErrorOr<void> restrict_filesystem_with_landlock(ReadonlySpan<LandlockPath> = {});
#endif

#if defined(AK_OS_MACOS)
[[nodiscard]] ErrorOr<void> add_seatbelt_path_if_exists(Vector<SeatbeltPath>& paths, StringView path, SeatbeltPath::Access);
[[nodiscard]] ErrorOr<void> apply_macos_sandbox(ReadonlySpan<SeatbeltPath>, NetworkAccess, ReadonlySpan<ByteString> executable_paths = {});
#endif

}
