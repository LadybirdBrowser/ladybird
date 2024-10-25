/*
 * Copyright (c) 2021, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/PrototypeObject.h>
#include <LibJS/Runtime/Temporal/PlainYearMonth.h>

namespace JS::Temporal {

class PlainYearMonthPrototype final : public PrototypeObject<PlainYearMonthPrototype, PlainYearMonth> {
    JS_PROTOTYPE_OBJECT(PlainYearMonthPrototype, PlainYearMonth, Temporal.PlainYearMonth);
    JS_DECLARE_ALLOCATOR(PlainYearMonthPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~PlainYearMonthPrototype() override = default;

private:
    explicit PlainYearMonthPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(calendar_getter);
    JS_DECLARE_NATIVE_FUNCTION(calendar_id_getter);
    JS_DECLARE_NATIVE_FUNCTION(year_getter);
    JS_DECLARE_NATIVE_FUNCTION(month_getter);
    JS_DECLARE_NATIVE_FUNCTION(month_code_getter);
    JS_DECLARE_NATIVE_FUNCTION(days_in_year_getter);
    JS_DECLARE_NATIVE_FUNCTION(days_in_month_getter);
    JS_DECLARE_NATIVE_FUNCTION(months_in_year_getter);
    JS_DECLARE_NATIVE_FUNCTION(in_leap_year_getter);
    JS_DECLARE_NATIVE_FUNCTION(era_getter);
    JS_DECLARE_NATIVE_FUNCTION(era_year_getter);
    JS_DECLARE_NATIVE_FUNCTION(with);
    JS_DECLARE_NATIVE_FUNCTION(add);
    JS_DECLARE_NATIVE_FUNCTION(subtract);
    JS_DECLARE_NATIVE_FUNCTION(until);
    JS_DECLARE_NATIVE_FUNCTION(since);
    JS_DECLARE_NATIVE_FUNCTION(equals);
    JS_DECLARE_NATIVE_FUNCTION(to_string);
    JS_DECLARE_NATIVE_FUNCTION(to_locale_string);
    JS_DECLARE_NATIVE_FUNCTION(to_json);
    JS_DECLARE_NATIVE_FUNCTION(value_of);
    JS_DECLARE_NATIVE_FUNCTION(to_plain_date);
    JS_DECLARE_NATIVE_FUNCTION(get_iso_fields);
};

}
