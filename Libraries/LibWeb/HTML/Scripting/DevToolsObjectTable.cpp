/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/Map.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/PropertyDescriptor.h>
#include <LibJS/Runtime/RegExpObject.h>
#include <LibJS/Runtime/Set.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/DOM/CharacterData.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/HTMLCollection.h>
#include <LibWeb/DOM/NodeList.h>
#include <LibWeb/HTML/Scripting/DevToolsObjectTable.h>
#include <LibWeb/Namespace.h>

namespace Web::HTML {

// How many items, entries, or properties a preview lists, which is all a console shows of an object before it's expanded.
static constexpr size_t PREVIEW_ITEM_COUNT = 10;

// A preview lists what its object holds only for the object a value is, and not for those it holds, so a preview stays
// one level deep: An array of elements shows each element's name, but an array of arrays shows only their lengths.
static constexpr u8 MAX_PREVIEW_DEPTH = 1;

WebView::DebuggerValue DevToolsObjectTable::serialize_value(JS::Value value)
{
    return serialize_value(value, 0);
}

JsonValue DevToolsObjectTable::serialize_value_as_grip(JS::Value value)
{
    return grip_value(value, 0);
}

// Every table in the process takes its IDs from one count, so an ID never names a different object: The table of a
// document's console starts over when the next document replaces it, and DevTools may still hold IDs from the last.
static u64 s_next_object_id = 1;

u64 DevToolsObjectTable::id_for_object(JS::Object& object)
{
    if (auto existing_id = m_object_ids.get(&object); existing_id.has_value())
        return *existing_id;

    auto object_id = s_next_object_id++;
    m_object_ids.set(&object, object_id);
    m_objects.set(object_id, object);
    if (m_root_objects == RootObjects::Yes)
        m_roots.append(GC::make_root(object));
    return object_id;
}

// The class that a client shows for an object: the one JS knows it by, rather than the name of the engine's class for it.
static String class_name_of(JS::Object const& object)
{
    if (object.is_function())
        return "Function"_string;

    auto class_name = object.class_name();
    if (is<JS::RegExpObject>(object))
        return "RegExp"_string;
    if (object.is_proxy())
        return "Proxy"_string;
    for (auto wrapper : { "BigIntObject"sv, "BooleanObject"sv, "NumberObject"sv, "StringObject"sv, "SymbolObject"sv }) {
        if (class_name == wrapper)
            return MUST(String::from_utf8(class_name.substring_view(0, class_name.length() - "Object"sv.length())));
    }
    if (class_name.ends_with("Prototype"sv)) {
        // Of the built-in prototypes, only Array.prototype is an array, and Function.prototype a function.
        if (class_name == "ArrayPrototype"sv)
            return "Array"_string;
        return "Object"_string;
    }
    return MUST(String::from_utf8(class_name));
}

WebView::DebuggerValue DevToolsObjectTable::serialize_value(JS::Value value, u8 depth)
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
    result.type = WebView::DebuggerValueType::Object;
    result.object_id = id_for_object(object);
    result.object_class = class_name_of(object);
    if (depth <= MAX_PREVIEW_DEPTH)
        result.grip_fields = grip_fields_for_object(object, depth);
    return result;
}

// A value as a grip inside another grip's fields. An object is named by its ID, which the DevTools server swaps for an
// object actor.
JsonValue DevToolsObjectTable::grip_value(JS::Value value, u8 depth)
{
    auto serialized = serialize_value(value, depth);

    JsonObject grip;
    switch (serialized.type) {
    case WebView::DebuggerValueType::Undefined:
        grip.set("type"sv, "undefined"sv);
        return grip;
    case WebView::DebuggerValueType::Null:
    case WebView::DebuggerValueType::Uninitialized:
        grip.set("type"sv, "null"sv);
        return grip;
    case WebView::DebuggerValueType::Boolean:
        return serialized.boolean_value;
    case WebView::DebuggerValueType::Number:
        if (isnan(serialized.number_value)) {
            grip.set("type"sv, "NaN"sv);
            return grip;
        }
        if (isinf(serialized.number_value)) {
            grip.set("type"sv, serialized.number_value < 0 ? "-Infinity"sv : "Infinity"sv);
            return grip;
        }
        if (serialized.number_value == 0 && signbit(serialized.number_value)) {
            grip.set("type"sv, "-0"sv);
            return grip;
        }
        return serialized.number_value;
    case WebView::DebuggerValueType::String:
        return serialized.text.to_utf8();
    case WebView::DebuggerValueType::BigInt:
        grip.set("type"sv, "BigInt"sv);
        grip.set("text"sv, serialized.text.to_utf8());
        return grip;
    case WebView::DebuggerValueType::Object:
        if (serialized.grip_fields.is_object())
            grip = serialized.grip_fields.as_object();
        grip.set("type"sv, "object"sv);
        grip.set("objectId"sv, serialized.object_id);
        grip.set("class"sv, serialized.object_class);
        return grip;
    }
    VERIFY_NOT_REACHED();
}

// The value of a property as reading it produces, but without running any script: A getter written in script, or a
// proxy along the way, leaves the property unread.
static Optional<JS::Value> read_property_without_running_script(JS::Object& object, JS::PropertyKey const& key)
{
    for (GC::Ptr<JS::Object> holder = &object; holder;) {
        if (holder->is_proxy())
            return {};

        auto descriptor = holder->internal_get_own_property(key);
        if (descriptor.is_error())
            return {};

        if (descriptor.value().has_value()) {
            auto const& property = *descriptor.value();
            if (property.is_data_descriptor())
                return property.value.value_or(JS::js_undefined());

            auto getter = property.get.value_or(nullptr);
            if (!getter || !is<JS::NativeFunction>(*getter))
                return {};

            auto value = JS::call(object.vm(), *getter, &object);
            if (value.is_error())
                return {};
            return value.value();
        }

        auto prototype = holder->internal_get_prototype_of();
        if (prototype.is_error())
            return {};
        holder = prototype.value();
    }

    return JS::js_undefined();
}

static Optional<JS::Value> read_property_without_running_script(JS::Object& object, StringView name)
{
    return read_property_without_running_script(object, JS::PropertyKey { Utf16FlyString::from_utf8(name) });
}

// The length of an array, or of a list of nodes, whose items a preview lists by index.
static Optional<u32> array_like_length(JS::Object& object)
{
    if (!is<JS::Array>(object) && !Bindings::impl_from<DOM::NodeList>(&object) && !Bindings::impl_from<DOM::HTMLCollection>(&object))
        return {};

    auto length = read_property_without_running_script(object, "length"sv);
    if (!length.has_value() || !length->is_number())
        return {};
    return static_cast<u32>(length->as_double());
}

JsonObject DevToolsObjectTable::grip_fields_for_object(JS::Object& object, u8 depth)
{
    JsonObject fields;
    JsonObject preview;
    auto lists_contents = depth < MAX_PREVIEW_DEPTH;

    if (auto const* node = Bindings::impl_from<DOM::Node>(&object)) {
        preview.set("kind"sv, "DOMNode"sv);
        preview.set("nodeType"sv, node->node_type());
        preview.set("isConnected"sv, node->is_connected());

        auto node_name = node->node_name().view().to_utf8_but_should_be_ported_to_utf16();
        if (auto const* element = as_if<DOM::Element>(*node)) {
            // The nodeName of an HTML element is its tag name in uppercase, but a console writes the tag the way the
            // markup does.
            if (element->namespace_uri() == Namespace::HTML)
                node_name = node_name.to_ascii_lowercase();

            JsonObject attributes;
            element->for_each_attribute([&](Utf16FlyString name, Utf16String value) {
                attributes.set(name.view().to_utf8_but_should_be_ported_to_utf16(), value.to_utf8());
            });
            preview.set("attributes"sv, move(attributes));
            preview.set("attributesLength"sv, element->attribute_list_size());
        } else if (auto const* character_data = as_if<DOM::CharacterData>(*node)) {
            preview.set("textContent"sv, character_data->data().to_utf8());
        } else if (auto const* document = as_if<DOM::Document>(*node)) {
            preview.set("location"sv, document->url().serialize());
        }
        preview.set("nodeName"sv, move(node_name));

        // The DevTools server completes this with the browsing context, so a client can select the node in its
        // inspector.
        JsonObject content_dom_reference;
        content_dom_reference.set("id"sv, node->unique_id().value());
        fields.set("contentDomReference"sv, move(content_dom_reference));
        fields.set("preview"sv, move(preview));
        return fields;
    }

    if (auto length = array_like_length(object); length.has_value()) {
        preview.set("kind"sv, "ArrayLike"sv);
        preview.set("length"sv, *length);
        if (lists_contents) {
            JsonArray items;
            for (u32 index = 0; index < min<u32>(*length, PREVIEW_ITEM_COUNT); ++index) {
                auto item = read_property_without_running_script(object, JS::PropertyKey { index });
                items.must_append(grip_value(item.value_or(JS::js_undefined()), depth + 1));
            }
            preview.set("items"sv, move(items));
        }
        fields.set("preview"sv, move(preview));
        return fields;
    }

    if (auto* set = as_if<JS::Set>(object)) {
        preview.set("kind"sv, "ArrayLike"sv);
        preview.set("length"sv, set->set_size());
        if (lists_contents) {
            JsonArray items;
            (void)set->for_each_value([&](JS::Value value) -> JS::ThrowCompletionOr<void> {
                if (items.size() < PREVIEW_ITEM_COUNT)
                    items.must_append(grip_value(value, depth + 1));
                return {};
            });
            preview.set("items"sv, move(items));
        }
        fields.set("preview"sv, move(preview));
        return fields;
    }

    if (auto* map = as_if<JS::Map>(object)) {
        preview.set("kind"sv, "MapLike"sv);
        preview.set("size"sv, map->map_size());
        if (lists_contents) {
            JsonArray entries;
            (void)map->for_each_entry([&](JS::Value key, JS::Value value) -> JS::ThrowCompletionOr<void> {
                if (entries.size() < PREVIEW_ITEM_COUNT) {
                    JsonArray entry;
                    entry.must_append(grip_value(key, depth + 1));
                    entry.must_append(grip_value(value, depth + 1));
                    entries.must_append(move(entry));
                }
                return {};
            });
            preview.set("entries"sv, move(entries));
        }
        fields.set("preview"sv, move(preview));
        return fields;
    }

    if (object.is_function()) {
        auto name = read_property_without_running_script(object, "name"sv);
        if (name.has_value() && name->is_string()) {
            auto function_name = name->as_string().utf16_string().to_utf8();
            fields.set("name"sv, function_name);
            fields.set("displayName"sv, move(function_name));
        }
        return fields;
    }

    if (auto* date = as_if<JS::Date>(object)) {
        preview.set("timestamp"sv, date->date_value());
        fields.set("preview"sv, move(preview));
        return fields;
    }

    if (auto* regexp = as_if<JS::RegExpObject>(object)) {
        fields.set("displayString"sv, MUST(String::formatted("/{}/{}", regexp->pattern(), regexp->flags())));
        return fields;
    }

    if (is<JS::Error>(object)) {
        preview.set("kind"sv, "Error"sv);
        for (auto name : { "name"sv, "message"sv, "stack"sv, "fileName"sv, "lineNumber"sv, "columnNumber"sv }) {
            auto value = read_property_without_running_script(object, name);
            preview.set(name, grip_value(value.value_or(JS::js_undefined()), depth + 1));
        }
        fields.set("preview"sv, move(preview));
        return fields;
    }

    if (object.is_proxy())
        return fields;

    auto keys = object.internal_own_property_keys();
    if (keys.is_error())
        return fields;

    JsonObject own_properties;
    size_t own_properties_length = 0;
    for (auto const& key : keys.value()) {
        if (!key.is_string())
            continue;
        ++own_properties_length;
        if (!lists_contents || own_properties.size() >= PREVIEW_ITEM_COUNT)
            continue;

        auto name = key.as_string().utf16_string();
        auto descriptor = object.internal_get_own_property(JS::PropertyKey { name });
        if (descriptor.is_error() || !descriptor.value().has_value())
            continue;

        auto const& property = *descriptor.value();
        JsonObject serialized;
        if (property.value.has_value()) {
            serialized.set("value"sv, grip_value(*property.value, depth + 1));
            serialized.set("writable"sv, property.writable.value_or(false));
        } else {
            serialized.set("get"sv, grip_value(property.get->ptr() ? JS::Value { property.get->ptr() } : JS::js_undefined(), depth + 1));
            serialized.set("set"sv, grip_value(property.set.has_value() && property.set->ptr() ? JS::Value { property.set->ptr() } : JS::js_undefined(), depth + 1));
        }
        serialized.set("configurable"sv, property.configurable.value_or(false));
        serialized.set("enumerable"sv, property.enumerable.value_or(false));
        own_properties.set(name.to_utf8(), move(serialized));
    }

    preview.set("kind"sv, "Object"sv);
    preview.set("ownPropertiesLength"sv, own_properties_length);
    if (lists_contents) {
        preview.set("ownProperties"sv, move(own_properties));
        preview.set("ownSymbols"sv, JsonArray {});
        preview.set("ownSymbolsLength"sv, 0);
        preview.set("safeGetterValues"sv, JsonObject {});
    }
    fields.set("ownPropertyLength"sv, own_properties_length);
    fields.set("preview"sv, move(preview));
    return fields;
}

ErrorOr<WebView::DebuggerObjectProperties> DevToolsObjectTable::properties_for_object(u64 object_id)
{
    auto object_iterator = m_objects.find(object_id);
    if (object_iterator == m_objects.end())
        return Error::from_string_literal("Unable to locate object");
    // serialize_value() may add entries to m_objects, so keep a reference independent of the map's storage.
    auto object = object_iterator->value;

    WebView::DebuggerObjectProperties result;

    // Reading the prototype or the properties of a proxy runs its traps, which can be page script.
    if (object->is_proxy())
        return result;

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
    m_roots.clear();
}

void DevToolsObjectTable::visit_edges(GC::Cell::Visitor& visitor)
{
    visitor.visit(m_objects);
    visitor.visit(m_object_ids);
}

}
