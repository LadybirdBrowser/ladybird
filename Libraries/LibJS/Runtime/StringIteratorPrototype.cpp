/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/StringIteratorPrototype.h>

namespace JS {

GC_DEFINE_ALLOCATOR(StringIteratorPrototype);

StringIteratorPrototype::StringIteratorPrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().iterator_prototype())
{
}

void StringIteratorPrototype::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);
    define_native_function(realm, vm.names.next, next, 0, Attribute::Configurable | Attribute::Writable);

    // 22.1.5.1.2 %StringIteratorPrototype% [ @@toStringTag ], https://tc39.es/ecma262/#sec-%stringiteratorprototype%-@@tostringtag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "String Iterator"_string), Attribute::Configurable);
}

// 22.1.5.1.1 %StringIteratorPrototype%.next ( ), https://tc39.es/ecma262/#sec-%stringiteratorprototype%.next
JS_DEFINE_NATIVE_FUNCTION(StringIteratorPrototype::next)
{
    auto iterator = TRY(typed_this_value(vm));

    Value value;
    bool done = false;
    TRY(iterator->next(vm, done, value));

    return create_iterator_result_object(vm, value, done);
}

}
