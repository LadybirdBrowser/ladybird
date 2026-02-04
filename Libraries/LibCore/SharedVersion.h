/*
 * Copyright (c) 2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Types.h>
#include <LibCore/AnonymousBuffer.h>

namespace Core {

using SharedVersion = u64;
using SharedVersionIndex = u32;

constexpr inline SharedVersion INVALID_SHARED_VERSION = 0;
constexpr inline SharedVersion INITIAL_SHARED_VERSION = 1;

[[nodiscard]] AnonymousBuffer create_shared_version_buffer();
[[nodiscard]] bool initialize_shared_version(AnonymousBuffer&, SharedVersionIndex);
void increment_shared_version(AnonymousBuffer&, SharedVersionIndex);
Optional<SharedVersion> get_shared_version(AnonymousBuffer const&, SharedVersionIndex);

}
