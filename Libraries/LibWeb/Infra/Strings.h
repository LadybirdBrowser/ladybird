/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, networkException <networkexception@serenityos.org>
 * Copyright (c) 2023, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibWeb/Export.h>

namespace Web::Infra {

WEB_API String normalize_newlines(String const&);
WEB_API Utf16String normalize_newlines(Utf16String const&);
WEB_API ErrorOr<String> strip_and_collapse_whitespace(StringView string);
Utf16String strip_and_collapse_whitespace(Utf16String const& string);
WEB_API bool is_code_unit_prefix(StringView potential_prefix, StringView input);
WEB_API ErrorOr<String> convert_to_scalar_value_string(StringView string);
ByteBuffer isomorphic_encode(StringView input);
WEB_API String isomorphic_decode(ReadonlyBytes input);
bool code_unit_less_than(StringView a, StringView b);

}
