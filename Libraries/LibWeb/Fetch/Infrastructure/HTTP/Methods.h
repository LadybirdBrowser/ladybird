/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>

namespace Web::Fetch::Infrastructure {

[[nodiscard]] bool is_method(ReadonlyBytes);
[[nodiscard]] bool is_cors_safelisted_method(ReadonlyBytes);
[[nodiscard]] bool is_forbidden_method(ReadonlyBytes);
[[nodiscard]] ByteBuffer normalize_method(ReadonlyBytes);

}
