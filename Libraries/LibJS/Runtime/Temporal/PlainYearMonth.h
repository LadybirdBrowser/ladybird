/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>

namespace JS::Temporal {

class PlainYearMonth final : public Object {
    JS_OBJECT(PlainYearMonth, Object);
    JS_DECLARE_ALLOCATOR(PlainYearMonth);

public:
    virtual ~PlainYearMonth() override = default;

    [[nodiscard]] i32 iso_year() const { return m_iso_year; }
    [[nodiscard]] u8 iso_month() const { return m_iso_month; }
    [[nodiscard]] u8 iso_day() const { return m_iso_day; }
    [[nodiscard]] Object const& calendar() const { return m_calendar; }
    [[nodiscard]] Object& calendar() { return m_calendar; }

private:
    PlainYearMonth(i32 iso_year, u8 iso_month, u8 iso_day, Object& calendar, Object& prototype);

    virtual void visit_edges(Visitor&) override;

    // 9.4 Properties of Temporal.PlainYearMonth Instances, https://tc39.es/proposal-temporal/#sec-properties-of-temporal-plainyearmonth-instances
    i32 m_iso_year { 0 };            // [[ISOYear]]
    u8 m_iso_month { 0 };            // [[ISOMonth]]
    u8 m_iso_day { 0 };              // [[ISODay]]
    NonnullGCPtr<Object> m_calendar; // [[Calendar]]
};

struct ISOYearMonth {
    i32 year;
    u8 month;
    u8 reference_iso_day;
};

ThrowCompletionOr<PlainYearMonth*> to_temporal_year_month(VM&, Value item, Object const* options = nullptr);
ThrowCompletionOr<ISOYearMonth> regulate_iso_year_month(VM&, double year, double month, StringView overflow);
bool iso_year_month_within_limits(i32 year, u8 month);
ISOYearMonth balance_iso_year_month(double year, double month);
ThrowCompletionOr<PlainYearMonth*> create_temporal_year_month(VM&, i32 iso_year, u8 iso_month, Object& calendar, u8 reference_iso_day, FunctionObject const* new_target = nullptr);
ThrowCompletionOr<String> temporal_year_month_to_string(VM&, PlainYearMonth&, StringView show_calendar);
ThrowCompletionOr<NonnullGCPtr<Duration>> difference_temporal_plain_year_month(VM&, DifferenceOperation, PlainYearMonth&, Value other, Value options);
ThrowCompletionOr<PlainYearMonth*> add_duration_to_or_subtract_duration_from_plain_year_month(VM&, ArithmeticOperation, PlainYearMonth&, Value temporal_duration_like, Value options_value);

}
