/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/ArrayIteratorPrototype.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/TypedArray.h>

namespace JS {

GC_DEFINE_ALLOCATOR(ArrayIteratorPrototype);

ArrayIteratorPrototype::ArrayIteratorPrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().iterator_prototype())
{
}

void ArrayIteratorPrototype::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    define_native_function(realm, vm.names.next, next, 0, Attribute::Configurable | Attribute::Writable, Bytecode::Builtin::ArrayIteratorPrototypeNext);

    // 23.1.5.2.2 %ArrayIteratorPrototype% [ @@toStringTag ], https://tc39.es/ecma262/#sec-%arrayiteratorprototype%-@@tostringtag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "Array Iterator"_string), Attribute::Configurable);
}

// 23.1.5.2.1 %ArrayIteratorPrototype%.next ( ), https://tc39.es/ecma262/#sec-%arrayiteratorprototype%.next
JS_DEFINE_NATIVE_FUNCTION(ArrayIteratorPrototype::next)
{
    auto iterator = TRY(typed_this_value(vm));

    Value value;
    bool done = false;
    TRY(iterator->next(vm, done, value));

    return create_iterator_result_object(vm, value, done);
}

}
