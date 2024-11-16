/*
 * Copyright (c) 2020-2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Time.h>

namespace AK {

enum class HumanReadableBasedOn {
    Base2,
    Base10
};

enum class UseThousandsSeparator {
    Yes,
    No
};

String human_readable_size(u64 size, HumanReadableBasedOn based_on = HumanReadableBasedOn::Base2, UseThousandsSeparator use_thousands_separator = UseThousandsSeparator::No);
String human_readable_quantity(u64 quantity, HumanReadableBasedOn based_on = HumanReadableBasedOn::Base2, StringView unit = "B"sv, UseThousandsSeparator use_thousands_separator = UseThousandsSeparator::No);

String human_readable_size_long(u64 size, UseThousandsSeparator use_thousands_separator = UseThousandsSeparator::No);
String human_readable_time(Duration);
String human_readable_digital_time(i64 time_in_seconds);

}

#if USING_AK_GLOBALLY
using AK::human_readable_digital_time;
using AK::human_readable_quantity;
using AK::human_readable_size;
using AK::human_readable_size_long;
using AK::human_readable_time;
using AK::UseThousandsSeparator;
#endif
