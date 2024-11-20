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
#include <LibJS/Runtime/Temporal/PlainDate.h>

namespace JS::Temporal {

class PlainMonthDay final : public Object {
    JS_OBJECT(PlainMonthDay, Object);
    GC_DECLARE_ALLOCATOR(PlainMonthDay);

public:
    virtual ~PlainMonthDay() override = default;

    [[nodiscard]] ISODate iso_date() const { return m_iso_date; }
    [[nodiscard]] String const& calendar() const { return m_calendar; }

private:
    PlainMonthDay(ISODate, String calendar, Object& prototype);

    ISODate m_iso_date; // [[ISODate]]
    String m_calendar;  // [[Calendar]]
};

ThrowCompletionOr<GC::Ref<PlainMonthDay>> to_temporal_month_day(VM&, Value item, Value options = js_undefined());
ThrowCompletionOr<GC::Ref<PlainMonthDay>> create_temporal_month_day(VM&, ISODate, String calendar, GC::Ptr<FunctionObject> new_target = {});

}
