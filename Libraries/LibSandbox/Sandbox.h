/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Span.h>
#include <AK/StringView.h>
#include <AK/Vector.h>

namespace Sandbox {

struct LandlockPath {
    enum class Access {
        ReadOnly,
        ReadAndExecute,
        ReadWrite,
    };

    ByteString path;
    Access access { Access::ReadOnly };
};

[[nodiscard]] ErrorOr<void> install_no_new_privileges();
[[nodiscard]] ErrorOr<void> configure_runtime();
[[nodiscard]] ErrorOr<void> add_landlock_path_if_exists(Vector<LandlockPath>& paths, StringView path, LandlockPath::Access);
[[nodiscard]] ErrorOr<void> restrict_filesystem_with_landlock(ReadonlySpan<LandlockPath>);
[[nodiscard]] ErrorOr<void> restrict_filesystem_with_landlock(ReadonlySpan<StringView> readable_paths = {});

}
