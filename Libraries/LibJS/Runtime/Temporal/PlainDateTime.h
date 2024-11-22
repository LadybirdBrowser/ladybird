/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Temporal/ISORecords.h>

namespace JS::Temporal {

ISODateTime combine_iso_date_and_time_record(ISODate, Time);
bool iso_date_time_within_limits(ISODateTime);
ThrowCompletionOr<ISODateTime> interpret_temporal_date_time_fields(VM&, StringView calendar, CalendarFields&, Overflow);
ISODateTime balance_iso_date_time(double year, double month, double day, double hour, double minute, double second, double millisecond, double microsecond, double nanosecond);

}
