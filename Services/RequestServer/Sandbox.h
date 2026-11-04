/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Vector.h>

namespace RequestServer {

// The disk cache directory is the only place on disk this process may write to, and it has none without a disk cache.
[[nodiscard]] ErrorOr<void> apply_sandbox(StringView mach_server_name, Vector<ByteString> const& certificates, Optional<StringView> disk_cache_directory);

}
