/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>

namespace JS::Temporal {

// 3.5.1 ISO Date Records, https://tc39.es/proposal-temporal/#sec-temporal-iso-date-records
struct ISODate {
    i32 year { 0 };
    u8 month { 0 };
    u8 day { 0 };
};

ISODate create_iso_date_record(double year, double month, double day);
ThrowCompletionOr<ISODate> regulate_iso_date(VM& vm, double year, double month, double day, Overflow overflow);
bool is_valid_iso_date(double year, double month, double day);
String pad_iso_year(i32 year);
bool iso_date_within_limits(ISODate);

}
