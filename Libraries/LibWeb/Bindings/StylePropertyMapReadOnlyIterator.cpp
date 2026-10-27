/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/RootVector.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibWeb/Bindings/CSSStyleValue.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/StylePropertyMapReadOnly.h>
#include <LibWeb/Bindings/StylePropertyMapReadOnlyIterator.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/CSS/CSSStyleValue.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(StylePropertyMapReadOnlyIterator);

static constexpr JSHostClass style_property_map_iterator_host_class = JS::make_host_class(JS_HOST_CLASS_OBJECT, "StylePropertyMapReadOnlyIterator"sv, nullptr, nullptr, nullptr, 0);

StylePropertyMapReadOnlyIterator::StylePropertyMapReadOnlyIterator(JS::Object::PropertyKind kind, Vector<StylePropertyMapReadOnly::IterationEntry> entries, ReadonlySpan<GC::Ref<CSSStyleValue>> values)
    : m_kind(kind)
    , m_entries(move(entries))
{
    m_values.ensure_capacity(values.size());
    for (auto value : values)
        m_values.unchecked_append(value);
}

GC::Ref<JS::HostObject> StylePropertyMapReadOnlyIterator::create(JS::Realm& realm, StylePropertyMapReadOnly& map, JS::Object::PropertyKind kind)
{
    // INTEROP: Snapshot the entry list when iteration starts, matching Chrome and Safari.
    Vector<StylePropertyMapReadOnly::IterationEntry> entries;
    GC::RootVector<GC::Ref<CSSStyleValue>> values;
    if (kind == JS::Object::PropertyKind::Key) {
        for (auto const& name : map.property_names())
            entries.append({ name, 0, 0 });
    } else {
        map.collect_iteration_entries(entries, values);
    }
    auto iterator = realm.create<StylePropertyMapReadOnlyIterator>(kind, move(entries), values.span());
    auto& prototype = Bindings::ensure_web_prototype<Bindings::StylePropertyMapReadOnlyIteratorPrototype>(realm, "StylePropertyMapReadOnlyIterator"_utf16_fly_string);
    return JS::HostObject::create(realm, style_property_map_iterator_host_class, prototype, {}, iterator);
}

void StylePropertyMapReadOnlyIterator::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto value : m_values)
        visitor.visit(value);
}

// https://drafts.css-houdini.org/css-typed-om-1/#stylepropertymapreadonly
GC::Ref<JS::Object> StylePropertyMapReadOnlyIterator::next(JS::Realm& realm)
{
    if (m_index >= m_entries.size())
        return JS::create_iterator_result_object(realm, JS::js_undefined(), true);

    auto const& entry = m_entries[m_index++];
    auto name = JS::PrimitiveString::create(vm(), entry.name.to_utf16_string());
    if (m_kind == JS::Object::PropertyKind::Key)
        return JS::create_iterator_result_object(realm, name, false);

    GC::RootVector<JS::Value> wrapped_values;
    wrapped_values.ensure_capacity(entry.value_count);
    for (auto value : m_values.span().slice(entry.value_offset, entry.value_count))
        wrapped_values.unchecked_append(Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, value));
    auto array = JS::Array::create_from(realm, wrapped_values.span());
    if (m_kind == JS::Object::PropertyKind::Value)
        return JS::create_iterator_result_object(realm, array, false);

    return JS::create_iterator_result_object(realm, JS::Array::create_from(realm, { name, array }), false);
}

}
