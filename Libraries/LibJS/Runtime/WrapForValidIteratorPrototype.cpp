/*
 * Copyright (c) 2023-2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/WrapForValidIteratorPrototype.h>

namespace JS {

JS_DEFINE_ALLOCATOR(WrapForValidIteratorPrototype);

// 27.1.3.2.1.1 The %WrapForValidIteratorPrototype% Object, https://tc39.es/ecma262/#sec-%wrapforvaliditeratorprototype%-object
WrapForValidIteratorPrototype::WrapForValidIteratorPrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().iterator_prototype())
{
}

void WrapForValidIteratorPrototype::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.next, next, 0, attr);
    define_native_function(realm, vm.names.return_, return_, 0, attr);
}

// 27.1.3.2.1.1.1 %WrapForValidIteratorPrototype%.next ( ), https://tc39.es/ecma262/#sec-%wrapforvaliditeratorprototype%.next
JS_DEFINE_NATIVE_FUNCTION(WrapForValidIteratorPrototype::next)
{
    // 1. Let O be this value.
    // 2. Perform ? RequireInternalSlot(O, [[Iterated]]).
    auto object = TRY(typed_this_object(vm));

    // 3. Let iteratorRecord be O.[[Iterated]].
    auto const& iterator_record = object->iterated();

    // 4. Return ? Call(iteratorRecord.[[NextMethod]], iteratorRecord.[[Iterator]]).
    return TRY(call(vm, iterator_record.next_method, iterator_record.iterator));
}

// 27.1.3.2.1.1.2 %WrapForValidIteratorPrototype%.return ( ), https://tc39.es/ecma262/#sec-%wrapforvaliditeratorprototype%.return
JS_DEFINE_NATIVE_FUNCTION(WrapForValidIteratorPrototype::return_)
{
    // 1. Let O be this value.
    // 2. Perform ? RequireInternalSlot(O, [[Iterated]]).
    auto object = TRY(typed_this_object(vm));

    // 3. Let iterator be O.[[Iterated]].[[Iterator]].
    auto iterator = object->iterated().iterator;

    // 4. Assert: iterator is an Object.
    VERIFY(iterator);

    // 5. Let returnMethod be ? GetMethod(iterator, "return").
    auto return_method = TRY(Value { iterator }.get_method(vm, vm.names.return_));

    // 6. If returnMethod is undefined, then
    if (!return_method) {
        // a. Return CreateIterResultObject(undefined, true).
        return create_iterator_result_object(vm, js_undefined(), true);
    }

    // 7. Return ? Call(returnMethod, iterator).
    return TRY(call(vm, return_method, iterator));
}

}
