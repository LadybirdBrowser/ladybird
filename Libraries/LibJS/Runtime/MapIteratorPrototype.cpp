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
#include <LibJS/Runtime/MapIteratorPrototype.h>

namespace JS {

JS_DEFINE_ALLOCATOR(MapIteratorPrototype);

MapIteratorPrototype::MapIteratorPrototype(Realm& realm)
    : PrototypeObject(realm.intrinsics().iterator_prototype())
{
}

void MapIteratorPrototype::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    define_native_function(realm, vm.names.next, next, 0, Attribute::Configurable | Attribute::Writable);
    define_direct_property(vm.well_known_symbol_to_string_tag(), PrimitiveString::create(vm, "Map Iterator"_string), Attribute::Configurable);
}

// 24.1.5.2.1 %MapIteratorPrototype%.next ( ), https://tc39.es/ecma262/#sec-%mapiteratorprototype%.next
JS_DEFINE_NATIVE_FUNCTION(MapIteratorPrototype::next)
{
    auto& realm = *vm.current_realm();

    auto map_iterator = TRY(typed_this_value(vm));
    if (map_iterator->done())
        return create_iterator_result_object(vm, js_undefined(), true);

    if (map_iterator->m_iterator.is_end()) {
        map_iterator->m_done = true;
        return create_iterator_result_object(vm, js_undefined(), true);
    }

    auto iteration_kind = map_iterator->iteration_kind();

    auto entry = *map_iterator->m_iterator;
    ++map_iterator->m_iterator;
    if (iteration_kind == Object::PropertyKind::Key)
        return create_iterator_result_object(vm, entry.key, false);
    if (iteration_kind == Object::PropertyKind::Value)
        return create_iterator_result_object(vm, entry.value, false);

    return create_iterator_result_object(vm, Array::create_from(realm, { entry.key, entry.value }), false);
}

}
