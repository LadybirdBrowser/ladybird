/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Vector.h>

namespace RequestServer {

[[nodiscard]] ErrorOr<void> apply_sandbox(StringView mach_server_name, Vector<ByteString> const& certificates, StringView cache_path);

}
