/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/PropertyDescriptor.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibWeb/HTML/Scripting/DevToolsObjectTable.h>

namespace Web::HTML {

WebView::DebuggerValue DevToolsObjectTable::serialize_value(JS::Value value)
{
    WebView::DebuggerValue result;
    if (value.is_special_empty_value()) {
        result.type = WebView::DebuggerValueType::Uninitialized;
        return result;
    }
    if (value.is_undefined())
        return result;
    if (value.is_null()) {
        result.type = WebView::DebuggerValueType::Null;
        return result;
    }
    if (value.is_boolean()) {
        result.type = WebView::DebuggerValueType::Boolean;
        result.boolean_value = value.as_bool();
        return result;
    }
    if (value.is_number()) {
        result.type = WebView::DebuggerValueType::Number;
        result.number_value = value.as_double();
        return result;
    }
    if (value.is_string()) {
        result.type = WebView::DebuggerValueType::String;
        result.text = value.as_string().utf16_string();
        return result;
    }
    if (value.is_bigint()) {
        result.type = WebView::DebuggerValueType::BigInt;
        result.text = value.as_bigint().to_utf16_string();
        return result;
    }
    if (value.is_symbol()) {
        // FIXME: Represent symbols with dedicated SymbolActors instead of converting them to strings.
        result.type = WebView::DebuggerValueType::String;
        result.text = value.to_utf16_string_without_side_effects();
        return result;
    }

    VERIFY(value.is_object());
    auto& object = value.as_object();
    Optional<u64> object_id;
    if (auto existing_id = m_object_ids.get(&object); existing_id.has_value())
        object_id = *existing_id;
    if (!object_id.has_value()) {
        object_id = m_next_object_id++;
        m_object_ids.set(&object, *object_id);
        m_objects.set(*object_id, GC::make_root(object));
    }
    result.type = WebView::DebuggerValueType::Object;
    result.object_id = *object_id;
    result.object_class = object.is_function() ? "Function"_string : MUST(String::from_utf8(object.class_name()));
    return result;
}

ErrorOr<WebView::DebuggerObjectProperties> DevToolsObjectTable::properties_for_object(u64 object_id)
{
    auto object_iterator = m_objects.find(object_id);
    if (object_iterator == m_objects.end())
        return Error::from_string_literal("Unable to locate object");
    // serialize_value() may add entries to m_objects, so retain a root
    // independent of the map's storage.
    auto object = object_iterator->value;

    WebView::DebuggerObjectProperties result;
    auto prototype = object->internal_get_prototype_of();
    if (prototype.is_error())
        return Error::from_string_literal("Unable to inspect object prototype");
    if (auto* prototype_object = prototype.release_value()) {
        result.prototype = serialize_value(prototype_object);
    } else {
        WebView::DebuggerValue null_prototype;
        null_prototype.type = WebView::DebuggerValueType::Null;
        result.prototype = move(null_prototype);
    }

    auto own_property_keys = object->internal_own_property_keys();
    if (own_property_keys.is_error())
        return Error::from_string_literal("Unable to inspect object properties");

    for (auto const& key_value : own_property_keys.release_value()) {
        // FIXME: Expose symbol properties through a symbol iterator actor.
        if (!key_value.is_string())
            continue;

        auto name = key_value.as_string().utf16_string();
        auto descriptor = object->internal_get_own_property(JS::PropertyKey { name });
        if (descriptor.is_error())
            return Error::from_string_literal("Unable to inspect object property");
        if (!descriptor.value().has_value())
            continue;

        auto const& property_descriptor = descriptor.value().value();
        WebView::DebuggerProperty property;
        property.name = move(name);
        property.writable = property_descriptor.writable.value_or(false);
        property.enumerable = property_descriptor.enumerable.value_or(false);
        property.configurable = property_descriptor.configurable.value_or(false);
        if (property_descriptor.value.has_value()) {
            property.value = serialize_value(*property_descriptor.value);
        } else {
            if (property_descriptor.get.has_value())
                property.getter = serialize_value(property_descriptor.get->ptr() ? JS::Value { property_descriptor.get->ptr() } : JS::js_undefined());
            if (property_descriptor.set.has_value())
                property.setter = serialize_value(property_descriptor.set->ptr() ? JS::Value { property_descriptor.set->ptr() } : JS::js_undefined());
        }
        result.properties.append(move(property));
    }

    return result;
}

void DevToolsObjectTable::clear()
{
    m_objects.clear();
    m_object_ids.clear();
}

}
