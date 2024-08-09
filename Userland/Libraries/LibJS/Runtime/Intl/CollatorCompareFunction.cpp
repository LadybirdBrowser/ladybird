/*
 * Copyright (c) 2022, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf8View.h>
#include <LibJS/Runtime/Intl/Collator.h>
#include <LibJS/Runtime/Intl/CollatorCompareFunction.h>
#include <LibJS/Runtime/VM.h>

namespace JS::Intl {

JS_DEFINE_ALLOCATOR(CollatorCompareFunction);

NonnullGCPtr<CollatorCompareFunction> CollatorCompareFunction::create(Realm& realm, Collator& collator)
{
    return realm.heap().allocate<CollatorCompareFunction>(realm, realm, collator);
}

CollatorCompareFunction::CollatorCompareFunction(Realm& realm, Collator& collator)
    : NativeFunction(realm.intrinsics().function_prototype())
    , m_collator(collator)
{
}

void CollatorCompareFunction::initialize(Realm&)
{
    auto& vm = this->vm();
    define_direct_property(vm.names.length, Value(2), Attribute::Configurable);
    define_direct_property(vm.names.name, PrimitiveString::create(vm, String {}), Attribute::Configurable);
}

// 10.3.3.2 CompareStrings ( collator, x, y ), https://tc39.es/ecma402/#sec-collator-comparestrings
double compare_strings(Collator& collator, Utf8View const& x, Utf8View const& y)
{
    // FIXME: Implement https://unicode.org/reports/tr10
    (void)collator;
    auto x_iterator = x.begin();
    auto y_iterator = y.begin();
    for (; x_iterator != x.end() && y_iterator != y.end(); ++x_iterator, ++y_iterator) {
        if (*x_iterator != *y_iterator)
            return static_cast<double>(*x_iterator) - static_cast<double>(*y_iterator);
    }
    if (x_iterator != x.end())
        return 1.0;
    if (y_iterator != y.end())
        return -1.0;
    return 0.0;
}

// 10.3.3.1 Collator Compare Functions, https://tc39.es/ecma402/#sec-collator-compare-functions
ThrowCompletionOr<Value> CollatorCompareFunction::call()
{
    auto& vm = this->vm();

    // 1. Let collator be F.[[Collator]].
    // 2. Assert: Type(collator) is Object and collator has an [[InitializedCollator]] internal slot.
    // 3. If x is not provided, let x be undefined.
    // 4. If y is not provided, let y be undefined.

    // 5. Let X be ? ToString(x).
    auto x = TRY(vm.argument(0).to_string(vm));
    // 6. Let Y be ? ToString(y).
    auto y = TRY(vm.argument(1).to_string(vm));

    // 7. Return CompareStrings(collator, X, Y).
    return compare_strings(m_collator, x.code_points(), y.code_points());
}

void CollatorCompareFunction::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_collator);
}

}
