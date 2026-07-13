/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/StringView.h>
#include <AK/Utf16View.h>

namespace HTTP {

[[nodiscard]] bool is_method(StringView);
[[nodiscard]] bool is_method(Utf16View);
[[nodiscard]] bool is_cors_safelisted_method(StringView);
[[nodiscard]] bool is_cors_safelisted_method(Utf16View);
[[nodiscard]] bool is_forbidden_method(StringView);
[[nodiscard]] bool is_forbidden_method(Utf16View);
[[nodiscard]] ByteString normalize_method(StringView);
[[nodiscard]] ByteString normalize_method(Utf16View);

}
