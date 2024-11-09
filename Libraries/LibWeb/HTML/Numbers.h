/*
 * Copyright (c) 2023, Jonatan Klemets <jonatan.r.klemets@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/String.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

Optional<i32> parse_integer(StringView string);

Optional<u32> parse_non_negative_integer(StringView string);

Optional<double> parse_floating_point_number(StringView string);

bool is_valid_floating_point_number(StringView string);

WebIDL::ExceptionOr<String> convert_non_negative_integer_to_string(JS::Realm&, WebIDL::Long);

}
