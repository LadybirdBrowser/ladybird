/*
 * Copyright (c) 2021-2022, Tim Flynn <trflynn89@serenityos.org>
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

JS_DEFINE_ALLOCATOR(DateTimeFormatFunction);

// 11.5.4 DateTime Format Functions, https://tc39.es/ecma402/#sec-datetime-format-functions
NonnullGCPtr<DateTimeFormatFunction> DateTimeFormatFunction::create(Realm& realm, DateTimeFormat& date_time_format)
{
    return realm.heap().allocate<DateTimeFormatFunction>(realm, date_time_format, realm.intrinsics().function_prototype());
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

    auto date = vm.argument(0);

    // 1. Let dtf be F.[[DateTimeFormat]].
    // 2. Assert: Type(dtf) is Object and dtf has an [[InitializedDateTimeFormat]] internal slot.

    double date_value;

    // 3. If date is not provided or is undefined, then
    if (date.is_undefined()) {
        // a. Let x be ! Call(%Date.now%, undefined).
        date_value = MUST(JS::call(vm, *realm.intrinsics().date_constructor_now_function(), js_undefined())).as_double();
    }
    // 4. Else,
    else {
        // a. Let x be ? ToNumber(date).
        date_value = TRY(date.to_number(vm)).as_double();
    }

    // 5. Return ? FormatDateTime(dtf, x).
    auto formatted = TRY(format_date_time(vm, m_date_time_format, date_value));
    return PrimitiveString::create(vm, move(formatted));
}

void DateTimeFormatFunction::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_date_time_format);
}

}
