/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>
#include <LibJS/Runtime/Temporal/ISORecords.h>
#include <LibJS/Runtime/Temporal/PlainDate.h>

namespace JS::Temporal {

class PlainYearMonth final : public Object {
    JS_OBJECT(PlainYearMonth, Object);
    GC_DECLARE_ALLOCATOR(PlainYearMonth);

public:
    virtual ~PlainYearMonth() override = default;

    [[nodiscard]] ISODate iso_date() const { return m_iso_date; }
    [[nodiscard]] String const& calendar() const { return m_calendar; }

private:
    PlainYearMonth(ISODate, String calendar, Object& prototype);

    ISODate m_iso_date; // [[ISODate]]
    String m_calendar;  // [[Calendar]]
};

ThrowCompletionOr<GC::Ref<PlainYearMonth>> to_temporal_year_month(VM&, Value item, Value options = js_undefined());
bool iso_year_month_within_limits(ISODate);
ISOYearMonth balance_iso_year_month(double year, double month);
ThrowCompletionOr<GC::Ref<PlainYearMonth>> create_temporal_year_month(VM&, ISODate, String calendar, GC::Ptr<FunctionObject> new_target = {});
String temporal_year_month_to_string(PlainYearMonth const&, ShowCalendar);
ThrowCompletionOr<GC::Ref<Duration>> difference_temporal_plain_year_month(VM&, DurationOperation, PlainYearMonth const&, Value other, Value options);
ThrowCompletionOr<GC::Ref<PlainYearMonth>> add_duration_to_year_month(VM&, ArithmeticOperation, PlainYearMonth const&, Value temporal_duration_like, Value options);

}
