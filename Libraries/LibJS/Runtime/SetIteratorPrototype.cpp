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

JS_DEFINE_ALLOCATOR(SetIteratorPrototype);

SetIteratorPrototype::SetIteratorPrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().iterator_prototype())
{
}

void SetIteratorPrototype::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    define_native_function(realm, vm.names.next, next, 0, Attribute::Configurable | Attribute::Writable);

    // 24.2.5.2.2 %SetIteratorPrototype% [ @@toStringTag ], https://tc39.es/ecma262/#sec-%setiteratorprototype%-@@tostringtag
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "Set Iterator"_string), Attribute::Configurable);
}

// 24.2.5.2.1 %SetIteratorPrototype%.next ( ), https://tc39.es/ecma262/#sec-%setiteratorprototype%.next
JS_DEFINE_NATIVE_FUNCTION(SetIteratorPrototype::next)
{
    auto& realm = *vm.current_realm();

    auto set_iterator = TRY(typed_this_value(vm));
    if (set_iterator->done())
        return create_iterator_result_object(vm, js_undefined(), true);

    auto& set = set_iterator->set();
    if (set_iterator->m_iterator == set.end()) {
        set_iterator->m_done = true;
        return create_iterator_result_object(vm, js_undefined(), true);
    }

    auto iteration_kind = set_iterator->iteration_kind();
    VERIFY(iteration_kind != Object::PropertyKind::Key);

    auto value = (*set_iterator->m_iterator).key;
    ++set_iterator->m_iterator;
    if (iteration_kind == Object::PropertyKind::Value)
        return create_iterator_result_object(vm, value, false);

    return create_iterator_result_object(vm, Array::create_from(realm, { value, value }), false);
}

}
