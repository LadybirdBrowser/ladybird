/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Object.h>

namespace JS::Temporal {

class PlainMonthDay final : public Object {
    JS_OBJECT(PlainMonthDay, Object);
    JS_DECLARE_ALLOCATOR(PlainMonthDay);

public:
    virtual ~PlainMonthDay() override = default;

    [[nodiscard]] i32 iso_year() const { return m_iso_year; }
    [[nodiscard]] u8 iso_month() const { return m_iso_month; }
    [[nodiscard]] u8 iso_day() const { return m_iso_day; }
    [[nodiscard]] Object const& calendar() const { return m_calendar; }
    [[nodiscard]] Object& calendar() { return m_calendar; }

private:
    PlainMonthDay(u8 iso_month, u8 iso_day, i32 iso_year, Object& calendar, Object& prototype);

    virtual void visit_edges(Visitor&) override;

    // 10.4 Properties of Temporal.PlainMonthDay Instances, https://tc39.es/proposal-temporal/#sec-properties-of-temporal-plainmonthday-instances
    i32 m_iso_year { 0 };            // [[ISOYear]]
    u8 m_iso_month { 0 };            // [[ISOMonth]]
    u8 m_iso_day { 0 };              // [[ISODay]]
    NonnullGCPtr<Object> m_calendar; // [[Calendar]]
};

struct ISOMonthDay {
    u8 month;
    u8 day;
    i32 reference_iso_year;
};

ThrowCompletionOr<PlainMonthDay*> to_temporal_month_day(VM&, Value item, Object const* options = nullptr);
ThrowCompletionOr<PlainMonthDay*> create_temporal_month_day(VM&, u8 iso_month, u8 iso_day, Object& calendar, i32 reference_iso_year, FunctionObject const* new_target = nullptr);
ThrowCompletionOr<String> temporal_month_day_to_string(VM&, PlainMonthDay&, StringView show_calendar);

}
