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
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/ISORecords.h>

namespace JS::Temporal {

class PlainDate final : public Object {
    JS_OBJECT(PlainDate, Object);
    GC_DECLARE_ALLOCATOR(PlainDate);

public:
    virtual ~PlainDate() override = default;

    [[nodiscard]] ISODate iso_date() const { return m_iso_date; }
    [[nodiscard]] String const& calendar() const { return m_calendar; }

private:
    PlainDate(ISODate, String calendar, Object& prototype);

    ISODate m_iso_date; // [[ISODate]]
    String m_calendar;  // [[Calendar]]
};

ISODate create_iso_date_record(double year, double month, double day);
ThrowCompletionOr<GC::Ref<PlainDate>> to_temporal_date(VM& vm, Value item, Value options = js_undefined());
ThrowCompletionOr<GC::Ref<PlainDate>> create_temporal_date(VM&, ISODate, String calendar, GC::Ptr<FunctionObject> new_target = {});
bool iso_date_surpasses(i8 sign, double year1, double month1, double day1, ISODate iso_date2);
ThrowCompletionOr<ISODate> regulate_iso_date(VM& vm, double year, double month, double day, Overflow overflow);
bool is_valid_iso_date(double year, double month, double day);
ISODate balance_iso_date(double year, double month, double day);
String pad_iso_year(i32 year);
String temporal_date_to_string(PlainDate const&, ShowCalendar);
bool iso_date_within_limits(ISODate);
i8 compare_iso_date(ISODate, ISODate);
ThrowCompletionOr<GC::Ref<Duration>> difference_temporal_plain_date(VM&, DurationOperation, PlainDate const&, Value other, Value options);
ThrowCompletionOr<GC::Ref<PlainDate>> add_duration_to_date(VM&, ArithmeticOperation, PlainDate const&, Value temporal_duration_like, Value options);

}
