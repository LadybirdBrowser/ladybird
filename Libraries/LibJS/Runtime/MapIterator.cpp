/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Bytecode/PropertyAccess.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/MapIterator.h>
#include <LibJS/Runtime/MapIteratorPrototype.h>
#include <LibJS/Runtime/MapPrototype.h>
#include <LibJS/Runtime/NativeFunction.h>

namespace JS {

bool map_iteration_is_unobservable(Realm& realm, FunctionObject const& iterator_method)
{
    auto& map_prototype = static_cast<MapPrototype const&>(*realm.intrinsics().map_prototype());
    if (&iterator_method != &map_prototype.entries_function())
        return false;

    // NB: Inspect the intrinsic prototype's own property without invoking it. An accessor or a replacement method
    //     is observable, so it makes the caller take the generic path.
    static auto& next_method_cache = *new Bytecode::StaticPropertyLookupCache;
    auto next_method = Bytecode::get_own_property_without_side_effects(*realm.intrinsics().map_iterator_prototype(), realm.vm().names.next, next_method_cache);
    auto native_function = next_method.as_if<NativeFunction>();
    return native_function && native_function->realm() == &realm && native_function->is_map_prototype_next_builtin();
}

GC_DEFINE_ALLOCATOR(MapIterator);

GC::Ref<MapIterator> MapIterator::create(Realm& realm, Map& map, Object::PropertyKind iteration_kind)
{
    return realm.create<MapIterator>(map, iteration_kind, realm.intrinsics().map_iterator_prototype());
}

MapIterator::MapIterator(Map& map, Object::PropertyKind iteration_kind, Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
    , m_map(map)
    , m_iteration_kind(iteration_kind)
    , m_iterator(static_cast<Map const&>(map).begin())
{
}

void MapIterator::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_map);
    m_iterator.visit_edges(visitor);
}

BuiltinIterator* MapIterator::as_builtin_iterator_if_next_is_not_redefined(Value next_method)
{
    if (next_method.is_object()) {
        auto& next_function = next_method.as_object();
        if (next_function.is_native_function()) {
            auto const& native_function = static_cast<NativeFunction const&>(next_function);
            if (native_function.is_map_prototype_next_builtin())
                return this;
        }
    }
    return nullptr;
}

ThrowCompletionOr<void> MapIterator::next(VM& vm, bool& done, Value& value)
{
    if (m_done) {
        done = true;
        value = js_undefined();
        return {};
    }

    if (m_iterator.is_end()) {
        m_done = true;
        done = true;
        value = js_undefined();
        return {};
    }

    auto entry = *m_iterator;
    ++m_iterator;
    if (m_iteration_kind == Object::PropertyKind::Key) {
        value = entry.key;
        return {};
    }
    if (m_iteration_kind == Object::PropertyKind::Value) {
        value = entry.value;
        return {};
    }

    value = Array::create_from(*vm.current_realm(), { entry.key, entry.value });
    return {};
}

}
