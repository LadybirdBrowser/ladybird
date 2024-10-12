/*
 * Copyright (c) 2023, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/String.h>
#include <LibJS/Runtime/Date.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::HTML {

struct Time {
    u32 hours { 0 };
    u32 minutes { 0 };
    double seconds { 0 };
};

u32 week_number_of_the_last_day(u64 year);
bool is_valid_week_string(StringView value);
bool is_valid_month_string(StringView value);
bool is_valid_date_string(StringView value);
WebIDL::ExceptionOr<JS::NonnullGCPtr<JS::Date>> parse_date_string(JS::Realm& realm, StringView value);
bool is_valid_local_date_and_time_string(StringView value);
String normalize_local_date_and_time_string(String const& value);
bool is_valid_time_string(StringView value);
WebIDL::ExceptionOr<JS::NonnullGCPtr<JS::Date>> parse_time_string_to_date(JS::Realm& realm, StringView value);
Optional<Time> parse_time_string(StringView value);
Optional<double> parse_time_string_to_number(StringView value);
String time_as_number_to_string(double time);
}
