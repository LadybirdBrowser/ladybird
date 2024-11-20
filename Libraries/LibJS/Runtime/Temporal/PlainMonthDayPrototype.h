/*
 * Copyright (c) 2021, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/PrototypeObject.h>
#include <LibJS/Runtime/Temporal/PlainMonthDay.h>

namespace JS::Temporal {

class PlainMonthDayPrototype final : public PrototypeObject<PlainMonthDayPrototype, PlainMonthDay> {
    JS_PROTOTYPE_OBJECT(PlainMonthDayPrototype, PlainMonthDay, Temporal.PlainMonthDay);
    GC_DECLARE_ALLOCATOR(PlainMonthDayPrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~PlainMonthDayPrototype() override = default;

private:
    explicit PlainMonthDayPrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(calendar_id_getter);
    JS_DECLARE_NATIVE_FUNCTION(month_code_getter);
    JS_DECLARE_NATIVE_FUNCTION(day_getter);
    JS_DECLARE_NATIVE_FUNCTION(with);
    JS_DECLARE_NATIVE_FUNCTION(equals);
    JS_DECLARE_NATIVE_FUNCTION(to_string);
    JS_DECLARE_NATIVE_FUNCTION(to_locale_string);
    JS_DECLARE_NATIVE_FUNCTION(to_json);
};

}
