/*
 * Copyright (c) 2021-2022, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Intl/Locale.h>
#include <LibJS/Runtime/PrototypeObject.h>

namespace JS::Intl {

class LocalePrototype final : public PrototypeObject<LocalePrototype, Locale> {
    JS_PROTOTYPE_OBJECT(LocalePrototype, Locale, Intl.Locale);
    JS_DECLARE_ALLOCATOR(LocalePrototype);

public:
    virtual void initialize(Realm&) override;
    virtual ~LocalePrototype() override = default;

private:
    explicit LocalePrototype(Realm&);

    JS_DECLARE_NATIVE_FUNCTION(maximize);
    JS_DECLARE_NATIVE_FUNCTION(minimize);
    JS_DECLARE_NATIVE_FUNCTION(to_string);

    JS_DECLARE_NATIVE_FUNCTION(base_name);
    JS_DECLARE_NATIVE_FUNCTION(calendar);
    JS_DECLARE_NATIVE_FUNCTION(case_first);
    JS_DECLARE_NATIVE_FUNCTION(collation);
    JS_DECLARE_NATIVE_FUNCTION(first_day_of_week);
    JS_DECLARE_NATIVE_FUNCTION(hour_cycle);
    JS_DECLARE_NATIVE_FUNCTION(numbering_system);
    JS_DECLARE_NATIVE_FUNCTION(numeric);
    JS_DECLARE_NATIVE_FUNCTION(language);
    JS_DECLARE_NATIVE_FUNCTION(script);
    JS_DECLARE_NATIVE_FUNCTION(region);
    JS_DECLARE_NATIVE_FUNCTION(get_calendars);
    JS_DECLARE_NATIVE_FUNCTION(get_collations);
    JS_DECLARE_NATIVE_FUNCTION(get_hour_cycles);
    JS_DECLARE_NATIVE_FUNCTION(get_numbering_systems);
    JS_DECLARE_NATIVE_FUNCTION(get_time_zones);
    JS_DECLARE_NATIVE_FUNCTION(get_text_info);
    JS_DECLARE_NATIVE_FUNCTION(get_week_info);
};

}
