/*
 * Copyright (c) 2021, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/PrototypeObject.h>
#include <LibJS/Runtime/Temporal/PlainMonthDay.h>

namespace JS::Temporal {

class PlainMonthDayPrototype final : public PrototypeObject<PlainMonthDayPrototype, PlainMonthDay> {
    JS_PROTOTYPE_OBJECT(PlainMonthDayPrototype, PlainMonthDay, Temporal.PlainMonthDay);
    JS_DECLARE_ALLOCATOR(PlainMonthDayPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~PlainMonthDayPrototype() override = default;

private:
    explicit PlainMonthDayPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(calendar_getter);
    JS_DECLARE_NATIVE_FUNCTION(calendar_id_getter);
    JS_DECLARE_NATIVE_FUNCTION(month_code_getter);
    JS_DECLARE_NATIVE_FUNCTION(day_getter);
    JS_DECLARE_NATIVE_FUNCTION(with);
    JS_DECLARE_NATIVE_FUNCTION(equals);
    JS_DECLARE_NATIVE_FUNCTION(to_string);
    JS_DECLARE_NATIVE_FUNCTION(to_locale_string);
    JS_DECLARE_NATIVE_FUNCTION(to_json);
    JS_DECLARE_NATIVE_FUNCTION(value_of);
    JS_DECLARE_NATIVE_FUNCTION(to_plain_date);
    JS_DECLARE_NATIVE_FUNCTION(get_iso_fields);
};

}
