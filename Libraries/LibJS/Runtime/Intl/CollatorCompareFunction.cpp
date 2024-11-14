/*
 * Copyright (c) 2022, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf8View.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intl/Collator.h>
#include <LibJS/Runtime/Intl/CollatorCompareFunction.h>

namespace JS::Intl {

GC_DEFINE_ALLOCATOR(CollatorCompareFunction);

GC::Ref<CollatorCompareFunction> CollatorCompareFunction::create(Realm& realm, Collator& collator)
{
    return realm.create<CollatorCompareFunction>(realm, collator);
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

void CollatorCompareFunction::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_collator);
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
    return compare_strings(m_collator, x, y);
}

// 10.3.3.2 CompareStrings ( collator, x, y ), https://tc39.es/ecma402/#sec-collator-comparestrings
int compare_strings(Collator const& collator, StringView x, StringView y)
{
    auto result = collator.collator().compare(x, y);

    // The result is intended to correspond with a sort order of String values according to the effective locale and
    // collation options of collator, and will be negative when x is ordered before y, positive when x is ordered after
    // y, and zero in all other cases (representing no relative ordering between x and y).
    switch (result) {
    case Unicode::Collator::Order::Before:
        return -1;
    case Unicode::Collator::Order::Equal:
        return 0;
    case Unicode::Collator::Order::After:
        return 1;
    }

    VERIFY_NOT_REACHED();
}

}
