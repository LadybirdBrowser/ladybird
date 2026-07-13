/*
 * Copyright (c) 2023, Jonatan Klemets <jonatan.r.klemets@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Utf16String.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

WEB_API Optional<i32> parse_integer(Utf16View string);
Optional<Utf16View> parse_integer_digits(Utf16View string);

WEB_API Optional<u32> parse_non_negative_integer(Utf16View string);
Optional<Utf16View> parse_non_negative_integer_digits(Utf16View string);

WEB_API Optional<double> parse_floating_point_number(Utf16View string);

WEB_API bool is_valid_floating_point_number(Utf16View string);

WEB_API WebIDL::ExceptionOr<Utf16String> convert_non_negative_integer_to_string(JS::Realm&, WebIDL::Long);

}
