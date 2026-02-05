/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/SetIteratorPrototype.h>

namespace JS {

GC_DEFINE_ALLOCATOR(SetIteratorPrototype);

SetIteratorPrototype::SetIteratorPrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().iterator_prototype())
{
}

void SetIteratorPrototype::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    define_native_function(realm, vm.names.next, next, 0, Attribute::Configurable | Attribute::Writable, Bytecode::Builtin::SetIteratorPrototypeNext);

    // 24.2.5.2.2 %SetIteratorPrototype% [ @@toStringTag ], https://tc39.es/ecma262/#sec-%setiteratorprototype%-@@tostringtag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "Set Iterator"_string), Attribute::Configurable);
}

// 24.2.5.2.1 %SetIteratorPrototype%.next ( ), https://tc39.es/ecma262/#sec-%setiteratorprototype%.next
JS_DEFINE_NATIVE_FUNCTION(SetIteratorPrototype::next)
{
    auto iterator = TRY(typed_this_value(vm));

    Value value;
    bool done = false;
    TRY(iterator->next(vm, done, value));

    return create_iterator_result_object(vm, value, done);
}

}
