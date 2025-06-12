/*
 * Copyright (c) 2021-2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/DateConstructor.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intl/DateTimeFormat.h>
#include <LibJS/Runtime/Intl/DateTimeFormatFunction.h>
#include <LibJS/Runtime/ValueInlines.h>

namespace JS::Intl {

GC_DEFINE_ALLOCATOR(DateTimeFormatFunction);

// 11.5.4 DateTime Format Functions, https://tc39.es/ecma402/#sec-datetime-format-functions
// 15.9.3 DateTime Format Functions, https://tc39.es/proposal-temporal/#sec-datetime-format-functions
GC::Ref<DateTimeFormatFunction> DateTimeFormatFunction::create(Realm& realm, DateTimeFormat& date_time_format)
{
    return realm.create<DateTimeFormatFunction>(date_time_format, realm.intrinsics().function_prototype());
}

DateTimeFormatFunction::DateTimeFormatFunction(DateTimeFormat& date_time_format, Object& prototype)
    : NativeFunction(prototype)
    , m_date_time_format(date_time_format)
{
}

void DateTimeFormatFunction::initialize(Realm& realm)
{
    auto& vm = this->vm();

    Base::initialize(realm);
    define_direct_property(vm.names.length, Value(1), Attribute::Configurable);
    define_direct_property(vm.names.name, PrimitiveString::create(vm, String {}), Attribute::Configurable);
}

ThrowCompletionOr<Value> DateTimeFormatFunction::call()
{
    auto& vm = this->vm();
    auto& realm = *vm.current_realm();

    auto date_value = vm.argument(0);

    // 1. Let dtf be F.[[DateTimeFormat]].
    // 2. Assert: Type(dtf) is Object and dtf has an [[InitializedDateTimeFormat]] internal slot.

    FormattableDateTime date { 0 };

    // 3. If date is not provided or is undefined, then
    if (date_value.is_undefined()) {
        // a. Let x be ! Call(%Date.now%, undefined).
        date = MUST(JS::call(vm, *realm.intrinsics().date_constructor_now_function(), js_undefined())).as_double();
    }
    // 4. Else,
    else {
        // a. Let x be ? ToDateTimeFormattable(date).
        date = TRY(to_date_time_formattable(vm, date_value));
    }

    // 5. Return ? FormatDateTime(dtf, x).
    auto formatted = TRY(format_date_time(vm, m_date_time_format, date));
    return PrimitiveString::create(vm, move(formatted));
}

void DateTimeFormatFunction::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_date_time_format);
}

}
