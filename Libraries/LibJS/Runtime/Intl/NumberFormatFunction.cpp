/*
 * Copyright (c) 2021-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intl/NumberFormat.h>
#include <LibJS/Runtime/Intl/NumberFormatFunction.h>

namespace JS::Intl {

GC_DEFINE_ALLOCATOR(NumberFormatFunction);

// 16.5.2 Number Format Functions, https://tc39.es/ecma402/#sec-number-format-functions
GC::Ref<NumberFormatFunction> NumberFormatFunction::create(Realm& realm, NumberFormat& number_format)
{
    return realm.create<NumberFormatFunction>(number_format, realm.intrinsics().function_prototype());
}

NumberFormatFunction::NumberFormatFunction(NumberFormat& number_format, Object& prototype)
    : NativeFunction(prototype)
    , m_number_format(number_format)
{
}

void NumberFormatFunction::initialize(Realm& realm)
{
    auto& vm = this->vm();

    Base::initialize(realm);
    define_direct_property(vm.names.length, Value(1), Attribute::Configurable);
    define_direct_property(vm.names.name, PrimitiveString::create(vm, String {}), Attribute::Configurable);
}

ThrowCompletionOr<Value> NumberFormatFunction::call()
{
    auto& vm = this->vm();

    // 1. Let nf be F.[[NumberFormat]].
    // 2. Assert: Type(nf) is Object and nf has an [[InitializedNumberFormat]] internal slot.
    // 3. If value is not provided, let value be undefined.
    auto value = vm.argument(0);

    // 4. Let x be ? ToIntlMathematicalValue(value).
    auto mathematical_value = TRY(to_intl_mathematical_value(vm, value));

    // 5. Return ? FormatNumeric(nf, x).
    auto formatted = format_numeric(m_number_format, move(mathematical_value));
    return PrimitiveString::create(vm, move(formatted));
}

void NumberFormatFunction::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_number_format);
}

}
