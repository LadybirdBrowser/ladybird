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

namespace Web::Infra {

bool is_ascii_case_insensitive_match(StringView a, StringView b);
String normalize_newlines(String const&);
ErrorOr<String> strip_and_collapse_whitespace(StringView string);
bool is_code_unit_prefix(StringView potential_prefix, StringView input);
ErrorOr<String> convert_to_scalar_value_string(StringView string);
ErrorOr<String> to_ascii_lowercase(StringView string);
ErrorOr<String> to_ascii_uppercase(StringView string);
ByteBuffer isomorphic_encode(StringView input);
String isomorphic_decode(ReadonlyBytes input);

}
