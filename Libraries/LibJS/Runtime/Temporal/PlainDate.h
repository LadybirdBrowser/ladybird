/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Temporal/AbstractOperations.h>

namespace JS::Temporal {

class PlainDate final : public Object {
    JS_OBJECT(PlainDate, Object);
    JS_DECLARE_ALLOCATOR(PlainDate);

public:
    virtual ~PlainDate() override = default;

    [[nodiscard]] i32 iso_year() const { return m_iso_year; }
    [[nodiscard]] u8 iso_month() const { return m_iso_month; }
    [[nodiscard]] u8 iso_day() const { return m_iso_day; }
    [[nodiscard]] Object const& calendar() const { return m_calendar; }
    [[nodiscard]] Object& calendar() { return m_calendar; }

private:
    PlainDate(i32 iso_year, u8 iso_month, u8 iso_day, Object& calendar, Object& prototype);

    virtual void visit_edges(Visitor&) override;

    // 3.4 Properties of Temporal.PlainDate Instances, https://tc39.es/proposal-temporal/#sec-properties-of-temporal-plaindate-instances
    i32 m_iso_year { 0 };            // [[ISOYear]]
    u8 m_iso_month { 1 };            // [[ISOMonth]]
    u8 m_iso_day { 1 };              // [[ISODay]]
    NonnullGCPtr<Object> m_calendar; // [[Calendar]]
};

// 3.5.1 ISO Date Records, https://tc39.es/proposal-temporal/#sec-temporal-iso-date-records
struct ISODateRecord {
    i32 year;
    u8 month;
    u8 day;
};

ISODateRecord create_iso_date_record(i32 year, u8 month, u8 day);
ThrowCompletionOr<PlainDate*> create_temporal_date(VM&, i32 iso_year, u8 iso_month, u8 iso_day, Object& calendar, FunctionObject const* new_target = nullptr);
ThrowCompletionOr<PlainDate*> to_temporal_date(VM&, Value item, Object const* options = nullptr);
DateDurationRecord difference_iso_date(VM&, i32 year1, u8 month1, u8 day1, i32 year2, u8 month2, u8 day2, StringView largest_unit);
ThrowCompletionOr<ISODateRecord> regulate_iso_date(VM&, double year, double month, double day, StringView overflow);
bool is_valid_iso_date(i32 year, u8 month, u8 day);
ISODateRecord balance_iso_date(double year, double month, double day);
ThrowCompletionOr<String> pad_iso_year(VM&, i32 y);
ThrowCompletionOr<String> temporal_date_to_string(VM&, PlainDate&, StringView show_calendar);
ThrowCompletionOr<ISODateRecord> add_iso_date(VM&, i32 year, u8 month, u8 day, double years, double months, double weeks, double days, StringView overflow);
i8 compare_iso_date(i32 year1, u8 month1, u8 day1, i32 year2, u8 month2, u8 day2);
ThrowCompletionOr<NonnullGCPtr<Duration>> difference_temporal_plain_date(VM&, DifferenceOperation, PlainDate&, Value other, Value options);
ThrowCompletionOr<NonnullGCPtr<Duration>> difference_date(VM& vm, CalendarMethods const&, PlainDate const& one, PlainDate const& two, Object const& options);

}
