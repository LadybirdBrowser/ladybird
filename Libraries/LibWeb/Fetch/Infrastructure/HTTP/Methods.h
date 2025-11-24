/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/StringView.h>
#include <LibWeb/Export.h>

namespace Web::Fetch::Infrastructure {

[[nodiscard]] bool is_method(StringView);
[[nodiscard]] WEB_API bool is_cors_safelisted_method(StringView);
[[nodiscard]] bool is_forbidden_method(StringView);
[[nodiscard]] ByteString normalize_method(StringView);

}
