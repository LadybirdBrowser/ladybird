/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericLexer.h>
#include <AK/HashTable.h>
#include <AK/JsonArray.h>
#include <AK/Random.h>
#include <AK/TemporaryChange.h>
#include <LibCrypto/BigInt/SignedBigInteger.h>
#include <LibGC/ConservativeVector.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/ErrorData.h>
#include <LibJS/Runtime/Map.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/Promise.h>
#include <LibJS/Runtime/RegExpObject.h>
#include <LibJS/Runtime/Set.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Attr.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/HTMLCollection.h>
#include <LibWeb/DOM/NamedNodeMap.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/NodeList.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/WebDriver/BiDi/BiDi.h>
#include <LibWeb/WebDriver/BiDi/BindingsGlue.h>
#include <LibWeb/WebDriver/BiDi/RemoteValue.h>
#include <LibWeb/WebDriver/BiDi/Script.h>
#include <LibWeb/WebDriver/ElementReference.h>

namespace Web::WebDriver::BiDi {

static Error invalid_argument(StringView message)
{
    return Error::from_code(ErrorCode::InvalidArgument, message);
}

static String to_utf8(Utf16View view)
{
    return view.to_utf8_but_should_be_ported_to_utf16();
}

// https://w3c.github.io/webdriver-bidi/#type-script-ResultOwnership
ErrorOr<ResultOwnership, Error> parse_result_ownership(JsonValue const& value)
{
    // script.ResultOwnership = "root" / "none"
    if (value.is_string()) {
        if (value.as_string() == "root"sv)
            return ResultOwnership::Root;
        if (value.as_string() == "none"sv)
            return ResultOwnership::None;
    }
    return invalid_argument("Result ownership must be \"root\" or \"none\""sv);
}

// https://w3c.github.io/webdriver-bidi/#handle-object-map
// Each ECMAScript Realm has a corresponding handle object map. This is a strong map from handle ids to their
// corresponding objects.
// NB: Keyed by realm id. The map of a window realm is cleared when its document is unloaded.
// NB: A handle names an object or a symbol, which the map keeps alive.
using HandleObjectMap = HashMap<String, GC::RootVector<JS::Value>>;
static HashMap<String, HandleObjectMap>& handle_object_maps()
{
    static auto& maps = *new HashMap<String, HandleObjectMap>;
    return maps;
}

// NB: The id of a realm that is not the realm of a window, which no document names.
static HashMap<HTML::EnvironmentSettingsObject const*, String>& other_realm_ids()
{
    static auto& ids = *new HashMap<HTML::EnvironmentSettingsObject const*, String>;
    return ids;
}

// https://w3c.github.io/webdriver-bidi/#realm-id
// Each realm has an associated realm id, which is a string uniquely identifying that realm. This is implicitly set when
// the realm is created.
// NB: The realm of a window is named by the document that is its window's associated Document, which the browser
//     process routes commands to. A window that a new document reuses is a new realm for WebDriver BiDi.
String realm_id(HTML::EnvironmentSettingsObject& settings)
{
    if (auto* window = HTML::window_from_global_object(settings.global_object())) {
        auto id = window->associated_document().id();
        return MUST(String::formatted("{}.{}", id.namespace_id, id.local_id));
    }

    return other_realm_ids().ensure(&settings, [] { return generate_random_uuid(); });
}

HTML::EnvironmentSettingsObject* settings_with_realm_id(HTML::Navigable& navigable, StringView id)
{
    auto* local_navigable = as_if<HTML::LocalNavigable>(navigable);
    if (!local_navigable)
        return nullptr;

    auto document = local_navigable->active_document();
    if (!document)
        return nullptr;

    auto& settings = document->relevant_settings_object();
    if (realm_id(settings) != id)
        return nullptr;
    return &settings;
}

void clear_handle_object_map(HTML::EnvironmentSettingsObject& settings)
{
    handle_object_maps().remove(realm_id(settings));
}

void clear_all_handle_object_maps()
{
    handle_object_maps().clear();
    other_realm_ids().clear();
}

bool remove_handle(HTML::EnvironmentSettingsObject& settings, StringView handle_id)
{
    auto map = handle_object_maps().find(realm_id(settings));
    if (map == handle_object_maps().end())
        return false;
    return map->value.remove(handle_id);
}

// https://w3c.github.io/webdriver-bidi/#serialize-primitive-protocol-value
static Optional<JsonObject> serialize_primitive_protocol_value(JS::Value value)
{
    // 1. Let remote value be undefined.
    JsonObject remote_value;

    // 2. In the following list of conditions and associated steps, run the first set of steps for which the associated
    //    condition is true, if any:
    // -> Type(value) is undefined
    if (value.is_undefined()) {
        // Let remote value be a map matching the script.UndefinedValue production in the local end definition.
        remote_value.set("type"sv, "undefined"sv);
    }
    // -> Type(value) is Null
    else if (value.is_null()) {
        // Let remote value be a map matching the script.NullValue production in the local end definition.
        remote_value.set("type"sv, "null"sv);
    }
    // -> Type(value) is String
    else if (value.is_string()) {
        // Let remote value be a map matching the script.StringValue production in the local end definition, with the
        // value property set to value.
        remote_value.set("type"sv, "string"sv);
        remote_value.set("value"sv, to_utf8(value.as_string().utf16_string_view()));
    }
    // -> Type(value) is Number
    else if (value.is_number()) {
        // 1. Switch on the value of value:
        JsonValue serialized;
        auto number = value.as_double();
        // NaN
        if (isnan(number))
            serialized = "NaN"sv;
        // -0
        else if (value.is_negative_zero())
            serialized = "-0"sv;
        // Infinity
        else if (value.is_positive_infinity())
            serialized = "Infinity"sv;
        // -Infinity
        else if (value.is_negative_infinity())
            serialized = "-Infinity"sv;
        // Otherwise:
        else if (value.is_int32())
            serialized = value.as_i32();
        else
            serialized = number;

        // 2. Let remote value be a map matching the script.NumberValue production in the local end definition, with the
        //    value property set to serialized.
        remote_value.set("type"sv, "number"sv);
        remote_value.set("value"sv, move(serialized));
    }
    // -> Type(value) is Boolean
    else if (value.is_boolean()) {
        // Let remote value be a map matching the script.BooleanValue production in the local end definition, with the
        // value property set to value.
        remote_value.set("type"sv, "boolean"sv);
        remote_value.set("value"sv, value.as_bool());
    }
    // -> Type(value) is BigInt
    else if (value.is_bigint()) {
        // Let remote value be a map matching the script.BigIntValue production in the local end definition, with the
        // value property set to the result of running the ToString operation on value.
        remote_value.set("type"sv, "bigint"sv);
        remote_value.set("value"sv, MUST(value.as_bigint().big_integer().to_base(10)));
    } else {
        return {};
    }

    // 3. Return remote value
    return remote_value;
}

// The ISO 8601 string of a time value, as Date.prototype.toISOString gives it.
static String date_to_iso_string(double time_value)
{
    auto year = JS::year_from_time(time_value);
    StringBuilder builder;
    if (year >= 0 && year <= 9999)
        builder.appendff("{:04}", year);
    else
        builder.appendff("{}{:06}", year < 0 ? '-' : '+', AK::abs(year));
    builder.appendff("-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z",
        JS::month_from_time(time_value) + 1,
        JS::date_from_time(time_value),
        JS::hour_from_time(time_value),
        JS::min_from_time(time_value),
        JS::sec_from_time(time_value),
        JS::ms_from_time(time_value));
    return builder.to_string_without_validation();
}

namespace {

// NB: A serialization gives every object it may name twice a marker as its internalId, and then the internalId of each
//     object it named twice, or removes the marker of one it named once. The internal id of a remote value is known
//     only once the value is serialized again.
struct Serializer {
    GC::Ref<HTML::EnvironmentSettingsObject> settings;

    // NB: The depth of the remote value being serialized, which an object graph of web content decides.
    size_t depth { 0 };

    // https://w3c.github.io/webdriver-bidi/#serialization-internal-map
    // NB: The objects of the map are kept alive, as serializing may run script that drops them, so that the address of
    //     an object is not reused for another one while the map names it.
    HashMap<GC::RawPtr<JS::Object const>, u64> serialization_internal_map;
    GC::RootVector<JS::Value> serialized_objects;
    HashTable<u64> duplicated_markers;
    u64 next_marker { 0 };

    JsonValue serialize_as_a_remote_value(JS::Value, SerializationOptions const&, ResultOwnership);
    JsonArray serialize_as_a_list(GC::RootVector<JS::Value> const&, SerializationOptions const&, ResultOwnership);
    JsonArray serialize_as_a_mapping(GC::ConservativeVector<Array<JS::Value, 2>> const&, SerializationOptions const&, ResultOwnership);
    JsonObject serialize_an_array_like(StringView type, Optional<String> handle_id, bool known_object, JS::Object const& value, GC::RootVector<JS::Value> const& items, SerializationOptions const&, ResultOwnership);
    Optional<String> handle_for_an_object(ResultOwnership, JS::Value);
    void set_internal_ids_if_needed(JsonObject& remote_value, JS::Object const&);
    void assign_internal_ids(JsonValue&);

    HashMap<u64, String> internal_ids;
};

}

static String marker_string(u64 marker)
{
    return MUST(String::formatted("{}", marker));
}

// https://w3c.github.io/webdriver-bidi/#handle-for-an-object
Optional<String> Serializer::handle_for_an_object(ResultOwnership ownership_type, JS::Value object)
{
    // 1. If ownership type is equal "none", return null.
    if (ownership_type == ResultOwnership::None)
        return {};

    // 2. Let handle id be a new, unique, string handle for object.
    auto handle_id = generate_random_uuid();

    // 3. Let handle map be realm's handle object map
    // 4. Set handle map[handle id] to object.
    GC::RootVector<JS::Value> root;
    root.append(object);
    handle_object_maps().ensure(realm_id(*settings)).set(handle_id, move(root));

    // 5. Return handle id as a result.
    return handle_id;
}

// https://w3c.github.io/webdriver-bidi/#set-internal-ids-if-needed
void Serializer::set_internal_ids_if_needed(JsonObject& remote_value, JS::Object const& object)
{
    // 1. If the serialization internal map does not contain object, set serialization internal map[object] to remote
    //    value.
    auto it = serialization_internal_map.find(&object);
    if (it == serialization_internal_map.end()) {
        auto marker = next_marker++;
        serialization_internal_map.set(&object, marker);
        serialized_objects.append(JS::Value { const_cast<JS::Object*>(&object) });
        remote_value.set("internalId"sv, marker_string(marker));
        return;
    }

    // 2. Otherwise, run the following steps:
    //     1. Let previously serialized remote value be serialization internal map[object].
    //     2. If previously serialized remote value does not have a field internalId, run the following steps:
    //         1. Let internal id be the string representation of a UUID based on truly random, or pseudo-random numbers.
    //         2. Set the internalId field of previously serialized remote value to internal id.
    //     3. Set the internalId field of remote value to a field internalId in previously serialized remote value.
    duplicated_markers.set(it->value);
    remote_value.set("internalId"sv, marker_string(it->value));
}

static void for_each_child_remote_value(JsonObject& remote_value, Function<void(JsonValue&)> const& callback)
{
    auto type = remote_value.get_string("type"sv);
    if (!type.has_value())
        return;

    if (type->is_one_of("array"sv, "set"sv, "nodelist"sv, "htmlcollection"sv)) {
        if (auto list = remote_value.get_array("value"sv); list.has_value())
            list->for_each([&](JsonValue& item) { callback(item); });
    } else if (type->is_one_of("map"sv, "object"sv)) {
        if (auto mapping = remote_value.get_array("value"sv); mapping.has_value()) {
            mapping->for_each([&](JsonValue& entry) {
                if (!entry.is_array())
                    return;
                entry.as_array().for_each([&](JsonValue& key_or_value) {
                    if (key_or_value.is_object())
                        callback(key_or_value);
                });
            });
        }
    } else if (*type == "node"sv) {
        if (auto properties = remote_value.get_object("value"sv); properties.has_value()) {
            if (auto children = properties->get_array("children"sv); children.has_value())
                children->for_each([&](JsonValue& child) { callback(child); });
            if (auto shadow_root = properties->get("shadowRoot"sv); shadow_root.has_value() && shadow_root->is_object())
                callback(*shadow_root);
        }
    }
}

void Serializer::assign_internal_ids(JsonValue& value)
{
    if (!value.is_object())
        return;
    auto& remote_value = value.as_object();

    if (auto marker_value = remote_value.get_string("internalId"sv); marker_value.has_value()) {
        auto marker = marker_value->to_number<u64>().value();
        if (duplicated_markers.contains(marker))
            remote_value.set("internalId"sv, internal_ids.ensure(marker, [] { return generate_random_uuid(); }));
        else
            remote_value.remove("internalId"sv);
    }

    for_each_child_remote_value(remote_value, [&](JsonValue& child) {
        assign_internal_ids(child);
    });
}

static SerializationOptions child_serialization_options_for_object_depth(SerializationOptions const& options)
{
    // 1. Let child serialization options be a clone of serialization options.
    auto child_serialization_options = options;

    // 2. If child serialization options["maxObjectDepth"] is not null, set child serialization options["maxObjectDepth"]
    //    to child serialization options["maxObjectDepth"] - 1.
    if (child_serialization_options.max_object_depth.has_value())
        child_serialization_options.max_object_depth = *child_serialization_options.max_object_depth - 1;

    return child_serialization_options;
}

// https://w3c.github.io/webdriver-bidi/#serialize-as-a-list
JsonArray Serializer::serialize_as_a_list(GC::RootVector<JS::Value> const& values, SerializationOptions const& serialization_options, ResultOwnership ownership_type)
{
    // 1. If serialization options["maxObjectDepth"] is not null, assert: serialization options["maxObjectDepth"] is
    //    greater than 0.
    VERIFY(!serialization_options.max_object_depth.has_value() || *serialization_options.max_object_depth > 0);

    // 2. Let serialized be a new list.
    JsonArray serialized;

    // 3. For each child value in IteratorToList(GetIterator(iterable, sync)):
    for (auto child_value : values) {
        // 1. Let child serialization options be a clone of serialization options.
        // 2. If child serialization options["maxObjectDepth"] is not null, set child serialization
        //    options["maxObjectDepth"] to child serialization options["maxObjectDepth"] - 1.
        auto child_serialization_options = child_serialization_options_for_object_depth(serialization_options);

        // 3. Let serialized child be the result of serialize as a remote value with child value, child serialization
        //    options, ownership type, serialization internal map, realm, and session.
        // 4. Append serialized child to serialized.
        serialized.must_append(serialize_as_a_remote_value(child_value, child_serialization_options, ownership_type));
    }

    // 4. Return serialized
    return serialized;
}

// https://w3c.github.io/webdriver-bidi/#serialize-as-a-mapping
JsonArray Serializer::serialize_as_a_mapping(GC::ConservativeVector<Array<JS::Value, 2>> const& items, SerializationOptions const& serialization_options, ResultOwnership ownership_type)
{
    // 1. If serialization options["maxObjectDepth"] is not null, assert: serialization options["maxObjectDepth"] is
    //    greater than 0.
    VERIFY(!serialization_options.max_object_depth.has_value() || *serialization_options.max_object_depth > 0);

    // 2. Let serialized be a new list.
    JsonArray serialized;

    // 3. For item in IteratorToList(GetIterator(iterable, sync)):
    for (auto const& item : items) {
        // 1. Assert: IsArray(item)
        // 2. Let property be CreateListFromArrayLike(item)
        // 3. Assert: property is a list of size 2
        // 4. Let key be property[0] and let value be property[1]
        auto key = item[0];
        auto value = item[1];

        // 5. Let child serialization options be a clone of serialization options.
        // 6. If child serialization options["maxObjectDepth"] is not null, set child serialization
        //    options["maxObjectDepth"] to child serialization options["maxObjectDepth"] - 1.
        auto child_serialization_options = child_serialization_options_for_object_depth(serialization_options);

        // 7. If Type(key) is String, let serialized key be child key, otherwise let serialized key be the result of
        //    serialize as a remote value with child key, child serialization options, ownership type, serialization
        //    internal map, realm, and session.
        JsonValue serialized_key;
        if (key.is_string())
            serialized_key = to_utf8(key.as_string().utf16_string_view());
        else
            serialized_key = serialize_as_a_remote_value(key, child_serialization_options, ownership_type);

        // 8. Let serialized value be the result of serialize as a remote value with value, child serialization options,
        //    ownership type, serialization internal map, realm, and session.
        auto serialized_value = serialize_as_a_remote_value(value, child_serialization_options, ownership_type);

        // 9. Let serialized child be («serialized key, serialized value»).
        JsonArray serialized_child;
        serialized_child.must_append(move(serialized_key));
        serialized_child.must_append(move(serialized_value));

        // 10. Append serialized child to serialized.
        serialized.must_append(move(serialized_child));
    }

    // 4. Return serialized
    return serialized;
}

// https://w3c.github.io/webdriver-bidi/#serialize-an-array-like
JsonObject Serializer::serialize_an_array_like(StringView production, Optional<String> handle_id, bool known_object, JS::Object const& value, GC::RootVector<JS::Value> const& items, SerializationOptions const& serialization_options, ResultOwnership ownership_type)
{
    // 1. Let remote value be a map matching production, with the handle property set to handle id if it's not null, or
    //    omitted otherwise.
    JsonObject remote_value;
    remote_value.set("type"sv, production);
    if (handle_id.has_value())
        remote_value.set("handle"sv, handle_id.release_value());

    // 2. Set internal ids if needed with serialization internal map, remote value and value.
    set_internal_ids_if_needed(remote_value, value);

    // 3. If known object is false, and serialization options["maxObjectDepth"] is not 0:
    if (!known_object && serialization_options.max_object_depth != 0u) {
        // 1. Let serialized be the result of serialize as a list with CreateArrayIterator(value, value), serialization
        //    options, ownership type, serialization internal map, realm, and session.
        // 2. If serialized is not null, set field value of remote value to serialized.
        remote_value.set("value"sv, serialize_as_a_list(items, serialization_options, ownership_type));
    }

    // 4. Return remote value
    return remote_value;
}

// NB: The elements of an Array-like that CreateArrayIterator visits.
static GC::RootVector<JS::Value> array_like_items(JS::VM& vm, JS::Object const& object)
{
    // AD-HOC: The length of an Array-like does not need to match the elements it has, so the remote value of one lists
    //         this many elements at most.
    static constexpr size_t MAXIMUM_ITEMS = 1 << 16;

    GC::RootVector<JS::Value> items;
    auto length_value = object.get(vm.names.length);
    if (length_value.is_error())
        return items;
    auto length = length_value.value().to_length(vm);
    if (length.is_error())
        return items;
    for (size_t index = 0; index < min<size_t>(length.value(), MAXIMUM_ITEMS); ++index) {
        auto item = object.get(JS::PropertyKey { index });
        items.append(item.is_error() ? JS::js_undefined() : item.value());
    }
    return items;
}

// https://w3c.github.io/webdriver-bidi/#get-shared-id-for-a-node
static Optional<String> get_shared_id_for_a_node(DOM::Node const& node)
{
    // 1. Let node be unwrapped node.
    // 2. If node does not implement Node, return null.
    // 3. Let navigable be node's node navigable.
    auto navigable = node.navigable();

    // 4. If navigable is null, return null.
    if (!navigable || !navigable->active_browsing_context())
        return {};

    // 5. Return get or create a node reference with session, navigable and node.
    return get_or_create_a_node_reference(*navigable->active_browsing_context(), node);
}

// https://w3c.github.io/webdriver-bidi/#serialize-as-a-remote-value
JsonValue Serializer::serialize_as_a_remote_value(JS::Value value, SerializationOptions const& serialization_options, ResultOwnership ownership_type)
{
    auto& vm = settings->global_object().vm();

    // AD-HOC: A remote value deeper than this is serialized as if the serialization options allowed no deeper objects
    //         and nodes, so that serializing it does not exhaust the stack, and its JSON can be parsed.
    static constexpr size_t MAXIMUM_DEPTH = 100;
    TemporaryChange depth_change { depth, depth + 1 };
    if (depth > MAXIMUM_DEPTH && (serialization_options.max_object_depth != 0u || serialization_options.max_dom_depth != 0u)) {
        auto limited_serialization_options = serialization_options;
        limited_serialization_options.max_object_depth = 0;
        limited_serialization_options.max_dom_depth = 0;
        return serialize_as_a_remote_value(value, limited_serialization_options, ownership_type);
    }

    // 1. Let remote value be a result of serialize primitive protocol value given a value.
    // 2. If remote value is not undefined, return remote value.
    if (auto remote_value = serialize_primitive_protocol_value(value); remote_value.has_value())
        return remote_value.release_value();

    JsonObject remote_value;

    // 3. Let handle id be the handle for an object with realm, ownership type and value.
    auto handle_id = handle_for_an_object(ownership_type, value);

    // -> Type(value) is Symbol
    if (value.is_symbol()) {
        // Let remote value be a map matching the script.SymbolRemoteValue production in the local end definition, with
        // the handle property set to handle id if it's not null, or omitted otherwise.
        remote_value.set("type"sv, "symbol"sv);
        if (handle_id.has_value())
            remote_value.set("handle"sv, handle_id.release_value());
        return remote_value;
    }

    VERIFY(value.is_object());
    auto& object = value.as_object();

    // 4. Set ownership type to "none".
    ownership_type = ResultOwnership::None;

    // 5. Let known object be true, if value is in the serialization internal map, otherwise false.
    auto known_object = serialization_internal_map.contains(&object);

    auto set_handle = [&](JsonObject& remote_value) {
        if (handle_id.has_value())
            remote_value.set("handle"sv, *handle_id);
    };

    auto simple = [&](StringView type) {
        JsonObject remote_value;
        remote_value.set("type"sv, type);
        set_handle(remote_value);
        return remote_value;
    };

    // 6. In the following list of conditions and associated steps, run the first set of steps for which the associated
    //    condition is true:
    // -> IsArray(value)
    if (auto is_array = value.is_array(vm); !is_array.is_error() && is_array.value()) {
        // Let remote value be serialize an Array-like with session, script.ArrayRemoteValue, handle id, known object,
        // value, serialization options, ownership type, serialization internal map, realm, and session.
        GC::RootVector<JS::Value> items;
        if (!known_object && serialization_options.max_object_depth != 0u)
            items = array_like_items(vm, object);
        return serialize_an_array_like("array"sv, move(handle_id), known_object, object, items, serialization_options, ownership_type);
    }

    // -> IsRegExp(value)
    // NB: This does not get @@match, which would run script.
    if (auto const* regexp = as_if<JS::RegExpObject>(object)) {
        // 1. Let pattern be ToString(Get(value, "source")).
        // 2. Let flags be ToString(Get(value, "flags")).
        // NB: These are the source and flags the RegExp object has, without running script.
        auto pattern = to_utf8(regexp->pattern());
        auto flags = to_utf8(regexp->flags());

        // 3. Let serialized be a map matching the script.RegExpValue production in the local end definition, with the
        //    pattern property set to the pattern and the the flags property set to the flags.
        JsonObject serialized;
        serialized.set("pattern"sv, move(pattern));
        serialized.set("flags"sv, move(flags));

        // 4. Let remote value be a map matching the script.RegExpRemoteValue production in the local end definition,
        //    with the handle property set to handle id if it's not null, or omitted otherwise, and the value property set
        //    to serialized.
        auto remote_value = simple("regexp"sv);
        remote_value.set("value"sv, move(serialized));
        return remote_value;
    }

    // -> value has a [[DateValue]] internal slot.
    if (auto const* date = as_if<JS::Date>(object)) {
        // 1. Set serialized to Call(Date.prototype.toISOString, value).
        // 2. Assert: serialized is not a throw completion.
        // NB: toISOString throws a RangeError for an invalid date.
        auto remote_value = simple("date"sv);
        if (isfinite(date->date_value()))
            remote_value.set("value"sv, date_to_iso_string(date->date_value()));
        else
            remote_value.set("value"sv, "Invalid Date"sv);

        // 3. Let remote value be a map matching the script.DateRemoteValue production in the local end definition, with
        //    the handle property set to handle id if it's not null, or omitted otherwise, and the value set to
        //    serialized.
        return remote_value;
    }

    // -> value has a [[MapData]] internal slot
    if (auto const* map = as_if<JS::Map>(object)) {
        // 1. Let remote value be a map matching the script.MapRemoteValue production in the local end definition, with
        //    the handle property set to handle id if it's not null, or omitted otherwise.
        auto remote_value = simple("map"sv);

        // 2. Set internal ids if needed with serialization internal map, remote value and value.
        set_internal_ids_if_needed(remote_value, object);

        // 3. Let serialized be null.
        // 4. If known object is false, and serialization options["maxObjectDepth"] is not 0, run the following steps:
        if (!known_object && serialization_options.max_object_depth != 0u) {
            // 1. Let serialized be the result of serialize as a mapping with CreateMapIterator(value, key+value),
            //    serialization options, ownership type, serialization internal map, realm, and session.
            GC::ConservativeVector<Array<JS::Value, 2>> entries;
            GC::RootVector<JS::Value> roots;
            (void)map->for_each_entry([&](JS::Value key, JS::Value entry_value) -> JS::ThrowCompletionOr<void> {
                roots.append(key);
                roots.append(entry_value);
                entries.append({ key, entry_value });
                return {};
            });

            // 5. If serialized is not null, set field value of remote value to serialized.
            remote_value.set("value"sv, serialize_as_a_mapping(entries, serialization_options, ownership_type));
        }
        return remote_value;
    }

    // -> value has a [[SetData]] internal slot
    if (auto const* set = as_if<JS::Set>(object)) {
        // 1. Let remote value be a map matching the script.SetRemoteValue production in the local end definition, with
        //    the handle property set to handle id if it's not null, or omitted otherwise.
        auto remote_value = simple("set"sv);

        // 2. Set internal ids if needed with serialization internal map, remote value and value.
        set_internal_ids_if_needed(remote_value, object);

        // 3. Let serialized be null.
        // 4. If known object is false, and serialization options["maxObjectDepth"] is not 0, run the following steps:
        if (!known_object && serialization_options.max_object_depth != 0u) {
            // 1. Let serialized be the result of serialize as a list with CreateSetIterator(value, value), serialization
            //    options, ownership type, serialization internal map, realm, and session.
            GC::RootVector<JS::Value> values;
            (void)set->for_each_value([&](JS::Value entry) -> JS::ThrowCompletionOr<void> {
                values.append(entry);
                return {};
            });

            // 5. If serialized is not null, set field value of remote value to serialized.
            remote_value.set("value"sv, serialize_as_a_list(values, serialization_options, ownership_type));
        }
        return remote_value;
    }

    // -> value has a [[WeakMapData]] internal slot
    if (object.is_weak_map())
        return simple("weakmap"sv);

    // -> value has a [[WeakSetData]] internal slot
    if (object.is_weak_set())
        return simple("weakset"sv);

    // -> value has a [[GeneratorState]] internal slot or [[AsyncGeneratorState]] internal slot
    if (object.is_generator())
        return simple("generator"sv);

    // -> value has an [[ErrorData]] internal slot
    // NB: A platform object such as a DOMException exposes error data too, but has no [[ErrorData]] internal slot.
    if (!object.is_platform_object() && object.has_error_data())
        return simple("error"sv);

    // -> value has a [[ProxyHandler]] internal slot and a [[ProxyTarget]] internal slot
    if (object.is_proxy())
        return simple("proxy"sv);

    // -> IsPromise(value)
    if (is<JS::Promise>(object))
        return simple("promise"sv);

    // -> value has a [[TypedArrayName]] internal slot
    if (object.is_typed_array())
        return simple("typedarray"sv);

    // -> value has an [[ArrayBufferData]] internal slot
    if (object.is_array_buffer())
        return simple("arraybuffer"sv);

    // -> value is a platform object that implements NodeList
    if (Bindings::impl_from<DOM::NodeList>(&object)) {
        // Let remote value be serialize an Array-like with script.NodeListRemoteValue, handle id, known object, value,
        // serialization options, ownership type, serialization internal map, realm, and session.
        GC::RootVector<JS::Value> items;
        if (!known_object && serialization_options.max_object_depth != 0u)
            items = array_like_items(vm, object);
        return serialize_an_array_like("nodelist"sv, move(handle_id), known_object, object, items, serialization_options, ownership_type);
    }

    // -> value is a platform object that implements HTMLCollection
    if (Bindings::impl_from<DOM::HTMLCollection>(&object)) {
        // Let remote value be serialize an Array-like with script.HTMLCollectionRemoteValue, handle id, known object,
        // value, serialization options, ownership type, known object, serialization internal map, realm, and session.
        GC::RootVector<JS::Value> items;
        if (!known_object && serialization_options.max_object_depth != 0u)
            items = array_like_items(vm, object);
        return serialize_an_array_like("htmlcollection"sv, move(handle_id), known_object, object, items, serialization_options, ownership_type);
    }

    // -> value is a platform object that implements Node
    if (auto* node = Bindings::impl_from<DOM::Node>(&object)) {
        // 1. Let shared id be get shared id for a node with value and session.
        auto shared_id = get_shared_id_for_a_node(*node);

        // 2. Let remote value be a map matching the script.NodeRemoteValue production in the local end definition, with
        //    the sharedId property set to shared id if it's not null, or omitted otherwise, and the handle property set
        //    to handle id if it's not null, or omitted otherwise.
        auto remote_value = simple("node"sv);
        if (shared_id.has_value())
            remote_value.set("sharedId"sv, shared_id.release_value());

        // 3. Set internal ids if needed with serialization internal map, remote value and value.
        set_internal_ids_if_needed(remote_value, object);

        // 4. Let serialized be null.
        // 5. If known object is false, run the following steps:
        if (!known_object) {
            // 1. Let serialized be a map.
            JsonObject serialized;

            // 2. Set serialized["nodeType"] to Get(value, "nodeType").
            serialized.set("nodeType"sv, node->node_type());

            // 3. Set node value to Get(value, "nodeValue").
            // 4. If node value is not null set serialized["nodeValue"] to node value.
            if (auto node_value = node->node_value(); node_value.has_value())
                serialized.set("nodeValue"sv, to_utf8(*node_value));

            // 5. If value implements Element or Attr:
            if (auto const* element = as_if<DOM::Element>(*node)) {
                // 1. Set serialized["localName"] to Get(value, "localName").
                serialized.set("localName"sv, to_utf8(element->local_name()));

                // 2. Set serialized["namespaceURI"] to Get(value, "namespaceURI")
                if (auto const& namespace_uri = element->namespace_uri(); namespace_uri.has_value())
                    serialized.set("namespaceURI"sv, to_utf8(*namespace_uri));
                else
                    serialized.set("namespaceURI"sv, JsonValue {});
            } else if (auto const* attr = as_if<DOM::Attr>(*node)) {
                serialized.set("localName"sv, to_utf8(attr->local_name()));
                if (auto const& namespace_uri = attr->namespace_uri(); namespace_uri.has_value())
                    serialized.set("namespaceURI"sv, to_utf8(*namespace_uri));
                else
                    serialized.set("namespaceURI"sv, JsonValue {});
            }

            // 6. Let child node count be the size of value's children.
            // 7. Set serialized["childNodeCount"] to child node count.
            serialized.set("childNodeCount"sv, node->child_count());

            // 8. If serialization options["maxDomDepth"] is equal to 0, or if value implements ShadowRoot and
            //    serialization options["includeShadowTree"] is "none", or if serialization options["includeShadowTree"]
            //    is "open" and value's mode is "closed", let children be null.
            auto const* shadow_root = as_if<DOM::ShadowRoot>(*node);
            bool children_are_null = serialization_options.max_dom_depth == 0u
                || (shadow_root && serialization_options.include_shadow_tree == SerializationOptions::IncludeShadowTree::None)
                || (shadow_root && serialization_options.include_shadow_tree == SerializationOptions::IncludeShadowTree::Open && shadow_root->mode() == Bindings::ShadowRootMode::Closed);

            // Otherwise, let children be an empty list and, for each node child in the children of value:
            if (!children_are_null) {
                JsonArray children;
                for (auto* child = node->first_child(); child; child = child->next_sibling()) {
                    // 1. Let child serialization options be a clone of serialization options.
                    auto child_serialization_options = serialization_options;

                    // 2. If child serialization options["maxDomDepth"] is not null, set child serialization
                    //    options["maxDomDepth"] to child serialization options["maxDomDepth"] - 1.
                    if (child_serialization_options.max_dom_depth.has_value())
                        child_serialization_options.max_dom_depth = *child_serialization_options.max_dom_depth - 1;

                    // 3. Let serialized be the result of serialize as a remote value with child, child serialization
                    //    options, ownership type, serialization internal map, realm, and session.
                    // 4. Append serialized to children.
                    auto child_value = BindingsGlue::wrap_node(*settings, *child);
                    children.must_append(serialize_as_a_remote_value(child_value, child_serialization_options, ownership_type));
                }

                // 9. If children is not null, set serialized["children"] to children.
                serialized.set("children"sv, move(children));
            }

            // 10. If value implements Element:
            if (auto* element = as_if<DOM::Element>(*node)) {
                // 1. Let attributes be a new map.
                JsonObject attributes;

                // 2. For each attribute in value's attribute list:
                element->for_each_attribute([&](DOM::Attr const& attribute) {
                    // 1. Let name be attribute's qualified name
                    // 2. Let value be attribute's value.
                    // 3. Set attributes[name] to value
                    attributes.set(to_utf8(attribute.name()), to_utf8(attribute.value()));
                });

                // 3. Set serialized["attributes"] to attributes.
                serialized.set("attributes"sv, move(attributes));

                // 4. Let shadow root be value's shadow root.
                // 5. If shadow root is null, let serialized shadow be null. Otherwise run the following substeps:
                JsonValue serialized_shadow;
                if (auto element_shadow_root = element->shadow_root()) {
                    // 1. Let serialized shadow be the result of serialize as a remote value with shadow root, serialization
                    //    options, ownership type, serialization internal map, realm, and session.
                    auto shadow_root_value = BindingsGlue::wrap_node(*settings, *element_shadow_root);
                    serialized_shadow = serialize_as_a_remote_value(shadow_root_value, serialization_options, ownership_type);
                }

                // 6. Set serialized["shadowRoot"] to serialized shadow.
                serialized.set("shadowRoot"sv, move(serialized_shadow));
            }

            // 11. If value implements ShadowRoot, set serialized["mode"] to value's mode.
            if (shadow_root)
                serialized.set("mode"sv, shadow_root->mode() == Bindings::ShadowRootMode::Closed ? "closed"sv : "open"sv);

            // 6. If serialized is not null, set field value of remote value to serialized.
            remote_value.set("value"sv, move(serialized));
        }

        return remote_value;
    }

    // -> value is a platform object that implements WindowProxy
    if (auto* window_proxy = HTML::WindowProxy::from_object(object)) {
        // 1. Let window be the value of value's [[WindowProxy]] internal slot.
        // 2. Let navigable be window's navigable.
        auto navigable = window_proxy->navigable();

        // 3. Let navigable id be the navigable id for navigable.
        // 4. Let serialized be a map matching the script.WindowProxyProperties production in the local end definition
        //    with the context property set to navigable id.
        JsonObject serialized;
        if (navigable)
            serialized.set("context"sv, navigable_id(*navigable));
        else
            serialized.set("context"sv, JsonValue {});

        // 5. Let remote value be a map matching the script.WindowProxyRemoteValue production in the local end
        //    definition, with the handle property set to handle id if it's not null, or omitted otherwise, and the value
        //    property set to serialized.
        auto remote_value = simple("window"sv);
        remote_value.set("value"sv, move(serialized));
        return remote_value;
    }

    // -> value is a platform object
    if (object.is_platform_object()) {
        // Let remote value be a map matching the script.ObjectRemoteValue production in the local end definition, with
        // the handle property set to handle id if it's not null, or omitted otherwise.
        return simple("object"sv);
    }

    // -> IsCallable(value)
    if (object.is_function()) {
        // Let remote value be a map matching the script.FunctionRemoteValue production in the local end definition,
        // with the handle property set to handle id if it's not null, or omitted otherwise.
        return simple("function"sv);
    }

    // -> Otherwise:
    // 1. Assert: Type(value) is Object
    // 2. Let remote value be a map matching the script.ObjectRemoteValue production in the local end definition, with
    //    the handle property set to handle id if it's not null, or omitted otherwise.
    remote_value = simple("object"sv);

    // 3. Set internal ids if needed with serialization internal map, remote value and value.
    set_internal_ids_if_needed(remote_value, object);

    // 4. Let serialized be null.
    // 5. If known object is false, and serialization options["maxObjectDepth"] is not 0, run the following steps:
    if (!known_object && serialization_options.max_object_depth != 0u) {
        // 1. Let serialized be the result of serialize as a mapping with EnumerableOwnPropertyNames(value, key+value),
        //    serialization options, ownership type, serialization internal map, realm, and session.
        GC::ConservativeVector<Array<JS::Value, 2>> entries;
        GC::RootVector<JS::Value> roots;
        if (auto properties = object.enumerable_own_property_names(JS::Object::PropertyKind::KeyAndValue); !properties.is_error()) {
            for (auto property : properties.value()) {
                auto& entry = property.as_object();
                auto key = entry.get(JS::PropertyKey { 0u });
                auto entry_value = entry.get(JS::PropertyKey { 1u });
                if (key.is_error() || entry_value.is_error())
                    continue;
                roots.append(key.value());
                roots.append(entry_value.value());
                entries.append({ key.value(), entry_value.value() });
            }
        }

        // 6. If serialized is not null, set field value of remote value to serialized.
        remote_value.set("value"sv, serialize_as_a_mapping(entries, serialization_options, ownership_type));
    }

    // 7. Return remote value
    return remote_value;
}

JsonValue serialize_as_a_remote_value(JS::Value value, SerializationOptions const& serialization_options, ResultOwnership ownership_type, HTML::EnvironmentSettingsObject& settings)
{
    Serializer serializer { .settings = settings, .depth = 0, .serialization_internal_map = {}, .serialized_objects = {}, .duplicated_markers = {}, .next_marker = 0, .internal_ids = {} };
    auto remote_value = serializer.serialize_as_a_remote_value(value, serialization_options, ownership_type);
    serializer.assign_internal_ids(remote_value);
    return remote_value;
}

// https://tc39.es/ecma262/#sec-stringtobigint
static Optional<::Crypto::SignedBigInteger> string_to_bigint(StringView string)
{
    // NB: StringIntegerLiteral allows the whitespace and line terminators that StrWhiteSpaceChar does around the
    //     literal, a sign only for a decimal literal, and an empty literal for 0n.
    auto literal = string.trim_whitespace();
    if (literal.is_empty())
        return ::Crypto::SignedBigInteger { 0 };

    u16 base = 10;
    if (literal.length() > 2 && literal[0] == '0') {
        auto prefix = to_ascii_lowercase(literal[1]);
        if (prefix == 'x')
            base = 16;
        else if (prefix == 'o')
            base = 8;
        else if (prefix == 'b')
            base = 2;
        if (base != 10)
            literal = literal.substring_view(2);
    }

    bool is_negative = false;
    if (base == 10 && (literal.starts_with('-') || literal.starts_with('+'))) {
        is_negative = literal[0] == '-';
        literal = literal.substring_view(1);
    }

    if (literal.is_empty())
        return {};
    for (auto character : literal) {
        if (!is_ascii_hex_digit(character) || parse_ascii_hex_digit(character) >= base)
            return {};
    }

    auto result = ::Crypto::SignedBigInteger::from_base(base, literal);
    if (result.is_error())
        return {};
    auto bigint = result.release_value();
    if (is_negative)
        bigint.negate();
    return bigint;
}

// https://tc39.es/ecma262/#sec-timeclip
static double time_clip(double time)
{
    // 1. If time is not finite, return NaN.
    if (!isfinite(time))
        return NAN;

    // 2. If abs(ℝ(time)) > 8.64 × 10**15, return NaN.
    if (fabs(time) > 8.64e15)
        return NAN;

    // 3. Return 𝔽(! ToIntegerOrInfinity(time)).
    return trunc(time) + 0.0;
}

// https://w3c.github.io/webdriver-bidi/#deserialize-primitive-protocol-value
static Optional<ErrorOr<JS::Value, Error>> deserialize_primitive_protocol_value(JS::VM& vm, JsonObject const& primitive_protocol_value)
{
    // 1. Let type be the value of the type field of primitive protocol value.
    auto type = primitive_protocol_value.get_string("type"sv);
    if (!type.has_value())
        return {};

    // 2. Let value be undefined.
    // 3. If primitive protocol value has field value:
    //     1. Let value be the value of the value field of primitive protocol value.
    auto value = primitive_protocol_value.get("value"sv);

    // 4. In the following list of conditions and associated steps, run the first set of steps for which the associated
    //    condition is true:
    // -> type is the string "undefined"
    if (*type == "undefined"sv) {
        // Return success with data undefined.
        return ErrorOr<JS::Value, Error> { JS::js_undefined() };
    }

    // -> type is the string "null"
    if (*type == "null"sv) {
        // Return success with data null.
        return ErrorOr<JS::Value, Error> { JS::js_null() };
    }

    // -> type is the string "string"
    if (*type == "string"sv) {
        // Return success with data value.
        if (!value.has_value() || !value->is_string())
            return ErrorOr<JS::Value, Error> { invalid_argument("A string value must have a string value"sv) };
        return ErrorOr<JS::Value, Error> { JS::PrimitiveString::create(vm, Utf16String::from_utf8(value->as_string())) };
    }

    // -> type is the string "number"
    if (*type == "number"sv) {
        if (!value.has_value())
            return ErrorOr<JS::Value, Error> { invalid_argument("A number value must have a value"sv) };

        // 1. If Type(value) is Number, return success with data value.
        if (value->is_number())
            return ErrorOr<JS::Value, Error> { JS::Value(value->get_number_with_precision_loss<double>().value()) };

        // 2. Assert: Type(value) is String.
        if (!value->is_string())
            return ErrorOr<JS::Value, Error> { invalid_argument("A number value must be a number or a special number"sv) };
        auto const& string = value->as_string();

        // 3. If value is the string "NaN", return success with data NaN.
        if (string == "NaN"sv)
            return ErrorOr<JS::Value, Error> { JS::Value(NAN) };

        // 4. Let number_result be StringToNumber(value).
        // 5. If number_result is NaN, return error with error code invalid argument
        // 6. Return success with data number_result.
        // NB: The special numbers other than NaN are the strings StringToNumber gives a number for that JSON has no
        //     representation of.
        if (string == "-0"sv)
            return ErrorOr<JS::Value, Error> { JS::Value(-0.0) };
        if (string == "Infinity"sv)
            return ErrorOr<JS::Value, Error> { JS::Value(INFINITY) };
        if (string == "-Infinity"sv)
            return ErrorOr<JS::Value, Error> { JS::Value(-INFINITY) };
        return ErrorOr<JS::Value, Error> { invalid_argument("A number value must be a number or a special number"sv) };
    }

    // -> type is the string "boolean"
    if (*type == "boolean"sv) {
        // Return success with data value.
        if (!value.has_value() || !value->is_bool())
            return ErrorOr<JS::Value, Error> { invalid_argument("A boolean value must have a boolean value"sv) };
        return ErrorOr<JS::Value, Error> { JS::Value(value->as_bool()) };
    }

    // -> type is the string "bigint"
    if (*type == "bigint"sv) {
        // 1. Let bigint_result be StringToBigInt(value).
        // 2. If bigint_result is undefined, return error with error code invalid argument
        // 3. Return success with data bigint_result.
        if (!value.has_value() || !value->is_string())
            return ErrorOr<JS::Value, Error> { invalid_argument("A bigint value must have a string value"sv) };
        auto bigint = string_to_bigint(value->as_string());
        if (!bigint.has_value())
            return ErrorOr<JS::Value, Error> { invalid_argument("A bigint value must be a valid BigInt"sv) };
        return ErrorOr<JS::Value, Error> { JS::Value(JS::BigInt::create(vm, bigint.release_value())) };
    }

    return {};
}

// https://w3c.github.io/webdriver-bidi/#deserialize-shared-reference
ErrorOr<JS::Value, Error> deserialize_shared_reference(JsonObject const& shared_reference, HTML::EnvironmentSettingsObject& settings)
{
    // 1. Assert shared reference matches the script.SharedReference production.
    // 2. Let navigable be get the navigable with realm.
    // https://w3c.github.io/webdriver-bidi/#get-the-navigable
    auto* window = HTML::window_from_global_object(settings.global_object());
    auto navigable = window ? window->associated_document().navigable() : nullptr;

    // 3. If navigable is null, return error with error code no such node.
    if (!navigable)
        return Error::from_code(ErrorCode::NoSuchNode, "Realm is not the realm of a window"sv);

    // 4. Let shared id be the value of the sharedId field of shared reference.
    auto shared_id = shared_reference.get_string("sharedId"sv).value();

    // 5. Let node be result of trying to get a node with session, navigable and shared id.
    // NB: Getting a node looks it up in the node id map of navigable's browsing context group, so a node of another
    //     browsing context group is not found.
    auto node = get_node(shared_id);
    auto browsing_context_group_id = [](GC::Ptr<HTML::BrowsingContext> browsing_context) -> Optional<u64> {
        if (!browsing_context || !browsing_context->top_level_browsing_context())
            return {};
        return browsing_context->top_level_browsing_context()->browsing_context_group_id();
    };
    if (node && browsing_context_group_id(node->document().browsing_context()) != browsing_context_group_id(navigable->active_browsing_context()))
        node = nullptr;

    // 6. If node is null, return error with error code no such node.
    if (!node)
        return Error::from_code(ErrorCode::NoSuchNode, "Node not found"sv);

    // 7. Let environment settings be the environment settings object whose realm execution context's Realm component
    //    is realm.
    // 8. If node's node document's origin is not same origin domain with environment settings's origin then return
    //    error with error code no such node.
    if (!node->document().origin().is_same_origin_domain(settings.origin()))
        return Error::from_code(ErrorCode::NoSuchNode, "Node is not accessible from the realm"sv);

    // 9. Let realm global object be the global object of the realm.
    // 10. If the realm global object is SandboxWindowProxy object, set node to the SandboxProxy wrapping node in the
    //     realm.

    // 11. Return success with data node.
    return BindingsGlue::wrap_node(settings, *node);
}

// https://w3c.github.io/webdriver-bidi/#deserialize-remote-object-reference
static ErrorOr<JS::Value, Error> deserialize_remote_object_reference(JsonObject const& remote_object_reference, HTML::EnvironmentSettingsObject& settings)
{
    // 1. Let handle id be the value of the handle field of remote object reference.
    auto handle_id = remote_object_reference.get_string("handle"sv).value();

    // 2. Let handle map be realm's handle object map
    // 3. If handle map does not contain handle id, then return error with error code no such handle.
    auto handle_map = handle_object_maps().find(realm_id(settings));
    if (handle_map == handle_object_maps().end())
        return Error::from_code(ErrorCode::NoSuchHandle, "Handle not found"sv);
    auto object = handle_map->value.find(handle_id);
    if (object == handle_map->value.end())
        return Error::from_code(ErrorCode::NoSuchHandle, "Handle not found"sv);

    // 4. Return success with data handle map[handle id].
    return object->value.first();
}

static ErrorOr<GC::RootVector<JS::Value>, Error> deserialize_value_list(JsonValue const* serialized_value_list, HTML::EnvironmentSettingsObject& settings)
{
    if (!serialized_value_list || !serialized_value_list->is_array())
        return invalid_argument("Expected a list of local values"sv);

    // https://w3c.github.io/webdriver-bidi/#deserialize-value-list
    // 1. Let deserialized values be a new list.
    GC::RootVector<JS::Value> deserialized_values;

    // 2. For each serialized value in the serialized value list:
    for (auto const& serialized_value : serialized_value_list->as_array().values()) {
        // 1. Let deserialized value be result of trying to deserialize local value given serialized value, realm and
        //    session.
        // 2. Append deserialized value to deserialized values;
        deserialized_values.append(TRY(deserialize_local_value(serialized_value, settings)));
    }

    // 3. Return success with data deserialized values.
    return deserialized_values;
}

// https://w3c.github.io/webdriver-bidi/#deserialize-key-value-list
static ErrorOr<GC::ConservativeVector<Array<JS::Value, 2>>, Error> deserialize_key_value_list(JsonValue const* serialized_key_value_list, HTML::EnvironmentSettingsObject& settings, GC::RootVector<JS::Value>& roots)
{
    if (!serialized_key_value_list || !serialized_key_value_list->is_array())
        return invalid_argument("Expected a list of key-value pairs"sv);

    auto& vm = settings.global_object().vm();

    // 1. Let deserialized key-value list be a new list.
    GC::ConservativeVector<Array<JS::Value, 2>> deserialized_key_value_list;

    // 2. For each serialized key-value in the serialized key-value list:
    for (auto const& serialized_key_value : serialized_key_value_list->as_array().values()) {
        // 1. If size of serialized key-value is not 2, return error with error code invalid argument.
        if (!serialized_key_value.is_array() || serialized_key_value.as_array().size() != 2)
            return invalid_argument("A key-value pair must be a list of 2 values"sv);

        // 2. Let serialized key be serialized key-value[0].
        auto const& serialized_key = serialized_key_value.as_array().at(0);

        // 3. If serialized key is a string, let deserialized key be serialized key.
        JS::Value deserialized_key;
        if (serialized_key.is_string())
            deserialized_key = JS::PrimitiveString::create(vm, Utf16String::from_utf8(serialized_key.as_string()));
        // 4. Otherwise let deserialized key be result of trying to given deserialize local value with serialized key,
        //    realm and session.
        else
            deserialized_key = TRY(deserialize_local_value(serialized_key, settings));
        roots.append(deserialized_key);

        // 5. Let serialized value be serialized key-value[1].
        auto const& serialized_value = serialized_key_value.as_array().at(1);

        // 6. Let deserialized value be result of trying to deserialize local value given serialized value, realm and
        //    session.
        auto deserialized_value = TRY(deserialize_local_value(serialized_value, settings));
        roots.append(deserialized_value);

        // 7. Append CreateArrayFromList(«deserialized key, deserialized value») to deserialized key-value list.
        deserialized_key_value_list.append({ deserialized_key, deserialized_value });
    }

    // 3. Return success with data deserialized key-value list.
    return deserialized_key_value_list;
}

// https://tc39.es/ecma262/#sec-date-time-string-format
static Optional<double> parse_date_time_string(StringView string)
{
    GenericLexer lexer { string };

    auto parse_digits = [&](size_t count) -> Optional<i64> {
        i64 value = 0;
        for (size_t i = 0; i < count; ++i) {
            if (lexer.is_eof() || !is_ascii_digit(lexer.peek()))
                return {};
            value = value * 10 + (lexer.consume() - '0');
        }
        return value;
    };

    // YYYY, or ±YYYYYY for an expanded year.
    i64 year = 0;
    if (lexer.next_is('+') || lexer.next_is('-')) {
        auto sign = lexer.consume() == '-' ? -1 : 1;
        auto digits = parse_digits(6);
        if (!digits.has_value() || (sign == -1 && *digits == 0))
            return {};
        year = sign * *digits;
    } else {
        auto digits = parse_digits(4);
        if (!digits.has_value())
            return {};
        year = *digits;
    }

    i64 month = 1;
    i64 day = 1;
    if (lexer.consume_specific('-')) {
        auto digits = parse_digits(2);
        if (!digits.has_value() || *digits < 1 || *digits > 12)
            return {};
        month = *digits;
        if (lexer.consume_specific('-')) {
            digits = parse_digits(2);
            if (!digits.has_value() || *digits < 1 || *digits > 31)
                return {};
            day = *digits;
        }
    }

    i64 hour = 0;
    i64 minute = 0;
    i64 second = 0;
    i64 millisecond = 0;
    i64 offset_minutes = 0;
    if (lexer.consume_specific('T')) {
        auto digits = parse_digits(2);
        if (!digits.has_value() || *digits > 24 || !lexer.consume_specific(':'))
            return {};
        hour = *digits;
        digits = parse_digits(2);
        if (!digits.has_value() || *digits > 59)
            return {};
        minute = *digits;
        if (lexer.consume_specific(':')) {
            digits = parse_digits(2);
            if (!digits.has_value() || *digits > 59)
                return {};
            second = *digits;
            if (lexer.consume_specific('.')) {
                digits = parse_digits(3);
                if (!digits.has_value())
                    return {};
                millisecond = *digits;
            }
        }
        if (hour == 24 && (minute != 0 || second != 0 || millisecond != 0))
            return {};

        // FIXME: A date-time form without a UTC offset is a local time, which this treats as UTC.
        if (lexer.consume_specific('Z')) {
        } else if (lexer.next_is('+') || lexer.next_is('-')) {
            auto sign = lexer.consume() == '-' ? -1 : 1;
            auto offset_hours = parse_digits(2);
            if (!offset_hours.has_value() || *offset_hours > 23 || !lexer.consume_specific(':'))
                return {};
            auto offset_minutes_part = parse_digits(2);
            if (!offset_minutes_part.has_value() || *offset_minutes_part > 59)
                return {};
            offset_minutes = sign * (*offset_hours * 60 + *offset_minutes_part);
        }
    }

    if (!lexer.is_eof())
        return {};

    auto date = JS::make_day(static_cast<double>(year), static_cast<double>(month - 1), static_cast<double>(day));
    auto time = JS::make_time(static_cast<double>(hour), static_cast<double>(minute), static_cast<double>(second), static_cast<double>(millisecond));
    return JS::make_date(date, time) - static_cast<double>(offset_minutes) * 60'000;
}

// https://w3c.github.io/webdriver-bidi/#deserialize-local-value
ErrorOr<JS::Value, Error> deserialize_local_value(JsonValue const& local_protocol_value_json, HTML::EnvironmentSettingsObject& settings)
{
    auto& vm = settings.global_object().vm();

    if (!local_protocol_value_json.is_object())
        return invalid_argument("A local value must be an object"sv);
    auto const& local_protocol_value = local_protocol_value_json.as_object();

    // 1. If local protocol value matches the script.RemoteReference production, return deserialize remote reference of
    //    given local protocol value, realm and session.
    // https://w3c.github.io/webdriver-bidi/#deserialize-remote-reference
    // NB: If the provided reference has both handle and sharedId, the algorithm will ignore handle and respect only
    //     sharedId.
    if (auto shared_id = local_protocol_value.get("sharedId"sv); shared_id.has_value()) {
        if (!shared_id->is_string())
            return invalid_argument("sharedId must be a string"sv);
        if (auto handle = local_protocol_value.get("handle"sv); handle.has_value() && !handle->is_string())
            return invalid_argument("handle must be a string"sv);
        return deserialize_shared_reference(local_protocol_value, settings);
    }
    if (auto handle = local_protocol_value.get("handle"sv); handle.has_value()) {
        if (!handle->is_string())
            return invalid_argument("handle must be a string"sv);
        return deserialize_remote_object_reference(local_protocol_value, settings);
    }

    // 2. If local protocol value matches the script.PrimitiveProtocolValue production, return deserialize primitive
    //    protocol value with local protocol value.
    if (auto primitive = deserialize_primitive_protocol_value(vm, local_protocol_value); primitive.has_value())
        return primitive.release_value();

    // 4. Let type be the value of the type field of local protocol value or undefined if no such a field.
    auto type = local_protocol_value.get_string("type"sv);
    if (!type.has_value())
        return invalid_argument("A local value must have a type"sv);

    // 3. If local protocol value matches the script.ChannelValue production, return create a channel with session, realm
    //    and local protocol value.
    if (*type == "channel"sv) {
        // script.ChannelValue = {
        //   type: "channel",
        //   value: script.ChannelProperties,
        // }
        // script.ChannelProperties = {
        //   channel: script.Channel,
        //   ? serializationOptions: script.SerializationOptions,
        //   ? ownership: script.ResultOwnership,
        // }
        auto properties = local_protocol_value.get_object("value"sv);
        if (!properties.has_value() || !properties->get_string("channel"sv).has_value())
            return invalid_argument("A channel value must have channel properties with a channel"sv);
        if (auto options = properties->get("serializationOptions"sv); options.has_value())
            TRY(parse_serialization_options(*options));
        if (auto ownership = properties->get("ownership"sv); ownership.has_value())
            TRY(parse_result_ownership(*ownership));
        return create_a_channel(settings, local_protocol_value);
    }

    // 5. Let value be the value of the value field of local protocol value or undefined if no such a field.
    auto value = local_protocol_value.get("value"sv);
    auto const* value_pointer = value.has_value() ? &value.value() : nullptr;

    // 6. In the following list of conditions and associated steps, run the first set of steps for which the associated
    //    condition is true:
    // -> type is the string "array"
    if (*type == "array"sv) {
        // 1. Let deserialized value list be a result of trying to deserialize value list given value, realm and session.
        auto deserialized_value_list = TRY(deserialize_value_list(value_pointer, settings));

        // 2. Return success with data CreateArrayFromList(deserialized value list).
        return BindingsGlue::create_array(settings, deserialized_value_list.span());
    }

    // -> type is the string "date"
    if (*type == "date"sv) {
        // 1. If value does not match Date Time String Format, return error with error code invalid argument.
        if (!value_pointer || !value_pointer->is_string())
            return invalid_argument("A date value must be a string"sv);
        auto date_value = parse_date_time_string(value_pointer->as_string());
        if (!date_value.has_value())
            return invalid_argument("A date value must match the Date Time String Format"sv);

        // 2. Let date result be Construct(Date, value).
        // 3. Assert: date result is not an abrupt completion.
        // 4. Return success with data date result.
        return BindingsGlue::create_date(settings, time_clip(*date_value));
    }

    // -> type is the string "map"
    if (*type == "map"sv) {
        // 1. Let deserialized key-value list be a result of trying to deserialize key-value list with value, realm and
        //    session.
        GC::RootVector<JS::Value> roots;
        auto deserialized_key_value_list = TRY(deserialize_key_value_list(value_pointer, settings, roots));

        // 2. Let iterable be CreateArrayFromList(deserialized key-value list)
        // 3. Return success with data Map(iterable).
        auto map = BindingsGlue::create_map(settings);
        for (auto const& entry : deserialized_key_value_list)
            map->map_set(entry[0], entry[1]);
        return map;
    }

    // -> type is the string "object"
    if (*type == "object"sv) {
        // 1. Let deserialized key-value list be a result of trying to deserialize key-value list with value, realm and
        //    session.
        GC::RootVector<JS::Value> roots;
        auto deserialized_key_value_list = TRY(deserialize_key_value_list(value_pointer, settings, roots));

        // 2. Let iterable be CreateArrayFromList(deserialized key-value list)
        // 3. Return success with data Object.fromEntries(iterable).
        auto object = BindingsGlue::create_object(settings);
        for (auto const& entry : deserialized_key_value_list) {
            auto property_key = JS::PropertyKey::from_value(vm, entry[0]);
            if (property_key.is_error())
                return invalid_argument("An object key must be a property key"sv);
            MUST(object->create_data_property_or_throw(property_key.release_value(), entry[1]));
        }
        return object;
    }

    // -> type is the string "regexp"
    if (*type == "regexp"sv) {
        if (!value_pointer || !value_pointer->is_object())
            return invalid_argument("A regexp value must be an object"sv);

        // 1. Let pattern be the value of the pattern field of local protocol value.
        auto pattern = value_pointer->as_object().get_string("pattern"sv);
        if (!pattern.has_value())
            return invalid_argument("A regexp value must have a string pattern"sv);

        // 2. Let flags be the value of the flags field of local protocol value or undefined if no such a field.
        JS::Value flags = JS::js_undefined();
        if (auto flags_value = value_pointer->as_object().get("flags"sv); flags_value.has_value()) {
            if (!flags_value->is_string())
                return invalid_argument("The flags of a regexp value must be a string"sv);
            flags = JS::PrimitiveString::create(vm, Utf16String::from_utf8(flags_value->as_string()));
        }

        // 3. Let regex_result be Regexp(pattern, flags). If this throws exception, return error with error code invalid
        //    argument.
        auto regex_result = JS::regexp_create(vm, JS::PrimitiveString::create(vm, Utf16String::from_utf8(*pattern)), flags);
        if (regex_result.is_error())
            return invalid_argument("The regexp value is not a valid regular expression"sv);

        // 4. Return success with data regex_result.
        return JS::Value { regex_result.value() };
    }

    // -> type is the string "set"
    if (*type == "set"sv) {
        // 1. Let deserialized value list be a result of trying to deserialize value list given value, realm and session.
        auto deserialized_value_list = TRY(deserialize_value_list(value_pointer, settings));

        // 2. Let iterable be CreateArrayFromList(deserialized key-value list)
        // 3. Return success with data Set object(iterable).
        auto set = BindingsGlue::create_set(settings);
        for (auto item : deserialized_value_list)
            set->set_add(item);
        return set;
    }

    // -> otherwise
    //    Return error with error code invalid argument.
    return invalid_argument("Unknown local value type"sv);
}

static JsonObject stack_frame(Utf16View function_name, Utf16View url, JS::Position position)
{
    // script.StackFrame = {
    //   columnNumber: js-uint,
    //   functionName: text,
    //   lineNumber: js-uint,
    //   url: text,
    // }
    // NB: The line and column numbers of a stack frame are zero-based, while those the engine keeps are one-based.
    JsonObject frame_info;
    frame_info.set("columnNumber"sv, position.column > 0 ? position.column - 1 : 0);
    frame_info.set("functionName"sv, to_utf8(function_name));
    frame_info.set("lineNumber"sv, position.line > 0 ? position.line - 1 : 0);
    frame_info.set("url"sv, to_utf8(url));
    return frame_info;
}

// https://w3c.github.io/webdriver-bidi/#construct-a-stack-trace
static JsonObject construct_a_stack_trace(Vector<JS::TracebackFrame, 32> const& stack)
{
    // 1. Let call frames be a new list.
    JsonArray call_frames;

    // 2. For each stack frame frame in stack, starting from the most recently executed frame, run the following steps:
    for (auto const& frame : stack) {
        // 1. Let url be the result of running the URL serializer, given the URL of frame's script url.
        // 2. Let frame info be a new map matching the script.StackFrame production, with the url field set to url, the
        //    functionName field set to frame's function, the lineNumber field set to frame's line number and the
        //    columnNumber field set to frame's column number.
        auto const& source_range = frame.source_range();

        // NB: A frame of native code has no script.
        if (source_range.filename().is_empty())
            continue;

        // 3. Append frame info to call frames.
        call_frames.must_append(stack_frame(frame.function_name, source_range.filename(), source_range.start));
    }

    // 4. Let stack trace be a new map matching the script.StackTrace production, with the callFrames property set to
    //    call frames.
    JsonObject stack_trace;
    stack_trace.set("callFrames"sv, move(call_frames));

    // 5. Return stack trace.
    return stack_trace;
}

// https://w3c.github.io/webdriver-bidi/#stack-trace-for-an-exception
JsonObject stack_trace_for_an_exception(JS::Value exception)
{
    // 1. If exception is a value that has been thrown as an exception, let record be the Completion Record created to
    //    throw exception. Otherwise let record be exception.
    // 2. Let stack be the list of stack frames corresponding to execution at the point record was created.
    // NB: An error keeps the stack it was created on.
    Vector<JS::TracebackFrame, 32> stack;
    if (exception.is_object()) {
        if (auto const* error_data = exception.as_object().error_data())
            stack = error_data->traceback();
    }

    // 3. Return construct a stack trace given stack.
    return construct_a_stack_trace(stack);
}

// https://w3c.github.io/webdriver-bidi/#current-stack-trace
JsonObject current_stack_trace(JS::VM& vm)
{
    // The current stack trace is the result of construct a stack trace given a list of stack frames representing the
    // callstack of the running execution context.
    return construct_a_stack_trace(JS::ErrorDataCell::capture(vm)->traceback());
}

// https://w3c.github.io/webdriver-bidi/#get-exception-details
String textual_description_of_an_exception(HTML::EnvironmentSettingsObject& settings, JS::Value exception)
{
    if (exception.is_object() && exception.as_object().has_error_data()) {
        auto& object = exception.as_object();
        auto& vm = settings.global_object().vm();
        auto name = object.get_without_side_effects(vm.names.name);
        auto message = object.get_without_side_effects(vm.names.message);
        auto name_string = name.is_string() ? to_utf8(name.as_string().utf16_string_view()) : "Error"_string;
        auto message_string = message.is_string() ? to_utf8(message.as_string().utf16_string_view()) : String {};
        return message_string.is_empty() ? name_string : MUST(String::formatted("{}: {}", name_string, message_string));
    }
    return to_utf8(exception.to_utf16_string_without_side_effects());
}

JsonObject get_exception_details(HTML::EnvironmentSettingsObject& settings, JS::Value exception, ResultOwnership ownership_type)
{
    // 1. Assert: record.[[Type]] is throw.

    // 2. Let text be an implementation-defined textual description of the error represented by record.
    auto text = textual_description_of_an_exception(settings, exception);

    // 3. Let serialization options be a map matching the script.SerializationOptions production with the fields set to
    //    their default values.
    SerializationOptions serialization_options;

    // 4. Let exception be the result of serialize as a remote value with record.[[Value]], serialization options,
    //    ownership type, a new map as serialization internal map, realm and session.
    auto serialized_exception = serialize_as_a_remote_value(exception, serialization_options, ownership_type, settings);

    // 5. Let stack trace be the stack trace for an exception given record.
    auto stack_trace = stack_trace_for_an_exception(exception);

    // 6. If stack trace has size of 1 or greater, let line number be value of the lineNumber field in stack trace[0], and
    //    let column number be the value of the columnNumber field stack trace[0]. Otherwise let line number and column
    //    number be 0.
    JsonValue line_number = 0;
    JsonValue column_number = 0;
    if (auto call_frames = stack_trace.get_array("callFrames"sv); call_frames.has_value() && call_frames->size() > 0) {
        auto const& first_frame = call_frames->at(0).as_object();
        line_number = *first_frame.get("lineNumber"sv);
        column_number = *first_frame.get("columnNumber"sv);
    }

    // 7. Let exception details be a map matching the script.ExceptionDetails production, with the text field set to
    //    text, the exception field set to exception, the lineNumber field set to line number, the columnNumber field set
    //    to column number, and the stackTrace field set to stack trace.
    JsonObject exception_details;
    exception_details.set("columnNumber"sv, move(column_number));
    exception_details.set("exception"sv, move(serialized_exception));
    exception_details.set("lineNumber"sv, move(line_number));
    exception_details.set("stackTrace"sv, move(stack_trace));
    exception_details.set("text"sv, move(text));

    // 8. Return exception details.
    return exception_details;
}

}
