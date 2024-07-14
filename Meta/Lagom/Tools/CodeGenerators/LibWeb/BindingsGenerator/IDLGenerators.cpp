/*
 * Copyright (c) 2020-2023, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021-2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023-2024, Kenneth Myhra <kennethmyhra@serenityos.org>
 * Copyright (c) 2023-2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2023-2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "IDLGenerators.h"
#include "Namespaces.h"
#include <AK/Array.h>
#include <AK/LexicalPath.h>
#include <AK/Queue.h>
#include <AK/QuickSort.h>
#include <LibIDL/Types.h>

namespace IDL {

Vector<StringView> g_header_search_paths;

// FIXME: Generate this automatically somehow.
static bool is_platform_object(Type const& type)
{
    // NOTE: This is a hand-curated subset of platform object types that are actually relevant
    // in places where this function is used. If you add IDL code and get compile errors, you
    // might simply need to add another type here.
    static constexpr Array types = {
        "AbortSignal"sv,
        "Animation"sv,
        "AnimationEffect"sv,
        "AnimationTimeline"sv,
        "Attr"sv,
        "AudioNode"sv,
        "AudioParam"sv,
        "AudioScheduledSourceNode"sv,
        "AudioTrack"sv,
        "BaseAudioContext"sv,
        "Blob"sv,
        "CanvasGradient"sv,
        "CanvasPattern"sv,
        "CanvasRenderingContext2D"sv,
        "CloseWatcher"sv,
        "CryptoKey"sv,
        "DataTransfer"sv,
        "Document"sv,
        "DocumentType"sv,
        "DOMRectReadOnly"sv,
        "DynamicsCompressorNode"sv,
        "ElementInternals"sv,
        "EventTarget"sv,
        "FileList"sv,
        "FontFace"sv,
        "FormData"sv,
        "HTMLCollection"sv,
        "ImageBitmap"sv,
        "ImageData"sv,
        "Instance"sv,
        "IntersectionObserverEntry"sv,
        "KeyframeEffect"sv,
        "MediaList"sv,
        "Memory"sv,
        "MessagePort"sv,
        "Module"sv,
        "MutationRecord"sv,
        "NamedNodeMap"sv,
        "NavigationDestination"sv,
        "NavigationHistoryEntry"sv,
        "Node"sv,
        "Path2D"sv,
        "PerformanceEntry"sv,
        "PerformanceMark"sv,
        "PeriodicWave"sv,
        "PointerEvent"sv,
        "ReadableStreamBYOBReader"sv,
        "ReadableStreamDefaultReader"sv,
        "RadioNodeList"sv,
        "Range"sv,
        "ReadableStream"sv,
        "Request"sv,
        "Selection"sv,
        "SVGTransform"sv,
        "ShadowRoot"sv,
        "Table"sv,
        "Text"sv,
        "TextMetrics"sv,
        "TextTrack"sv,
        "URLSearchParams"sv,
        "VideoTrack"sv,
        "VideoTrackList"sv,
        "WebGLRenderingContext"sv,
        "Window"sv,
        "WritableStream"sv,
    };
    if (type.name().ends_with("Element"sv))
        return true;
    if (type.name().ends_with("Event"sv))
        return true;
    if (types.span().contains_slow(type.name()))
        return true;
    return false;
}

// FIXME: Generate this automatically somehow.
static bool is_javascript_builtin(Type const& type)
{
    // NOTE: This is a hand-curated subset of JavaScript built-in types that are actually relevant
    // in places where this function is used. If you add IDL code and get compile errors, you
    // might simply need to add another type here.
    static constexpr Array types = {
        "ArrayBuffer"sv,
        "Float32Array"sv,
        "Float64Array"sv,
        "Uint8Array"sv
    };

    return types.span().contains_slow(type.name());
}

static StringView sequence_storage_type_to_cpp_storage_type_name(SequenceStorageType sequence_storage_type)
{
    switch (sequence_storage_type) {
    case SequenceStorageType::Vector:
        return "Vector"sv;
    case SequenceStorageType::MarkedVector:
        return "JS::MarkedVector"sv;
    default:
        VERIFY_NOT_REACHED();
    }
}

CppType idl_type_name_to_cpp_type(Type const& type, Interface const& interface);

static ByteString union_type_to_variant(UnionType const& union_type, Interface const& interface)
{
    StringBuilder builder;
    builder.append("Variant<"sv);

    auto flattened_types = union_type.flattened_member_types();
    for (size_t type_index = 0; type_index < flattened_types.size(); ++type_index) {
        auto& type = flattened_types.at(type_index);

        if (type_index > 0)
            builder.append(", "sv);

        auto cpp_type = idl_type_name_to_cpp_type(type, interface);
        builder.append(cpp_type.name);
    }

    if (union_type.includes_undefined())
        builder.append(", Empty"sv);

    builder.append('>');
    return builder.to_byte_string();
}

CppType idl_type_name_to_cpp_type(Type const& type, Interface const& interface)
{
    if (is_platform_object(type) || type.name() == "WindowProxy"sv)
        return { .name = ByteString::formatted("JS::Handle<{}>", type.name()), .sequence_storage_type = SequenceStorageType::MarkedVector };

    if (is_javascript_builtin(type))
        return { .name = ByteString::formatted("JS::Handle<JS::{}>", type.name()), .sequence_storage_type = SequenceStorageType::MarkedVector };

    if (interface.callback_functions.contains(type.name()))
        return { .name = "JS::Handle<WebIDL::CallbackType>", .sequence_storage_type = SequenceStorageType::MarkedVector };

    if (type.is_string())
        return { .name = "String", .sequence_storage_type = SequenceStorageType::Vector };

    if ((type.name() == "double" || type.name() == "unrestricted double") && !type.is_nullable())
        return { .name = "double", .sequence_storage_type = SequenceStorageType::Vector };

    if ((type.name() == "float" || type.name() == "unrestricted float") && !type.is_nullable())
        return { .name = "float", .sequence_storage_type = SequenceStorageType::Vector };

    if (type.name() == "boolean" && !type.is_nullable())
        return { .name = "bool", .sequence_storage_type = SequenceStorageType::Vector };

    if (type.name() == "unsigned long" && !type.is_nullable())
        return { .name = "WebIDL::UnsignedLong", .sequence_storage_type = SequenceStorageType::Vector };

    if (type.name() == "short" && !type.is_nullable())
        return { .name = "WebIDL::Short", .sequence_storage_type = SequenceStorageType::Vector };

    if (type.name() == "unsigned short" && !type.is_nullable())
        return { .name = "WebIDL::UnsignedShort", .sequence_storage_type = SequenceStorageType::Vector };

    if (type.name() == "long long" && !type.is_nullable())
        return { .name = "WebIDL::LongLong", .sequence_storage_type = SequenceStorageType::Vector };

    if (type.name() == "unsigned long long" && !type.is_nullable())
        return { .name = "WebIDL::UnsignedLongLong", .sequence_storage_type = SequenceStorageType::Vector };

    if (type.name() == "long" && !type.is_nullable())
        return { .name = "WebIDL::Long", .sequence_storage_type = SequenceStorageType::Vector };

    if (type.name() == "any" || type.name() == "undefined")
        return { .name = "JS::Value", .sequence_storage_type = SequenceStorageType::MarkedVector };

    if (type.name() == "object")
        return { .name = "JS::Handle<JS::Object>", .sequence_storage_type = SequenceStorageType::Vector };

    if (type.name() == "BufferSource")
        return { .name = "JS::Handle<WebIDL::BufferSource>", .sequence_storage_type = SequenceStorageType::MarkedVector };

    if (type.name() == "ArrayBufferView")
        return { .name = "JS::Handle<WebIDL::ArrayBufferView>", .sequence_storage_type = SequenceStorageType::MarkedVector };

    if (type.name() == "File")
        return { .name = "JS::Handle<FileAPI::File>", .sequence_storage_type = SequenceStorageType::MarkedVector };

    if (type.name() == "Function")
        return { .name = "JS::NonnullGCPtr<WebIDL::CallbackType>", .sequence_storage_type = SequenceStorageType::MarkedVector };

    if (type.name() == "sequence") {
        auto& parameterized_type = verify_cast<ParameterizedType>(type);
        auto& sequence_type = parameterized_type.parameters().first();
        auto sequence_cpp_type = idl_type_name_to_cpp_type(sequence_type, interface);
        auto storage_type_name = sequence_storage_type_to_cpp_storage_type_name(sequence_cpp_type.sequence_storage_type);

        if (sequence_cpp_type.sequence_storage_type == SequenceStorageType::MarkedVector)
            return { .name = storage_type_name, .sequence_storage_type = SequenceStorageType::Vector };

        return { .name = ByteString::formatted("{}<{}>", storage_type_name, sequence_cpp_type.name), .sequence_storage_type = SequenceStorageType::Vector };
    }

    if (type.name() == "record") {
        auto& parameterized_type = verify_cast<ParameterizedType>(type);
        auto& record_key_type = parameterized_type.parameters()[0];
        auto& record_value_type = parameterized_type.parameters()[1];
        auto record_key_cpp_type = idl_type_name_to_cpp_type(record_key_type, interface);
        auto record_value_cpp_type = idl_type_name_to_cpp_type(record_value_type, interface);

        return { .name = ByteString::formatted("OrderedHashMap<{}, {}>", record_key_cpp_type.name, record_value_cpp_type.name), .sequence_storage_type = SequenceStorageType::Vector };
    }

    if (is<UnionType>(type)) {
        auto& union_type = verify_cast<UnionType>(type);
        return { .name = union_type_to_variant(union_type, interface), .sequence_storage_type = SequenceStorageType::Vector };
    }

    if (!type.is_nullable()) {
        for (auto& dictionary : interface.dictionaries) {
            if (type.name() == dictionary.key)
                return { .name = type.name(), .sequence_storage_type = SequenceStorageType::Vector };
        }
    }

    if (interface.enumerations.contains(type.name()))
        return { .name = type.name(), .sequence_storage_type = SequenceStorageType::Vector };

    dbgln("Unimplemented type for idl_type_name_to_cpp_type: {}{}", type.name(), type.is_nullable() ? "?" : "");
    TODO();
}

static ByteString make_input_acceptable_cpp(ByteString const& input)
{
    if (input.is_one_of("class", "template", "for", "default", "char", "namespace", "delete", "inline")) {
        StringBuilder builder;
        builder.append(input);
        builder.append('_');
        return builder.to_byte_string();
    }

    return input.replace("-"sv, "_"sv, ReplaceMode::All);
}

static void generate_include_for_iterator(auto& generator, auto& iterator_path)
{
    auto iterator_generator = generator.fork();
    iterator_generator.set("iterator_class.path", iterator_path);
    // FIXME: These may or may not exist, because REASONS.
    iterator_generator.append(R"~~~(
//#if __has_include(<LibWeb/@iterator_class.path@.h>)
#   include <LibWeb/@iterator_class.path@.h>
//#endif
)~~~");
}

static void generate_include_for(auto& generator, auto& path)
{
    auto forked_generator = generator.fork();
    auto path_string = path;
    for (auto& search_path : g_header_search_paths) {
        if (!path.starts_with(search_path))
            continue;
        auto relative_path = LexicalPath::relative_path(path, search_path);
        if (relative_path.length() < path_string.length())
            path_string = relative_path;
    }

    LexicalPath include_path { path_string };
    forked_generator.set("include.path", ByteString::formatted("{}/{}.h", include_path.dirname(), include_path.title()));
    forked_generator.append(R"~~~(
#include <@include.path@>
)~~~");
}

static void emit_includes_for_all_imports(auto& interface, auto& generator, bool is_iterator = false)
{
    Queue<RemoveCVReference<decltype(interface)> const*> interfaces;
    HashTable<ByteString> paths_imported;

    interfaces.enqueue(&interface);

    while (!interfaces.is_empty()) {
        auto interface = interfaces.dequeue();
        if (paths_imported.contains(interface->module_own_path))
            continue;

        paths_imported.set(interface->module_own_path);
        for (auto& imported_interface : interface->imported_modules) {
            if (!paths_imported.contains(imported_interface.module_own_path))
                interfaces.enqueue(&imported_interface);
        }

        if (!interface->will_generate_code())
            continue;

        generate_include_for(generator, interface->module_own_path);
    }

    if (is_iterator) {
        auto iterator_path = ByteString::formatted("{}Iterator", interface.fully_qualified_name.replace("::"sv, "/"sv, ReplaceMode::All));
        generate_include_for_iterator(generator, iterator_path);
    }
}

template<typename ParameterType>
static void generate_to_string(SourceGenerator& scoped_generator, ParameterType const& parameter, bool variadic, bool optional, Optional<ByteString> const& optional_default_value)
{
    if (variadic) {
        scoped_generator.append(R"~~~(
    Vector<String> @cpp_name@;

    if (vm.argument_count() > @js_suffix@) {
        @cpp_name@.ensure_capacity(vm.argument_count() - @js_suffix@);

        for (size_t i = @js_suffix@; i < vm.argument_count(); ++i) {
            auto to_string_result = TRY(vm.argument(i).to_string(vm));
            @cpp_name@.unchecked_append(move(to_string_result));
        }
    }
)~~~");
    } else if (!optional) {
        if (!parameter.type->is_nullable()) {
            scoped_generator.append(R"~~~(
    @string_type@ @cpp_name@;
    if (!@legacy_null_to_empty_string@ || !@js_name@@js_suffix@.is_null()) {
        @cpp_name@ = TRY(@js_name@@js_suffix@.to_string(vm));
    }
)~~~");
        } else {
            scoped_generator.append(R"~~~(
    Optional<@string_type@> @cpp_name@;
    if (!@js_name@@js_suffix@.is_nullish())
        @cpp_name@ = TRY(@js_name@@js_suffix@.to_string(vm));
)~~~");
        }
    } else {
        bool may_be_null = !optional_default_value.has_value() || parameter.type->is_nullable() || optional_default_value.value() == "null";
        if (may_be_null) {
            scoped_generator.append(R"~~~(
    Optional<@string_type@> @cpp_name@;
)~~~");
        } else {
            scoped_generator.append(R"~~~(
    @string_type@ @cpp_name@;
)~~~");
        }

        scoped_generator.append(R"~~~(
    if (!@js_name@@js_suffix@.is_undefined()) {
        if (!@legacy_null_to_empty_string@ || !@js_name@@js_suffix@.is_null())
            @cpp_name@ = TRY(@js_name@@js_suffix@.to_string(vm));
    })~~~");
        if (!may_be_null) {
            scoped_generator.append(R"~~~( else {
        @cpp_name@ = MUST(@string_type@::from_utf8(@parameter.optional_default_value@sv));
    }
)~~~");
        } else {
            scoped_generator.append(R"~~~(
)~~~");
        }
    }
}

template<typename ParameterType>
static void generate_to_integral(SourceGenerator& scoped_generator, ParameterType const& parameter, bool optional, Optional<ByteString> const& optional_default_value)
{
    struct TypeMap {
        StringView idl_type;
        StringView cpp_type;
    };
    static constexpr auto idl_type_map = to_array<TypeMap>({
        { "boolean"sv, "bool"sv },
        { "byte"sv, "WebIDL::Byte"sv },
        { "octet"sv, "WebIDL::Octet"sv },
        { "short"sv, "WebIDL::Short"sv },
        { "unsigned short"sv, "WebIDL::UnsignedShort"sv },
        { "long"sv, "WebIDL::Long"sv },
        { "long long"sv, "WebIDL::LongLong"sv },
        { "unsigned long"sv, "WebIDL::UnsignedLong"sv },
        { "unsigned long long"sv, "WebIDL::UnsignedLongLong"sv },
    });

    auto it = find_if(idl_type_map.begin(), idl_type_map.end(), [&](auto const& entry) {
        return entry.idl_type == parameter.type->name();
    });

    VERIFY(it != idl_type_map.end());
    scoped_generator.set("cpp_type"sv, it->cpp_type);
    scoped_generator.set("enforce_range", parameter.extended_attributes.contains("EnforceRange") ? "Yes" : "No");
    scoped_generator.set("clamp", parameter.extended_attributes.contains("Clamp") ? "Yes" : "No");

    if ((!optional && !parameter.type->is_nullable()) || optional_default_value.has_value()) {
        scoped_generator.append(R"~~~(
    @cpp_type@ @cpp_name@;
)~~~");
    } else {
        scoped_generator.append(R"~~~(
    Optional<@cpp_type@> @cpp_name@;
)~~~");
    }

    if (parameter.type->is_nullable()) {
        scoped_generator.append(R"~~~(
    if (!@js_name@@js_suffix@.is_null() && !@js_name@@js_suffix@.is_undefined())
)~~~");
    } else if (optional) {
        scoped_generator.append(R"~~~(
    if (!@js_name@@js_suffix@.is_undefined())
)~~~");
    }

    if (it->cpp_type == "bool"sv) {
        scoped_generator.append(R"~~~(
    @cpp_name@ = @js_name@@js_suffix@.to_boolean();
)~~~");
    } else {
        scoped_generator.append(R"~~~(
    @cpp_name@ = TRY(WebIDL::convert_to_int<@cpp_type@>(vm, @js_name@@js_suffix@, WebIDL::EnforceRange::@enforce_range@, WebIDL::Clamp::@clamp@));
)~~~");
    }

    if (optional_default_value.has_value()) {
        scoped_generator.append(R"~~~(
    else
        @cpp_name@ = static_cast<@cpp_type@>(@parameter.optional_default_value@);
)~~~");
    }
}

template<typename ParameterType>
static void generate_to_cpp(SourceGenerator& generator, ParameterType& parameter, ByteString const& js_name, ByteString const& js_suffix, ByteString const& cpp_name, IDL::Interface const& interface, bool legacy_null_to_empty_string = false, bool optional = false, Optional<ByteString> optional_default_value = {}, bool variadic = false, size_t recursion_depth = 0, bool string_to_fly_string = false)
{
    auto scoped_generator = generator.fork();
    auto acceptable_cpp_name = make_input_acceptable_cpp(cpp_name);
    auto explicit_null = parameter.extended_attributes.contains("ExplicitNull");
    scoped_generator.set("cpp_name", acceptable_cpp_name);
    scoped_generator.set("js_name", js_name);
    scoped_generator.set("js_suffix", js_suffix);
    scoped_generator.set("legacy_null_to_empty_string", legacy_null_to_empty_string ? "true" : "false");
    scoped_generator.set("string_type", string_to_fly_string ? "FlyString" : "String");
    scoped_generator.set("parameter.type.name", parameter.type->name());
    scoped_generator.set("parameter.name", parameter.name);

    if (explicit_null) {
        if (!IDL::is_platform_object(*parameter.type)) {
            dbgln("Parameter marked [ExplicitNull] in interface {} must be a platform object", interface.name);
            VERIFY_NOT_REACHED();
        }

        if (!optional || !parameter.type->is_nullable()) {
            dbgln("Parameter marked [ExplicitNull] in interface {} must be an optional and nullable type", interface.name);
            VERIFY_NOT_REACHED();
        }
    }

    if (optional_default_value.has_value())
        scoped_generator.set("parameter.optional_default_value", *optional_default_value);

    // FIXME: Add support for optional, variadic, nullable and default values to all types
    if (parameter.type->is_string()) {
        generate_to_string(scoped_generator, parameter, variadic, optional, optional_default_value);
    } else if (parameter.type->is_boolean() || parameter.type->is_integer()) {
        generate_to_integral(scoped_generator, parameter, optional, optional_default_value);
    } else if (parameter.type->name().is_one_of("EventListener", "NodeFilter")) {
        // FIXME: Replace this with support for callback interfaces. https://webidl.spec.whatwg.org/#idl-callback-interface

        if (parameter.type->name() == "EventListener")
            scoped_generator.set("cpp_type", "IDLEventListener");
        else
            scoped_generator.set("cpp_type", parameter.type->name());

        if (parameter.type->is_nullable()) {
            scoped_generator.append(R"~~~(
    @cpp_type@* @cpp_name@ = nullptr;
    if (!@js_name@@js_suffix@.is_nullish()) {
        if (!@js_name@@js_suffix@.is_object())
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObject, @js_name@@js_suffix@.to_string_without_side_effects());

        auto callback_type = vm.heap().allocate_without_realm<WebIDL::CallbackType>(@js_name@@js_suffix@.as_object(), HTML::incumbent_settings_object());
        @cpp_name@ = TRY(throw_dom_exception_if_needed(vm, [&] { return @cpp_type@::create(realm, *callback_type); }));
    }
)~~~");
        } else {
            scoped_generator.append(R"~~~(
    if (!@js_name@@js_suffix@.is_object())
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObject, @js_name@@js_suffix@.to_string_without_side_effects());

    auto callback_type = vm.heap().allocate_without_realm<WebIDL::CallbackType>(@js_name@@js_suffix@.as_object(), HTML::incumbent_settings_object());
    auto @cpp_name@ = adopt_ref(*new @cpp_type@(callback_type));
)~~~");
        }
    } else if (IDL::is_platform_object(*parameter.type)) {
        if (!parameter.type->is_nullable()) {
            if (!optional) {
                scoped_generator.append(R"~~~(
    if (!@js_name@@js_suffix@.is_object() || !is<@parameter.type.name@>(@js_name@@js_suffix@.as_object()))
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "@parameter.type.name@");

    auto& @cpp_name@ = static_cast<@parameter.type.name@&>(@js_name@@js_suffix@.as_object());
)~~~");
            } else {
                scoped_generator.append(R"~~~(
    JS::GCPtr<@parameter.type.name@> @cpp_name@;
    if (!@js_name@@js_suffix@.is_undefined()) {
        if (!@js_name@@js_suffix@.is_object() || !is<@parameter.type.name@>(@js_name@@js_suffix@.as_object()))
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "@parameter.type.name@");

        @cpp_name@ = static_cast<@parameter.type.name@&>(@js_name@@js_suffix@.as_object());
    }
)~~~");
            }
        } else {
            if (explicit_null) {
                scoped_generator.append(R"~~~(
    Optional<JS::GCPtr<@parameter.type.name@>> @cpp_name@;
    if (maybe_@js_name@@js_suffix@.has_value()) {
        auto @js_name@@js_suffix@ = maybe_@js_name@@js_suffix@.release_value();
)~~~");
            } else {
                scoped_generator.append(R"~~~(
    JS::GCPtr<@parameter.type.name@> @cpp_name@;
)~~~");
            }

            scoped_generator.append(R"~~~(
    if (!@js_name@@js_suffix@.is_nullish()) {
        if (!@js_name@@js_suffix@.is_object() || !is<@parameter.type.name@>(@js_name@@js_suffix@.as_object()))
            return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "@parameter.type.name@");

        @cpp_name@ = &static_cast<@parameter.type.name@&>(@js_name@@js_suffix@.as_object());
    }
)~~~");

            if (explicit_null) {
                scoped_generator.append(R"~~~(
    }
)~~~");
            }
        }
    } else if (parameter.type->is_floating_point()) {
        if (parameter.type->name() == "unrestricted float") {
            scoped_generator.set("parameter.type.name", "float");
        } else if (parameter.type->name() == "unrestricted double") {
            scoped_generator.set("parameter.type.name", "double");
        }

        bool is_wrapped_in_optional_type = false;
        if (!optional) {
            scoped_generator.append(R"~~~(
    @parameter.type.name@ @cpp_name@ = TRY(@js_name@@js_suffix@.to_double(vm));
)~~~");
        } else {
            if (optional_default_value.has_value() && optional_default_value != "null"sv) {
                scoped_generator.append(R"~~~(
    @parameter.type.name@ @cpp_name@;
)~~~");
            } else {
                is_wrapped_in_optional_type = true;
                scoped_generator.append(R"~~~(
    Optional<@parameter.type.name@> @cpp_name@;
)~~~");
            }
            scoped_generator.append(R"~~~(
    if (!@js_name@@js_suffix@.is_undefined())
        @cpp_name@ = TRY(@js_name@@js_suffix@.to_double(vm));
)~~~");
            if (optional_default_value.has_value() && optional_default_value.value() != "null"sv) {
                scoped_generator.append(R"~~~(
    else
        @cpp_name@ = @parameter.optional_default_value@;
)~~~");
            } else {
                scoped_generator.append(R"~~~(
)~~~");
            }
        }

        if (parameter.type->is_restricted_floating_point()) {
            if (is_wrapped_in_optional_type) {
                scoped_generator.append(R"~~~(
    if (@cpp_name@.has_value() && (isinf(*@cpp_name@) || isnan(*@cpp_name@))) {
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::InvalidRestrictedFloatingPointParameter, "@parameter.name@");
    }
    )~~~");
            } else {
                scoped_generator.append(R"~~~(
    if (isinf(@cpp_name@) || isnan(@cpp_name@)) {
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::InvalidRestrictedFloatingPointParameter, "@parameter.name@");
    }
    )~~~");
            }
        }
    } else if (parameter.type->name() == "Promise") {
        // NOTE: It's not clear to me where the implicit wrapping of non-Promise values in a resolved
        // Promise is defined in the spec; https://webidl.spec.whatwg.org/#idl-promise doesn't say
        // anything of this sort. Both Gecko and Blink do it, however, so I'm sure it's correct.
        scoped_generator.append(R"~~~(
    if (!@js_name@@js_suffix@.is_object() || !is<JS::Promise>(@js_name@@js_suffix@.as_object())) {
        auto new_promise = JS::Promise::create(realm);
        new_promise->fulfill(@js_name@@js_suffix@);
        @js_name@@js_suffix@ = new_promise;
    }
    auto @cpp_name@ = JS::make_handle(&static_cast<JS::Promise&>(@js_name@@js_suffix@.as_object()));
)~~~");
    } else if (parameter.type->name() == "object") {
        if (parameter.type->is_nullable()) {
            scoped_generator.append(R"~~~(
    Optional<JS::Handle<JS::Object>> @cpp_name@;
    if (!@js_name@@js_suffix@.is_null() && !@js_name@@js_suffix@.is_undefined())
        @cpp_name@ = JS::make_handle(TRY(@js_name@@js_suffix@.to_object(vm)));
)~~~");
        } else if (optional) {
            scoped_generator.append(R"~~~(
    Optional<JS::Handle<JS::Object>> @cpp_name@;
    if (!@js_name@@js_suffix@.is_undefined())
        @cpp_name@ = JS::make_handle(TRY(@js_name@@js_suffix@.to_object(vm)));
)~~~");
        } else {
            scoped_generator.append(R"~~~(
    auto @cpp_name@ = JS::make_handle(TRY(@js_name@@js_suffix@.to_object(vm)));
)~~~");
        }
    } else if (parameter.type->name().is_one_of("BufferSource", "Float32Array", "Float64Array", "Uint8Array", "Uint8ClampedArray")) {
        if (optional) {
            scoped_generator.append(R"~~~(
    Optional<JS::Handle<WebIDL::BufferSource>> @cpp_name@;
    if (!@js_name@@js_suffix@.is_undefined()) {
)~~~");
        } else {
            scoped_generator.append(R"~~~(
    JS::Handle<WebIDL::BufferSource> @cpp_name@;
)~~~");
        }
        scoped_generator.append(R"~~~(
    if (!@js_name@@js_suffix@.is_object() || !(is<JS::TypedArrayBase>(@js_name@@js_suffix@.as_object()) || is<JS::ArrayBuffer>(@js_name@@js_suffix@.as_object()) || is<JS::DataView>(@js_name@@js_suffix@.as_object())))
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "@parameter.type.name@");

    @cpp_name@ = JS::make_handle(vm.heap().allocate<WebIDL::BufferSource>(realm, @js_name@@js_suffix@.as_object()));
)~~~");

        if (optional) {
            scoped_generator.append(R"~~~(
        }
)~~~");
        }

    } else if (parameter.type->name() == "ArrayBufferView") {
        scoped_generator.append(R"~~~(
    if (!@js_name@@js_suffix@.is_object() || !(is<JS::TypedArrayBase>(@js_name@@js_suffix@.as_object()) || is<JS::DataView>(@js_name@@js_suffix@.as_object())))
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "@parameter.type.name@");

    auto @cpp_name@ = JS::make_handle(vm.heap().allocate<WebIDL::ArrayBufferView>(realm, @js_name@@js_suffix@.as_object()));
)~~~");
        if (optional) {
            scoped_generator.append(R"~~~(
        }
)~~~");
        }
    } else if (parameter.type->name() == "any") {
        if (variadic) {
            scoped_generator.append(R"~~~(
    JS::MarkedVector<JS::Value> @cpp_name@ { vm.heap() };

    if (vm.argument_count() > @js_suffix@) {
        @cpp_name@.ensure_capacity(vm.argument_count() - @js_suffix@);

        for (size_t i = @js_suffix@; i < vm.argument_count(); ++i)
            @cpp_name@.unchecked_append(vm.argument(i));
    }
)~~~");
        } else if (!optional) {
            scoped_generator.append(R"~~~(
    auto @cpp_name@ = @js_name@@js_suffix@;
)~~~");
        } else {
            scoped_generator.append(R"~~~(
    JS::Value @cpp_name@ = JS::js_undefined();
    if (!@js_name@@js_suffix@.is_undefined())
        @cpp_name@ = @js_name@@js_suffix@;
)~~~");
            if (optional_default_value.has_value()) {
                if (optional_default_value == "null") {
                    scoped_generator.append(R"~~~(
    else
        @cpp_name@ = JS::js_null();
)~~~");
                } else if (optional_default_value->to_number<int>().has_value() || optional_default_value->to_number<unsigned>().has_value()) {
                    scoped_generator.append(R"~~~(
    else
        @cpp_name@ = JS::Value(@parameter.optional_default_value@);
)~~~");
                } else {
                    TODO();
                }
            }
        }
    } else if (interface.enumerations.contains(parameter.type->name())) {
        auto enum_generator = scoped_generator.fork();
        auto& enumeration = interface.enumerations.find(parameter.type->name())->value;
        StringView enum_member_name;
        if (optional_default_value.has_value()) {
            VERIFY(optional_default_value->length() >= 2 && (*optional_default_value)[0] == '"' && (*optional_default_value)[optional_default_value->length() - 1] == '"');
            enum_member_name = optional_default_value->substring_view(1, optional_default_value->length() - 2);
        } else {
            enum_member_name = enumeration.first_member;
        }
        auto default_value_cpp_name = enumeration.translated_cpp_names.get(enum_member_name);
        VERIFY(default_value_cpp_name.has_value());
        enum_generator.set("enum.default.cpp_value", *default_value_cpp_name);
        enum_generator.set("js_name.as_string", ByteString::formatted("{}{}_string", enum_generator.get("js_name"sv), enum_generator.get("js_suffix"sv)));
        enum_generator.append(R"~~~(
    @parameter.type.name@ @cpp_name@ { @parameter.type.name@::@enum.default.cpp_value@ };
)~~~");

        if (optional) {
            enum_generator.append(R"~~~(
    if (!@js_name@@js_suffix@.is_undefined()) {
)~~~");
        }

        enum_generator.append(R"~~~(
    auto @js_name.as_string@ = TRY(@js_name@@js_suffix@.to_string(vm));
)~~~");
        auto first = true;
        VERIFY(enumeration.translated_cpp_names.size() >= 1);
        for (auto& it : enumeration.translated_cpp_names) {
            enum_generator.set("enum.alt.name", it.key);
            enum_generator.set("enum.alt.value", it.value);
            enum_generator.set("else", first ? "" : "else ");
            first = false;

            enum_generator.append(R"~~~(
    @else@if (@js_name.as_string@ == "@enum.alt.name@"sv)
        @cpp_name@ = @parameter.type.name@::@enum.alt.value@;
)~~~");
        }

        // NOTE: Attribute setters return undefined instead of throwing when the string doesn't match an enum value.
        if constexpr (!IsSame<Attribute, RemoveConst<ParameterType>>) {
            enum_generator.append(R"~~~(
    else
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::InvalidEnumerationValue, @js_name.as_string@, "@parameter.type.name@");
)~~~");
        } else {
            enum_generator.append(R"~~~(
    else
        return JS::js_undefined();
)~~~");
        }

        if (optional) {
            enum_generator.append(R"~~~(
    }
)~~~");
        }
    } else if (interface.dictionaries.contains(parameter.type->name())) {
        if (optional_default_value.has_value() && optional_default_value != "{}")
            TODO();
        auto dictionary_generator = scoped_generator.fork();
        dictionary_generator.append(R"~~~(
    if (!@js_name@@js_suffix@.is_nullish() && !@js_name@@js_suffix@.is_object())
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "@parameter.type.name@");

    @parameter.type.name@ @cpp_name@ {};
)~~~");
        auto* current_dictionary = &interface.dictionaries.find(parameter.type->name())->value;
        // FIXME: This (i) is a hack to make sure we don't generate duplicate variable names.
        static auto i = 0;
        while (true) {
            for (auto& member : current_dictionary->members) {
                dictionary_generator.set("member_key", member.name);
                auto member_js_name = make_input_acceptable_cpp(member.name.to_snakecase());
                auto member_value_name = ByteString::formatted("{}_value_{}", member_js_name, i);
                auto member_property_value_name = ByteString::formatted("{}_property_value_{}", member_js_name, i);
                dictionary_generator.set("member_name", member_js_name);
                dictionary_generator.set("member_value_name", member_value_name);
                dictionary_generator.set("member_property_value_name", member_property_value_name);
                dictionary_generator.append(R"~~~(
    auto @member_property_value_name@ = JS::js_undefined();
    if (@js_name@@js_suffix@.is_object())
        @member_property_value_name@ = TRY(@js_name@@js_suffix@.as_object().get("@member_key@"));
)~~~");
                if (member.required) {
                    dictionary_generator.append(R"~~~(
    if (@member_property_value_name@.is_undefined())
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::MissingRequiredProperty, "@member_key@");
)~~~");
                } else if (!member.default_value.has_value()) {
                    // Assume struct member is Optional<T> and _don't_ assign the generated default
                    // value (e.g. first enum member) when the dictionary member is optional (i.e.
                    // no `required` and doesn't have a default value).
                    // This is needed so that "dictionary has member" checks work as expected.
                    dictionary_generator.append(R"~~~(
    if (!@member_property_value_name@.is_undefined()) {
)~~~");
                }

                generate_to_cpp(dictionary_generator, member, member_property_value_name, "", member_value_name, interface, member.extended_attributes.contains("LegacyNullToEmptyString"), !member.required, member.default_value);

                bool may_be_null = !optional_default_value.has_value() || parameter.type->is_nullable() || optional_default_value.value() == "null";

                if (member.type->is_string() && optional && may_be_null) {
                    dictionary_generator.append(R"~~~(
    if (@member_value_name@.has_value())
        @cpp_name@.@member_name@ = @member_value_name@.release_value();
)~~~");
                } else {
                    dictionary_generator.append(R"~~~(
    @cpp_name@.@member_name@ = @member_value_name@;
)~~~");
                }
                if (!member.required && !member.default_value.has_value()) {
                    dictionary_generator.append(R"~~~(
    }
)~~~");
                }
                i++;
            }
            if (current_dictionary->parent_name.is_empty())
                break;
            VERIFY(interface.dictionaries.contains(current_dictionary->parent_name));
            current_dictionary = &interface.dictionaries.find(current_dictionary->parent_name)->value;
        }
    } else if (interface.callback_functions.contains(parameter.type->name())) {
        // https://webidl.spec.whatwg.org/#es-callback-function

        auto callback_function_generator = scoped_generator.fork();
        auto& callback_function = interface.callback_functions.find(parameter.type->name())->value;

        if (callback_function.return_type->is_object() && callback_function.return_type->name() == "Promise")
            callback_function_generator.set("operation_returns_promise", "WebIDL::OperationReturnsPromise::Yes");
        else
            callback_function_generator.set("operation_returns_promise", "WebIDL::OperationReturnsPromise::No");

        // An ECMAScript value V is converted to an IDL callback function type value by running the following algorithm:
        // 1. If the result of calling IsCallable(V) is false and the conversion to an IDL value is not being performed due to V being assigned to an attribute whose type is a nullable callback function that is annotated with [LegacyTreatNonObjectAsNull], then throw a TypeError.
        if (!callback_function.is_legacy_treat_non_object_as_null) {
            callback_function_generator.append(R"~~~(
    if (!@js_name@@js_suffix@.is_function())
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAFunction, @js_name@@js_suffix@.to_string_without_side_effects());
)~~~");
        }
        // 2. Return the IDL callback function type value that represents a reference to the same object that V represents, with the incumbent settings object as the callback context.
        if (callback_function.is_legacy_treat_non_object_as_null) {
            callback_function_generator.append(R"~~~(
    WebIDL::CallbackType* @cpp_name@ = nullptr;
    if (@js_name@@js_suffix@.is_object())
        @cpp_name@ = vm.heap().allocate_without_realm<WebIDL::CallbackType>(@js_name@@js_suffix@.as_object(), HTML::incumbent_settings_object(), @operation_returns_promise@);
)~~~");
        } else {
            callback_function_generator.append(R"~~~(
    auto @cpp_name@ = vm.heap().allocate_without_realm<WebIDL::CallbackType>(@js_name@@js_suffix@.as_object(), HTML::incumbent_settings_object(), @operation_returns_promise@);
)~~~");
        }
    } else if (parameter.type->name() == "sequence") {
        // https://webidl.spec.whatwg.org/#es-sequence

        auto sequence_generator = scoped_generator.fork();
        auto& parameterized_type = verify_cast<IDL::ParameterizedType>(*parameter.type);
        sequence_generator.set("recursion_depth", ByteString::number(recursion_depth));

        // An ECMAScript value V is converted to an IDL sequence<T> value as follows:
        // 1. If Type(V) is not Object, throw a TypeError.
        // 2. Let method be ? GetMethod(V, @@iterator).
        // 3. If method is undefined, throw a TypeError.
        // 4. Return the result of creating a sequence from V and method.

        if (optional) {
            auto sequence_cpp_type = idl_type_name_to_cpp_type(parameterized_type.parameters().first(), interface);
            sequence_generator.set("sequence.type", sequence_cpp_type.name);
            sequence_generator.set("sequence.storage_type", sequence_storage_type_to_cpp_storage_type_name(sequence_cpp_type.sequence_storage_type));

            if (!optional_default_value.has_value()) {
                if (sequence_cpp_type.sequence_storage_type == IDL::SequenceStorageType::Vector) {
                    sequence_generator.append(R"~~~(
    Optional<@sequence.storage_type@<@sequence.type@>> @cpp_name@;
)~~~");
                } else {
                    sequence_generator.append(R"~~~(
    Optional<@sequence.storage_type@> @cpp_name@;
)~~~");
                }
            } else {
                if (optional_default_value != "[]")
                    TODO();

                if (sequence_cpp_type.sequence_storage_type == IDL::SequenceStorageType::Vector) {
                    sequence_generator.append(R"~~~(
    @sequence.storage_type@<@sequence.type@> @cpp_name@;
)~~~");
                } else {
                    sequence_generator.append(R"~~~(
    @sequence.storage_type@<@sequence.type@> @cpp_name@ { vm.heap() };
)~~~");
                }
            }

            sequence_generator.append(R"~~~(
    if (!@js_name@@js_suffix@.is_undefined()) {
)~~~");
        }

        sequence_generator.append(R"~~~(
    if (!@js_name@@js_suffix@.is_object())
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObject, @js_name@@js_suffix@.to_string_without_side_effects());

    auto iterator_method@recursion_depth@ = TRY(@js_name@@js_suffix@.get_method(vm, vm.well_known_symbol_iterator()));
    if (!iterator_method@recursion_depth@)
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotIterable, @js_name@@js_suffix@.to_string_without_side_effects());
)~~~");

        parameterized_type.generate_sequence_from_iterable(sequence_generator, ByteString::formatted("{}{}", acceptable_cpp_name, optional ? "_non_optional" : ""), ByteString::formatted("{}{}", js_name, js_suffix), ByteString::formatted("iterator_method{}", recursion_depth), interface, recursion_depth + 1);

        if (optional) {
            sequence_generator.append(R"~~~(
        @cpp_name@ = move(@cpp_name@_non_optional);
    }
)~~~");
        }
    } else if (parameter.type->name() == "record") {
        // https://webidl.spec.whatwg.org/#es-record

        auto record_generator = scoped_generator.fork();
        auto& parameterized_type = verify_cast<IDL::ParameterizedType>(*parameter.type);
        record_generator.set("recursion_depth", ByteString::number(recursion_depth));

        // A record can only have two types: key type and value type.
        VERIFY(parameterized_type.parameters().size() == 2);

        // A record only allows the key to be a string.
        VERIFY(parameterized_type.parameters()[0]->is_string());

        // An ECMAScript value O is converted to an IDL record<K, V> value as follows:
        // 1. If Type(O) is not Object, throw a TypeError.
        // 2. Let result be a new empty instance of record<K, V>.
        // 3. Let keys be ? O.[[OwnPropertyKeys]]().
        // 4. For each key of keys:
        //    1. Let desc be ? O.[[GetOwnProperty]](key).
        //    2. If desc is not undefined and desc.[[Enumerable]] is true:
        //       1. Let typedKey be key converted to an IDL value of type K.
        //       2. Let value be ? Get(O, key).
        //       3. Let typedValue be value converted to an IDL value of type V.
        //       4. Set result[typedKey] to typedValue.
        // 5. Return result.

        auto record_cpp_type = IDL::idl_type_name_to_cpp_type(parameterized_type, interface);
        record_generator.set("record.type", record_cpp_type.name);

        // If this is a recursive call to generate_to_cpp, assume that the caller has already handled converting the JS value to an object for us.
        // This affects record types in unions for example.
        if (recursion_depth == 0) {
            record_generator.append(R"~~~(
    if (!@js_name@@js_suffix@.is_object())
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObject, @js_name@@js_suffix@.to_string_without_side_effects());

    auto& @js_name@@js_suffix@_object = @js_name@@js_suffix@.as_object();
)~~~");
        }

        record_generator.append(R"~~~(
    @record.type@ @cpp_name@;

    auto record_keys@recursion_depth@ = TRY(@js_name@@js_suffix@_object.internal_own_property_keys());

    for (auto& key@recursion_depth@ : record_keys@recursion_depth@) {
        auto property_key@recursion_depth@ = MUST(JS::PropertyKey::from_value(vm, key@recursion_depth@));

        auto descriptor@recursion_depth@ = TRY(@js_name@@js_suffix@_object.internal_get_own_property(property_key@recursion_depth@));

        if (!descriptor@recursion_depth@.has_value() || !descriptor@recursion_depth@->enumerable.has_value() || !descriptor@recursion_depth@->enumerable.value())
            continue;
)~~~");

        IDL::Parameter key_parameter { .type = parameterized_type.parameters()[0], .name = acceptable_cpp_name, .optional_default_value = {}, .extended_attributes = {} };
        generate_to_cpp(record_generator, key_parameter, "key", ByteString::number(recursion_depth), ByteString::formatted("typed_key{}", recursion_depth), interface, false, false, {}, false, recursion_depth + 1);

        record_generator.append(R"~~~(
        auto value@recursion_depth@ = TRY(@js_name@@js_suffix@_object.get(property_key@recursion_depth@));
)~~~");

        // FIXME: Record value types should be TypeWithExtendedAttributes, which would allow us to get [LegacyNullToEmptyString] here.
        IDL::Parameter value_parameter { .type = parameterized_type.parameters()[1], .name = acceptable_cpp_name, .optional_default_value = {}, .extended_attributes = {} };
        generate_to_cpp(record_generator, value_parameter, "value", ByteString::number(recursion_depth), ByteString::formatted("typed_value{}", recursion_depth), interface, false, false, {}, false, recursion_depth + 1);

        record_generator.append(R"~~~(
        @cpp_name@.set(typed_key@recursion_depth@, typed_value@recursion_depth@);
    }
)~~~");
    } else if (is<IDL::UnionType>(*parameter.type)) {
        // https://webidl.spec.whatwg.org/#es-union

        auto union_generator = scoped_generator.fork();

        auto& union_type = verify_cast<IDL::UnionType>(*parameter.type);
        union_generator.set("union_type", union_type_to_variant(union_type, interface));
        union_generator.set("recursion_depth", ByteString::number(recursion_depth));

        // NOTE: This is handled out here as we need the dictionary conversion code for the {} optional default value.
        // 3. Let types be the flattened member types of the union type.
        auto types = union_type.flattened_member_types();

        RefPtr<Type const> dictionary_type;
        for (auto& dictionary : interface.dictionaries) {
            for (auto& type : types) {
                if (type->name() == dictionary.key) {
                    dictionary_type = type;
                    break;
                }
            }

            if (dictionary_type)
                break;
        }

        if (dictionary_type) {
            auto dictionary_generator = union_generator.fork();
            dictionary_generator.set("dictionary.type", dictionary_type->name());

            // The lambda must take the JS::Value to convert as a parameter instead of capturing it in order to support union types being variadic.
            dictionary_generator.append(R"~~~(
    auto @js_name@@js_suffix@_to_dictionary = [&vm, &realm](JS::Value @js_name@@js_suffix@) -> JS::ThrowCompletionOr<@dictionary.type@> {
        // This might be unused.
        (void)realm;
)~~~");

            IDL::Parameter dictionary_parameter { .type = *dictionary_type, .name = acceptable_cpp_name, .optional_default_value = {}, .extended_attributes = {} };
            generate_to_cpp(dictionary_generator, dictionary_parameter, js_name, js_suffix, "dictionary_union_type"sv, interface, false, false, {}, false, recursion_depth + 1);

            dictionary_generator.append(R"~~~(
        return dictionary_union_type;
    };
)~~~");
        }

        // A lambda is used because Variants without "Empty" can't easily be default initialized.
        // Plus, this would require the user of union types to always accept a Variant with an Empty type.

        // Additionally, it handles the case of unconditionally throwing a TypeError at the end if none of the types match.
        // This is because we cannot unconditionally throw in generate_to_cpp as generate_to_cpp is supposed to assign to a variable and then continue.
        // Note that all the other types only throw on a condition.

        // The lambda must take the JS::Value to convert as a parameter instead of capturing it in order to support union types being variadic.

        StringBuilder to_variant_captures;
        to_variant_captures.append("&vm, &realm"sv);

        if (dictionary_type)
            to_variant_captures.append(ByteString::formatted(", &{}{}_to_dictionary", js_name, js_suffix));

        union_generator.set("to_variant_captures", to_variant_captures.to_byte_string());

        union_generator.append(R"~~~(
    auto @js_name@@js_suffix@_to_variant = [@to_variant_captures@](JS::Value @js_name@@js_suffix@) -> JS::ThrowCompletionOr<@union_type@> {
        // These might be unused.
        (void)vm;
        (void)realm;
)~~~");

        // 1. If the union type includes undefined and V is undefined, then return the unique undefined value.
        if (union_type.includes_undefined()) {
            scoped_generator.append(R"~~~(
        if (@js_name@@js_suffix@.is_undefined())
            return Empty {};
)~~~");
        }

        // FIXME: 2. If the union type includes a nullable type and V is null or undefined, then return the IDL value null.
        if (union_type.includes_nullable_type()) {
            // Implement me
        } else if (dictionary_type) {
            // 4. If V is null or undefined, then
            //    4.1 If types includes a dictionary type, then return the result of converting V to that dictionary type.
            union_generator.append(R"~~~(
        if (@js_name@@js_suffix@.is_nullish())
            return @union_type@ { TRY(@js_name@@js_suffix@_to_dictionary(@js_name@@js_suffix@)) };
)~~~");
        }

        bool includes_object = false;
        for (auto& type : types) {
            if (type->name() == "object") {
                includes_object = true;
                break;
            }
        }

        // FIXME: Don't generate this if the union type doesn't include any object types.
        union_generator.append(R"~~~(
        if (@js_name@@js_suffix@.is_object()) {
            [[maybe_unused]] auto& @js_name@@js_suffix@_object = @js_name@@js_suffix@.as_object();
)~~~");

        bool includes_platform_object = false;
        for (auto& type : types) {
            if (IDL::is_platform_object(type)) {
                includes_platform_object = true;
                break;
            }
        }

        if (includes_platform_object) {
            // 5. If V is a platform object, then:
            union_generator.append(R"~~~(
            if (is<PlatformObject>(@js_name@@js_suffix@_object)) {
)~~~");

            // NOTE: This codegen assumes that all union types are cells or values we can create a handle for.

            //    1. If types includes an interface type that V implements, then return the IDL value that is a reference to the object V.
            for (auto& type : types) {
                if (!IDL::is_platform_object(type))
                    continue;

                auto union_platform_object_type_generator = union_generator.fork();
                union_platform_object_type_generator.set("platform_object_type", type->name());

                union_platform_object_type_generator.append(R"~~~(
                if (is<@platform_object_type@>(@js_name@@js_suffix@_object))
                    return JS::make_handle(static_cast<@platform_object_type@&>(@js_name@@js_suffix@_object));
)~~~");
            }

            //    2. If types includes object, then return the IDL value that is a reference to the object V.
            if (includes_object) {
                union_generator.append(R"~~~(
                return JS::make_handle(@js_name@@js_suffix@_object);
)~~~");
            }

            union_generator.append(R"~~~(
            }
)~~~");
        }

        bool includes_window_proxy = false;
        for (auto& type : types) {
            if (type->name() == "WindowProxy"sv) {
                includes_window_proxy = true;
                break;
            }
        }

        if (includes_window_proxy) {
            union_generator.append(R"~~~(
            if (is<WindowProxy>(@js_name@@js_suffix@_object))
                return JS::make_handle(static_cast<WindowProxy&>(@js_name@@js_suffix@_object));
)~~~");
        }

        // Note: This covers steps 6-8 for when Buffersource is in a union with a type other than "object".
        //       Since in that case, the return type would be Handle<BufferSource>, and not Handle<Object>.
        if (any_of(types, [](auto const& type) { return type->name() == "BufferSource"; }) && !includes_object) {
            union_generator.append(R"~~~(
            if (is<JS::ArrayBuffer>(@js_name@@js_suffix@_object) || is<JS::DataView>(@js_name@@js_suffix@_object) || is<JS::TypedArrayBase>(@js_name@@js_suffix@_object)) {
                JS::NonnullGCPtr<WebIDL::BufferSource> source_object = vm.heap().allocate<WebIDL::BufferSource>(realm, @js_name@@js_suffix@_object);
                return JS::make_handle(source_object);
            }
)~~~");
        }

        // 6. If Type(V) is Object and V has an [[ArrayBufferData]] internal slot, then
        //    1. If types includes ArrayBuffer, then return the result of converting V to ArrayBuffer.
        //    2. If types includes object, then return the IDL value that is a reference to the object V.
        if (any_of(types, [](auto const& type) { return type->name() == "ArrayBuffer"; }) || includes_object) {
            union_generator.append(R"~~~(
            if (is<JS::ArrayBuffer>(@js_name@@js_suffix@_object))
                return JS::make_handle(@js_name@@js_suffix@_object);
)~~~");
        }

        // 7. If Type(V) is Object and V has a [[DataView]] internal slot, then:
        //    1. If types includes DataView, then return the result of converting V to DataView.
        //    2. If types includes object, then return the IDL value that is a reference to the object V.
        if (any_of(types, [](auto const& type) { return type->name() == "DataView"; }) || includes_object) {
            union_generator.append(R"~~~(
            if (is<JS::DataView>(@js_name@@js_suffix@_object))
                return JS::make_handle(@js_name@@js_suffix@_object);
)~~~");
        }

        // 8. If Type(V) is Object and V has a [[TypedArrayName]] internal slot, then:
        //    1. If types includes a typed array type whose name is the value of V’s [[TypedArrayName]] internal slot, then return the result of converting V to that type.
        //    2. If types includes object, then return the IDL value that is a reference to the object V.
        auto has_typed_array_name = any_of(types, [](auto const& type) {
            return type->name().is_one_of("Int8Array"sv, "Int16Array"sv, "Int32Array"sv, "Uint8Array"sv, "Uint16Array"sv, "Uint32Array"sv, "Uint8ClampedArray"sv, "BigInt64Array"sv, "BigUint64Array", "Float32Array"sv, "Float64Array"sv);
        });

        if (has_typed_array_name || includes_object) {
            union_generator.append(R"~~~(
            if (is<JS::TypedArrayBase>(@js_name@@js_suffix@_object))
                return JS::make_handle(@js_name@@js_suffix@_object);
)~~~");
        }

        // 9. If IsCallable(V) is true, then:
        //     1. If types includes a callback function type, then return the result of converting V to that callback function type.
        //     2. If types includes object, then return the IDL value that is a reference to the object V.
        bool includes_callable = false;
        for (auto const& type : types) {
            if (type->name() == "Function"sv) {
                includes_callable = true;
                break;
            }
        }

        if (includes_callable) {
            union_generator.append(R"~~~(
            if (@js_name@@js_suffix@_object.is_function())
                return vm.heap().allocate_without_realm<WebIDL::CallbackType>(@js_name@@js_suffix@.as_function(), HTML::incumbent_settings_object());
)~~~");
        }

        // 10. If Type(V) is Object, then:
        //     1. If types includes a sequence type, then:
        RefPtr<IDL::ParameterizedType const> sequence_type;
        for (auto& type : types) {
            if (type->name() == "sequence") {
                sequence_type = verify_cast<IDL::ParameterizedType>(*type);
                break;
            }
        }

        if (sequence_type) {
            // 1. Let method be ? GetMethod(V, @@iterator).
            union_generator.append(R"~~~(
        auto method = TRY(@js_name@@js_suffix@.get_method(vm, vm.well_known_symbol_iterator()));
)~~~");

            // 2. If method is not undefined, return the result of creating a sequence of that type from V and method.
            union_generator.append(R"~~~(
        if (method) {
)~~~");

            sequence_type->generate_sequence_from_iterable(union_generator, acceptable_cpp_name, ByteString::formatted("{}{}", js_name, js_suffix), "method", interface, recursion_depth + 1);

            union_generator.append(R"~~~(

            return @cpp_name@;
        }
)~~~");
        }

        // FIXME: 2. If types includes a frozen array type, then
        //           1. Let method be ? GetMethod(V, @@iterator).
        //           2. If method is not undefined, return the result of creating a frozen array of that type from V and method.

        // 3. If types includes a dictionary type, then return the result of converting V to that dictionary type.
        if (dictionary_type) {
            union_generator.append(R"~~~(
        return @union_type@ { TRY(@js_name@@js_suffix@_to_dictionary(@js_name@@js_suffix@)) };
)~~~");
        }

        // 4. If types includes a record type, then return the result of converting V to that record type.
        RefPtr<IDL::ParameterizedType const> record_type;
        for (auto& type : types) {
            if (type->name() == "record") {
                record_type = verify_cast<IDL::ParameterizedType>(*type);
                break;
            }
        }

        if (record_type) {
            IDL::Parameter record_parameter { .type = *record_type, .name = acceptable_cpp_name, .optional_default_value = {}, .extended_attributes = {} };
            generate_to_cpp(union_generator, record_parameter, js_name, js_suffix, "record_union_type"sv, interface, false, false, {}, false, recursion_depth + 1);

            union_generator.append(R"~~~(
        return record_union_type;
)~~~");
        }

        // FIXME: 5. If types includes a callback interface type, then return the result of converting V to that callback interface type.

        // 6. If types includes object, then return the IDL value that is a reference to the object V.
        if (includes_object) {
            union_generator.append(R"~~~(
        return @js_name@@js_suffix@_object;
)~~~");
        }

        // End of is_object.
        union_generator.append(R"~~~(
        }
)~~~");

        // 11. If Type(V) is Boolean, then:
        //     1. If types includes boolean, then return the result of converting V to boolean.
        bool includes_boolean = false;
        for (auto& type : types) {
            if (type->name() == "boolean") {
                includes_boolean = true;
                break;
            }
        }

        if (includes_boolean) {
            union_generator.append(R"~~~(
        if (@js_name@@js_suffix@.is_boolean())
            return @union_type@ { @js_name@@js_suffix@.as_bool() };
)~~~");
        }

        RefPtr<IDL::Type const> numeric_type;
        for (auto& type : types) {
            if (type->is_numeric()) {
                numeric_type = type;
                break;
            }
        }

        // 12. If Type(V) is Number, then:
        //     1. If types includes a numeric type, then return the result of converting V to that numeric type.
        if (numeric_type) {
            union_generator.append(R"~~~(
        if (@js_name@@js_suffix@.is_number()) {
)~~~");
            // NOTE: generate_to_cpp doesn't use the parameter name.
            // NOTE: generate_to_cpp will use to_{u32,etc.} which uses to_number internally and will thus use TRY, but it cannot throw as we know we are dealing with a number.
            IDL::Parameter parameter { .type = *numeric_type, .name = ByteString::empty(), .optional_default_value = {}, .extended_attributes = {} };
            generate_to_cpp(union_generator, parameter, js_name, js_suffix, ByteString::formatted("{}{}_number", js_name, js_suffix), interface, false, false, {}, false, recursion_depth + 1);

            union_generator.append(R"~~~(
            return { @js_name@@js_suffix@_number };
        }
)~~~");
        }

        // 13. If Type(V) is BigInt, then:
        //     1. If types includes bigint, then return the result of converting V to bigint
        bool includes_bigint = false;
        for (auto& type : types) {
            if (type->name() == "bigint") {
                includes_bigint = true;
                break;
            }
        }

        if (includes_bigint) {
            union_generator.append(R"~~~(
        if (@js_name@@js_suffix@.is_bigint())
            return @js_name@@js_suffix@.as_bigint();
)~~~");
        }

        bool includes_string = false;
        for (auto& type : types) {
            if (type->is_string()) {
                includes_string = true;
                break;
            }
        }

        if (includes_string) {
            // 14. If types includes a string type, then return the result of converting V to that type.
            // NOTE: Currently all string types are converted to String.
            union_generator.append(R"~~~(
        return TRY(@js_name@@js_suffix@.to_string(vm));
)~~~");
        } else if (numeric_type && includes_bigint) {
            // 15. If types includes a numeric type and bigint, then return the result of converting V to either that numeric type or bigint.
            // https://webidl.spec.whatwg.org/#converted-to-a-numeric-type-or-bigint
            // NOTE: This algorithm is only used here.

            // An ECMAScript value V is converted to an IDL numeric type T or bigint value by running the following algorithm:
            // 1. Let x be ? ToNumeric(V).
            // 2. If Type(x) is BigInt, then
            //    1. Return the IDL bigint value that represents the same numeric value as x.
            // 3. Assert: Type(x) is Number.
            // 4. Return the result of converting x to T.

            auto union_numeric_type_generator = union_generator.fork();
            auto cpp_type = IDL::idl_type_name_to_cpp_type(*numeric_type, interface);
            union_numeric_type_generator.set("numeric_type", cpp_type.name);

            union_numeric_type_generator.append(R"~~~(
        auto x = TRY(@js_name@@js_suffix@.to_numeric(vm));
        if (x.is_bigint())
            return x.as_bigint();
        VERIFY(x.is_number());
)~~~");

            // NOTE: generate_to_cpp doesn't use the parameter name.
            // NOTE: generate_to_cpp will use to_{u32,etc.} which uses to_number internally and will thus use TRY, but it cannot throw as we know we are dealing with a number.
            IDL::Parameter parameter { .type = *numeric_type, .name = ByteString::empty(), .optional_default_value = {}, .extended_attributes = {} };
            generate_to_cpp(union_numeric_type_generator, parameter, "x", ByteString::empty(), "x_number", interface, false, false, {}, false, recursion_depth + 1);

            union_numeric_type_generator.append(R"~~~(
        return x_number;
)~~~");
        } else if (numeric_type) {
            // 16. If types includes a numeric type, then return the result of converting V to that numeric type.

            // NOTE: generate_to_cpp doesn't use the parameter name.
            // NOTE: generate_to_cpp will use to_{u32,etc.} which uses to_number internally and will thus use TRY, but it cannot throw as we know we are dealing with a number.
            IDL::Parameter parameter { .type = *numeric_type, .name = ByteString::empty(), .optional_default_value = {}, .extended_attributes = {} };
            generate_to_cpp(union_generator, parameter, js_name, js_suffix, ByteString::formatted("{}{}_number", js_name, js_suffix), interface, false, false, {}, false, recursion_depth + 1);

            union_generator.append(R"~~~(
        return { @js_name@@js_suffix@_number };
)~~~");
        } else if (includes_boolean) {
            // 17. If types includes boolean, then return the result of converting V to boolean.
            union_generator.append(R"~~~(
        return @union_type@ { @js_name@@js_suffix@.to_boolean() };
)~~~");
        } else if (includes_bigint) {
            // 18. If types includes bigint, then return the result of converting V to bigint.
            union_generator.append(R"~~~(
        return TRY(@js_name@@js_suffix@.to_bigint(vm));
)~~~");
        } else {
            // 19. Throw a TypeError.
            // FIXME: Replace the error message with something more descriptive.
            union_generator.append(R"~~~(
        return vm.throw_completion<JS::TypeError>("No union types matched"sv);
)~~~");
        }

        // Close the lambda and then perform the conversion.
        union_generator.append(R"~~~(
    };
)~~~");

        if (!variadic) {
            if (!optional) {
                union_generator.append(R"~~~(
    @union_type@ @cpp_name@ = TRY(@js_name@@js_suffix@_to_variant(@js_name@@js_suffix@));
)~~~");
            } else {
                if (!optional_default_value.has_value() || optional_default_value == "null"sv) {
                    union_generator.append(R"~~~(
    Optional<@union_type@> @cpp_name@;
    if (!@js_name@@js_suffix@.is_nullish())
        @cpp_name@ = TRY(@js_name@@js_suffix@_to_variant(@js_name@@js_suffix@));
)~~~");
                } else {
                    if (optional_default_value == "\"\"") {
                        union_generator.append(R"~~~(
    @union_type@ @cpp_name@ = @js_name@@js_suffix@.is_undefined() ? String {} : TRY(@js_name@@js_suffix@_to_variant(@js_name@@js_suffix@));
)~~~");
                    } else if (optional_default_value->starts_with("\""sv) && optional_default_value->ends_with("\""sv)) {
                        union_generator.set("default_string_value", optional_default_value.value());
                        union_generator.append(R"~~~(
    @union_type@ @cpp_name@ = @js_name@@js_suffix@.is_undefined() ? MUST(String::from_utf8(@default_string_value@sv)) : TRY(@js_name@@js_suffix@_to_variant(@js_name@@js_suffix@));
)~~~");
                    } else if (optional_default_value == "{}") {
                        VERIFY(dictionary_type);
                        union_generator.append(R"~~~(
    @union_type@ @cpp_name@ = @js_name@@js_suffix@.is_undefined() ? TRY(@js_name@@js_suffix@_to_dictionary(@js_name@@js_suffix@)) : TRY(@js_name@@js_suffix@_to_variant(@js_name@@js_suffix@));
)~~~");
                    } else if (optional_default_value->to_number<int>().has_value() || optional_default_value->to_number<unsigned>().has_value()) {
                        union_generator.append(R"~~~(
    @union_type@ @cpp_name@ = @js_name@@js_suffix@.is_undefined() ? @parameter.optional_default_value@ : TRY(@js_name@@js_suffix@_to_variant(@js_name@@js_suffix@));
)~~~");
                    } else {
                        TODO();
                    }
                }
            }
        } else {
            union_generator.append(R"~~~(
        Vector<@union_type@> @cpp_name@;

        if (vm.argument_count() > @js_suffix@) {
            @cpp_name@.ensure_capacity(vm.argument_count() - @js_suffix@);

            for (size_t i = @js_suffix@; i < vm.argument_count(); ++i) {
                auto result = TRY(@js_name@@js_suffix@_to_variant(vm.argument(i)));
                @cpp_name@.unchecked_append(move(result));
            }
        }
    )~~~");
        }
    } else {
        dbgln("Unimplemented JS-to-C++ conversion: {}", parameter.type->name());
        VERIFY_NOT_REACHED();
    }
}

static void generate_argument_count_check(SourceGenerator& generator, ByteString const& function_name, size_t argument_count)
{
    if (argument_count == 0)
        return;

    auto argument_count_check_generator = generator.fork();
    argument_count_check_generator.set("function.name", function_name);
    argument_count_check_generator.set("function.nargs", ByteString::number(argument_count));

    if (argument_count == 1) {
        argument_count_check_generator.set(".bad_arg_count", "JS::ErrorType::BadArgCountOne");
        argument_count_check_generator.set(".arg_count_suffix", "");
    } else {
        argument_count_check_generator.set(".bad_arg_count", "JS::ErrorType::BadArgCountMany");
        argument_count_check_generator.set(".arg_count_suffix", ByteString::formatted(", \"{}\"", argument_count));
    }

    argument_count_check_generator.append(R"~~~(
    if (vm.argument_count() < @function.nargs@)
        return vm.throw_completion<JS::TypeError>(@.bad_arg_count@, "@function.name@"@.arg_count_suffix@);
)~~~");
}

static void generate_arguments(SourceGenerator& generator, Vector<IDL::Parameter> const& parameters, StringBuilder& arguments_builder, IDL::Interface const& interface)
{
    auto arguments_generator = generator.fork();

    Vector<ByteString> parameter_names;
    size_t argument_index = 0;
    for (auto& parameter : parameters) {
        auto parameter_name = make_input_acceptable_cpp(parameter.name.to_snakecase());

        if (parameter.variadic) {
            // JS::MarkedVector is non-copyable, and the implementations likely want ownership of the
            // list, so we move() it into the parameter list.
            parameter_names.append(ByteString::formatted("move({})", parameter_name));
        } else {
            parameter_names.append(move(parameter_name));

            arguments_generator.set("argument.index", ByteString::number(argument_index));

            if (parameter.extended_attributes.contains("ExplicitNull")) {
                arguments_generator.set("argument.size", ByteString::number(argument_index + 1));
                arguments_generator.append(R"~~~(
    auto maybe_arg@argument.index@ = vm.argument_count() >= @argument.size@ ? Optional<JS::Value> { vm.argument(@argument.index@) } : OptionalNone {};
)~~~");
            } else {
                arguments_generator.append(R"~~~(
    auto arg@argument.index@ = vm.argument(@argument.index@);
)~~~");
            }
        }

        bool legacy_null_to_empty_string = parameter.extended_attributes.contains("LegacyNullToEmptyString");
        bool fly_string = parameter.extended_attributes.contains("FlyString");
        generate_to_cpp(generator, parameter, "arg", ByteString::number(argument_index), parameter.name.to_snakecase(), interface, legacy_null_to_empty_string, parameter.optional, parameter.optional_default_value, parameter.variadic, 0, fly_string);
        ++argument_index;
    }

    arguments_builder.join(", "sv, parameter_names);
}

// https://webidl.spec.whatwg.org/#create-sequence-from-iterable
void IDL::ParameterizedType::generate_sequence_from_iterable(SourceGenerator& generator, ByteString const& cpp_name, ByteString const& iterable_cpp_name, ByteString const& iterator_method_cpp_name, IDL::Interface const& interface, size_t recursion_depth) const
{
    auto sequence_generator = generator.fork();
    sequence_generator.set("cpp_name", cpp_name);
    sequence_generator.set("iterable_cpp_name", iterable_cpp_name);
    sequence_generator.set("iterator_method_cpp_name", iterator_method_cpp_name);
    sequence_generator.set("recursion_depth", ByteString::number(recursion_depth));
    auto sequence_cpp_type = idl_type_name_to_cpp_type(parameters().first(), interface);
    sequence_generator.set("sequence.type", sequence_cpp_type.name);
    sequence_generator.set("sequence.storage_type", sequence_storage_type_to_cpp_storage_type_name(sequence_cpp_type.sequence_storage_type));

    // To create an IDL value of type sequence<T> given an iterable iterable and an iterator getter method, perform the following steps:
    // 1. Let iter be ? GetIterator(iterable, sync, method).
    // 2. Initialize i to be 0.
    // 3. Repeat
    //      1. Let next be ? IteratorStep(iter).
    //      2. If next is false, then return an IDL sequence value of type sequence<T> of length i, where the value of the element at index j is Sj.
    //      3. Let nextItem be ? IteratorValue(next).
    //      4. Initialize Si to the result of converting nextItem to an IDL value of type T.
    //      5. Set i to i + 1.

    // FIXME: The WebIDL spec is out of date - it should be using GetIteratorFromMethod.
    sequence_generator.append(R"~~~(
    auto iterator@recursion_depth@ = TRY(JS::get_iterator_from_method(vm, @iterable_cpp_name@, *@iterator_method_cpp_name@));
)~~~");

    if (sequence_cpp_type.sequence_storage_type == SequenceStorageType::Vector) {
        sequence_generator.append(R"~~~(
    @sequence.storage_type@<@sequence.type@> @cpp_name@;
)~~~");
    } else {
        sequence_generator.append(R"~~~(
    @sequence.storage_type@<@sequence.type@> @cpp_name@ { vm.heap() };
)~~~");
    }

    sequence_generator.append(R"~~~(
    for (;;) {
        auto next@recursion_depth@ = TRY(JS::iterator_step(vm, iterator@recursion_depth@));
        if (!next@recursion_depth@)
            break;

        auto next_item@recursion_depth@ = TRY(JS::iterator_value(vm, *next@recursion_depth@));
)~~~");

    // FIXME: Sequences types should be TypeWithExtendedAttributes, which would allow us to get [LegacyNullToEmptyString] here.
    IDL::Parameter parameter { .type = parameters().first(), .name = iterable_cpp_name, .optional_default_value = {}, .extended_attributes = {} };
    generate_to_cpp(sequence_generator, parameter, "next_item", ByteString::number(recursion_depth), ByteString::formatted("sequence_item{}", recursion_depth), interface, false, false, {}, false, recursion_depth);

    sequence_generator.append(R"~~~(
    @cpp_name@.append(sequence_item@recursion_depth@);
    }
)~~~");
}

enum class WrappingReference {
    No,
    Yes,
};

static void generate_wrap_statement(SourceGenerator& generator, ByteString const& value, IDL::Type const& type, IDL::Interface const& interface, StringView result_expression, WrappingReference wrapping_reference = WrappingReference::No, size_t recursion_depth = 0)
{
    auto scoped_generator = generator.fork();
    scoped_generator.set("value", value);
    if (!libweb_interface_namespaces.span().contains_slow(type.name())) {
        if (is_javascript_builtin(type))
            scoped_generator.set("type", ByteString::formatted("JS::{}", type.name()));
        else
            scoped_generator.set("type", type.name());
    } else {
        // e.g. Document.getSelection which returns Selection, which is in the Selection namespace.
        StringBuilder builder;
        builder.append(type.name());
        builder.append("::"sv);
        builder.append(type.name());
        scoped_generator.set("type", builder.to_byte_string());
    }
    scoped_generator.set("result_expression", result_expression);
    scoped_generator.set("recursion_depth", ByteString::number(recursion_depth));

    if (type.name() == "undefined") {
        scoped_generator.append(R"~~~(
    @result_expression@ JS::js_undefined();
)~~~");
        return;
    }

    if (type.is_nullable() && !is<UnionType>(type)) {
        if (type.is_string()) {
            scoped_generator.append(R"~~~(
    if (!@value@.has_value()) {
        @result_expression@ JS::js_null();
    } else {
)~~~");
        } else if (type.name() == "sequence") {
            scoped_generator.append(R"~~~(
    if (!@value@.has_value()) {
        @result_expression@ JS::js_null();
    } else {
)~~~");
        } else if (type.is_primitive() || interface.enumerations.contains(type.name())) {
            scoped_generator.append(R"~~~(
    if (!@value@.has_value()) {
        @result_expression@ JS::js_null();
    } else {
)~~~");
        } else {
            scoped_generator.append(R"~~~(
    if (!@value@) {
        @result_expression@ JS::js_null();
    } else {
)~~~");
        }
    }

    if (type.is_string()) {
        if (type.is_nullable()) {
            scoped_generator.append(R"~~~(
    @result_expression@ JS::PrimitiveString::create(vm, @value@.release_value());
)~~~");
        } else {
            scoped_generator.append(R"~~~(
    @result_expression@ JS::PrimitiveString::create(vm, @value@);
)~~~");
        }
    } else if (type.name() == "sequence") {
        // https://webidl.spec.whatwg.org/#es-sequence
        auto& sequence_generic_type = verify_cast<IDL::ParameterizedType>(type);

        scoped_generator.append(R"~~~(
    auto new_array@recursion_depth@ = MUST(JS::Array::create(realm, 0));
)~~~");

        if (!type.is_nullable()) {
            scoped_generator.append(R"~~~(
    for (size_t i@recursion_depth@ = 0; i@recursion_depth@ < @value@.size(); ++i@recursion_depth@) {
        auto& element@recursion_depth@ = @value@.at(i@recursion_depth@);
)~~~");
        } else {
            scoped_generator.append(R"~~~(
    auto& @value@_non_optional = @value@.value();
    for (size_t i@recursion_depth@ = 0; i@recursion_depth@ < @value@_non_optional.size(); ++i@recursion_depth@) {
        auto& element@recursion_depth@ = @value@_non_optional.at(i@recursion_depth@);
)~~~");
        }

        // If the type is a platform object we currently return a Vector<JS::Handle<T>> from the
        // C++ implementation, thus allowing us to unwrap the element (a handle) like below.
        // This might need to change if we switch to a MarkedVector.
        if (is_platform_object(sequence_generic_type.parameters().first())) {
            scoped_generator.append(R"~~~(
            auto* wrapped_element@recursion_depth@ = &(*element@recursion_depth@);
)~~~");
        } else {
            generate_wrap_statement(scoped_generator, ByteString::formatted("element{}", recursion_depth), sequence_generic_type.parameters().first(), interface, ByteString::formatted("auto wrapped_element{} =", recursion_depth), WrappingReference::Yes, recursion_depth + 1);
        }

        scoped_generator.append(R"~~~(
        auto property_index@recursion_depth@ = JS::PropertyKey { i@recursion_depth@ };
        MUST(new_array@recursion_depth@->create_data_property(property_index@recursion_depth@, wrapped_element@recursion_depth@));
    }

    @result_expression@ new_array@recursion_depth@;
)~~~");
    } else if (type.name() == "boolean" || type.is_floating_point()) {
        if (type.is_nullable()) {
            scoped_generator.append(R"~~~(
    @result_expression@ JS::Value(@value@.release_value());
)~~~");
        } else {
            scoped_generator.append(R"~~~(
    @result_expression@ JS::Value(@value@);
)~~~");
        }
    } else if (type.name() == "short" || type.name() == "long" || type.name() == "unsigned short") {
        scoped_generator.append(R"~~~(
    @result_expression@ JS::Value((i32)@value@);
)~~~");
    } else if (type.name() == "unsigned long") {
        scoped_generator.append(R"~~~(
    @result_expression@ JS::Value((u32)@value@);
)~~~");
    } else if (type.name() == "long long") {
        scoped_generator.append(R"~~~(
    @result_expression@ JS::Value((double)@value@);
)~~~");
    } else if (type.name() == "unsigned long long") {
        scoped_generator.append(R"~~~(
    @result_expression@ JS::Value((double)@value@);
)~~~");
    } else if (type.name() == "Location" || type.name() == "Promise" || type.name() == "Uint8Array" || type.name() == "Uint8ClampedArray" || type.name() == "any") {
        scoped_generator.append(R"~~~(
    @result_expression@ @value@;
)~~~");
    } else if (type.name() == "ArrayBufferView" || type.name() == "BufferSource") {
        scoped_generator.append(R"~~~(
    @result_expression@ JS::Value(@value@->raw_object());
)~~~");
    } else if (is<IDL::UnionType>(type)) {
        auto& union_type = verify_cast<IDL::UnionType>(type);
        auto union_types = union_type.flattened_member_types();
        auto union_generator = scoped_generator.fork();

        union_generator.append(R"~~~(
    @result_expression@ @value@.visit(
)~~~");

        for (size_t current_union_type_index = 0; current_union_type_index < union_types.size(); ++current_union_type_index) {
            auto& current_union_type = union_types.at(current_union_type_index);
            auto cpp_type = IDL::idl_type_name_to_cpp_type(current_union_type, interface);
            union_generator.set("current_type", cpp_type.name);
            union_generator.append(R"~~~(
        [&vm, &realm]([[maybe_unused]] @current_type@ const& visited_union_value@recursion_depth@) -> JS::Value {
            // These may be unused.
            (void)vm;
            (void)realm;
)~~~");

            // NOTE: While we are using const&, the underlying type for wrappable types in unions is (Nonnull)RefPtr, which are not references.
            generate_wrap_statement(union_generator, ByteString::formatted("visited_union_value{}", recursion_depth), current_union_type, interface, "return"sv, WrappingReference::No, recursion_depth + 1);

            // End of current visit lambda.
            // The last lambda cannot have a trailing comma on the closing brace, unless the type is nullable, where an extra lambda will be generated for the Empty case.
            if (current_union_type_index != union_types.size() - 1 || type.is_nullable()) {
                union_generator.append(R"~~~(
        },
)~~~");
            } else {
                union_generator.append(R"~~~(
        }
)~~~");
            }
        }

        if (type.is_nullable()) {
            union_generator.append(R"~~~(
        [](Empty) -> JS::Value {
            return JS::js_null();
        }
)~~~");
        }

        // End of visit.
        union_generator.append(R"~~~(
    );
)~~~");
    } else if (interface.enumerations.contains(type.name())) {
        // Handle Enum? values, which were null-checked above
        if (type.is_nullable())
            scoped_generator.set("value", ByteString::formatted("{}.value()", value));
        scoped_generator.append(R"~~~(
    @result_expression@ JS::PrimitiveString::create(vm, Bindings::idl_enum_to_string(@value@));
)~~~");
    } else if (interface.callback_functions.contains(type.name())) {
        // https://webidl.spec.whatwg.org/#es-callback-function

        auto& callback_function = interface.callback_functions.find(type.name())->value;

        // The result of converting an IDL callback function type value to an ECMAScript value is a reference to the same object that the IDL callback function type value represents.

        if (callback_function.is_legacy_treat_non_object_as_null && !type.is_nullable()) {
            scoped_generator.append(R"~~~(
  if (!@value@) {
      @result_expression@ JS::js_null();
  } else {
      @result_expression@ @value@->callback;
  }
)~~~");
        } else {
            scoped_generator.append(R"~~~(
  @result_expression@ @value@->callback;
)~~~");
        }
    } else if (interface.dictionaries.contains(type.name())) {
        // https://webidl.spec.whatwg.org/#es-dictionary
        auto dictionary_generator = scoped_generator.fork();

        dictionary_generator.append(R"~~~(
    auto dictionary_object@recursion_depth@ = JS::Object::create(realm, realm.intrinsics().object_prototype());
)~~~");

        auto* current_dictionary = &interface.dictionaries.find(type.name())->value;
        while (true) {
            for (auto& member : current_dictionary->members) {
                dictionary_generator.set("member_key", member.name);
                auto member_key_js_name = ByteString::formatted("{}{}", make_input_acceptable_cpp(member.name.to_snakecase()), recursion_depth);
                dictionary_generator.set("member_name", member_key_js_name);
                auto member_value_js_name = ByteString::formatted("{}_value", member_key_js_name);
                dictionary_generator.set("member_value", member_value_js_name);

                auto wrapped_value_name = ByteString::formatted("wrapped_{}", member_value_js_name);
                dictionary_generator.set("wrapped_value_name", wrapped_value_name);

                dictionary_generator.append(R"~~~(
    JS::Value @wrapped_value_name@;
)~~~");
                generate_wrap_statement(dictionary_generator, ByteString::formatted("{}.{}", value, member.name.to_snakecase()), member.type, interface, ByteString::formatted("{} =", wrapped_value_name), WrappingReference::No, recursion_depth + 1);

                dictionary_generator.append(R"~~~(
    MUST(dictionary_object@recursion_depth@->create_data_property("@member_key@", @wrapped_value_name@));
)~~~");
            }

            if (current_dictionary->parent_name.is_empty())
                break;
            VERIFY(interface.dictionaries.contains(current_dictionary->parent_name));
            current_dictionary = &interface.dictionaries.find(current_dictionary->parent_name)->value;
        }

        dictionary_generator.append(R"~~~(
    @result_expression@ dictionary_object@recursion_depth@;
)~~~");
    } else if (type.name() == "object") {
        scoped_generator.append(R"~~~(
    @result_expression@ JS::Value(const_cast<JS::Object*>(@value@));
)~~~");
    } else {
        if (wrapping_reference == WrappingReference::No) {
            scoped_generator.append(R"~~~(
    @result_expression@ &const_cast<@type@&>(*@value@);
)~~~");
        } else {
            scoped_generator.append(R"~~~(
    @result_expression@ &const_cast<@type@&>(@value@);
)~~~");
        }
    }

    if (type.is_nullable() && !is<UnionType>(type)) {
        scoped_generator.append(R"~~~(
    }
)~~~");
    }
}

enum class StaticFunction {
    No,
    Yes,
};

enum class IsConstructor {
    No,
    Yes
};

static void generate_return_statement(SourceGenerator& generator, IDL::Type const& return_type, IDL::Interface const& interface)
{
    return generate_wrap_statement(generator, "retval", return_type, interface, "return"sv);
}

static void generate_variable_statement(SourceGenerator& generator, ByteString const& variable_name, IDL::Type const& value_type, ByteString const& value_name, IDL::Interface const& interface)
{
    auto variable_generator = generator.fork();
    variable_generator.set("variable_name", variable_name);
    variable_generator.append(R"~~~(
    JS::Value @variable_name@;
)~~~");
    return generate_wrap_statement(generator, value_name, value_type, interface, ByteString::formatted("{} = ", variable_name));
}

static void generate_function(SourceGenerator& generator, IDL::Function const& function, StaticFunction is_static_function, ByteString const& class_name, ByteString const& interface_fully_qualified_name, IDL::Interface const& interface)
{
    auto function_generator = generator.fork();
    function_generator.set("class_name", class_name);
    function_generator.set("interface_fully_qualified_name", interface_fully_qualified_name);
    function_generator.set("function.name", function.name);
    function_generator.set("function.name:snakecase", make_input_acceptable_cpp(function.name.to_snakecase()));
    function_generator.set("overload_suffix", function.is_overloaded ? ByteString::number(function.overload_index) : ByteString::empty());

    if (function.extended_attributes.contains("ImplementedAs")) {
        auto implemented_as = function.extended_attributes.get("ImplementedAs").value();
        function_generator.set("function.cpp_name", implemented_as);
    } else {
        function_generator.set("function.cpp_name", make_input_acceptable_cpp(function.name.to_snakecase()));
    }

    function_generator.append(R"~~~(
JS_DEFINE_NATIVE_FUNCTION(@class_name@::@function.name:snakecase@@overload_suffix@)
{
    WebIDL::log_trace(vm, "@class_name@::@function.name:snakecase@@overload_suffix@");
    [[maybe_unused]] auto& realm = *vm.current_realm();
)~~~");

    if (is_static_function == StaticFunction::No) {
        function_generator.append(R"~~~(
    auto* impl = TRY(impl_from(vm));
)~~~");
    }

    // Optimization: overloaded functions' arguments count is checked by the overload arbiter
    if (!function.is_overloaded)
        generate_argument_count_check(generator, function.name, function.shortest_length());

    StringBuilder arguments_builder;
    generate_arguments(generator, function.parameters, arguments_builder, interface);
    function_generator.set(".arguments", arguments_builder.string_view());

    if (is_static_function == StaticFunction::No) {
        // For [CEReactions]: https://html.spec.whatwg.org/multipage/custom-elements.html#cereactions

        if (function.extended_attributes.contains("CEReactions")) {
            // 1. Push a new element queue onto this object's relevant agent's custom element reactions stack.
            function_generator.append(R"~~~(
    auto& relevant_agent = HTML::relevant_agent(*impl);
    auto* custom_data = verify_cast<Bindings::WebEngineCustomData>(relevant_agent.custom_data());
    auto& reactions_stack = custom_data->custom_element_reactions_stack;
    reactions_stack.element_queue_stack.append({});
)~~~");
        }

        if (!function.extended_attributes.contains("CEReactions")) {
            function_generator.append(R"~~~(
    [[maybe_unused]] auto retval = TRY(throw_dom_exception_if_needed(vm, [&] { return impl->@function.cpp_name@(@.arguments@); }));
)~~~");
        } else {
            // 2. Run the originally-specified steps for this construct, catching any exceptions. If the steps return a value, let value be the returned value. If they throw an exception, let exception be the thrown exception.
            // 3. Let queue be the result of popping from this object's relevant agent's custom element reactions stack.
            // 4. Invoke custom element reactions in queue.
            // 5. If an exception exception was thrown by the original steps, rethrow exception.
            // 6. If a value value was returned from the original steps, return value.
            function_generator.append(R"~~~(
    auto retval_or_exception = throw_dom_exception_if_needed(vm, [&] { return impl->@function.cpp_name@(@.arguments@); });

    auto queue = reactions_stack.element_queue_stack.take_last();
    Bindings::invoke_custom_element_reactions(queue);

    if (retval_or_exception.is_error())
        return retval_or_exception.release_error();

    [[maybe_unused]] auto retval = retval_or_exception.release_value();
)~~~");
        }
    } else {
        // Make sure first argument for static functions is the Realm.
        if (arguments_builder.is_empty())
            function_generator.set(".arguments", "vm");
        else
            function_generator.set(".arguments", ByteString::formatted("vm, {}", arguments_builder.string_view()));

        function_generator.append(R"~~~(
    [[maybe_unused]] auto retval = TRY(throw_dom_exception_if_needed(vm, [&] { return @interface_fully_qualified_name@::@function.cpp_name@(@.arguments@); }));
)~~~");
    }

    generate_return_statement(generator, *function.return_type, interface);

    function_generator.append(R"~~~(
}
)~~~");
}

// https://webidl.spec.whatwg.org/#compute-the-effective-overload-set
static Vector<EffectiveOverloadSet::Item> compute_the_effective_overload_set(auto const& overload_set)
{
    // 1. Let S be an ordered set.
    Vector<EffectiveOverloadSet::Item> overloads;

    // 2. Let F be an ordered set with items as follows, according to the kind of effective overload set:
    // Note: This is determined by the caller of generate_overload_arbiter()

    // 3. Let maxarg be the maximum number of arguments the operations, legacy factory functions, or
    //    callback functions in F are declared to take. For variadic operations and legacy factory functions,
    //    the argument on which the ellipsis appears counts as a single argument.
    auto overloaded_functions = overload_set.value;
    auto maximum_arguments = 0;
    for (auto const& function : overloaded_functions)
        maximum_arguments = max(maximum_arguments, static_cast<int>(function.parameters.size()));

    // 4. Let max be max(maxarg, N).
    // NOTE: We don't do this step. `N` is a runtime value, so we just use `maxarg` here instead.
    //       Later, `generate_overload_arbiter()` produces individual overload sets for each possible N.

    // 5. For each operation or extended attribute X in F:
    auto overload_id = 0;
    for (auto const& overload : overloaded_functions) {
        // 1. Let arguments be the list of arguments X is declared to take.
        auto const& arguments = overload.parameters;

        // 2. Let n be the size of arguments.
        int argument_count = (int)arguments.size();

        // 3. Let types be a type list.
        Vector<NonnullRefPtr<Type const>> types;

        // 4. Let optionalityValues be an optionality list.
        Vector<Optionality> optionality_values;

        bool overload_is_variadic = false;

        // 5. For each argument in arguments:
        for (auto const& argument : arguments) {
            // 1. Append the type of argument to types.
            types.append(argument.type);

            // 2. Append "variadic" to optionalityValues if argument is a final, variadic argument, "optional" if argument is optional, and "required" otherwise.
            if (argument.variadic) {
                optionality_values.append(Optionality::Variadic);
                overload_is_variadic = true;
            } else if (argument.optional) {
                optionality_values.append(Optionality::Optional);
            } else {
                optionality_values.append(Optionality::Required);
            }
        }

        // 6. Append the tuple (X, types, optionalityValues) to S.
        overloads.empend(overload_id, types, optionality_values);

        // 7. If X is declared to be variadic, then:
        if (overload_is_variadic) {
            // 1. For each i in the range n to max − 1, inclusive:
            for (auto i = argument_count; i < maximum_arguments; ++i) {
                // 1. Let t be a type list.
                // 2. Let o be an optionality list.
                // NOTE: We hold both of these in an Item instead.
                EffectiveOverloadSet::Item item;
                item.callable_id = overload_id;

                // 3. For each j in the range 0 to n − 1, inclusive:
                for (auto j = 0; j < argument_count; ++j) {
                    // 1. Append types[j] to t.
                    item.types.append(types[j]);

                    // 2. Append optionalityValues[j] to o.
                    item.optionality_values.append(optionality_values[j]);
                }

                // 4. For each j in the range n to i, inclusive:
                for (auto j = argument_count; j <= i; ++j) {
                    // 1. Append types[n − 1] to t.
                    item.types.append(types[argument_count - 1]);

                    // 2. Append "variadic" to o.
                    item.optionality_values.append(Optionality::Variadic);
                }

                // 5. Append the tuple (X, t, o) to S.
                overloads.append(move(item));
            }
        }

        // 8. Let i be n − 1.
        auto i = argument_count - 1;

        // 9. While i ≥ 0:
        while (i >= 0) {
            // 1. If arguments[i] is not optional (i.e., it is not marked as "optional" and is not a final, variadic argument), then break.
            if (!arguments[i].optional && !arguments[i].variadic)
                break;

            // 2. Let t be a type list.
            // 3. Let o be an optionality list.
            // NOTE: We hold both of these in an Item instead.
            EffectiveOverloadSet::Item item;
            item.callable_id = overload_id;

            // 4. For each j in the range 0 to i − 1, inclusive:
            for (auto j = 0; j < i; ++j) {
                // 1. Append types[j] to t.
                item.types.append(types[j]);

                // 2. Append optionalityValues[j] to o.
                item.optionality_values.append(optionality_values[j]);
            }

            // 5. Append the tuple (X, t, o) to S.
            overloads.append(move(item));

            // 6. Set i to i − 1.
            --i;
        }

        overload_id++;
    }

    return overloads;
}

static ByteString generate_constructor_for_idl_type(Type const& type)
{
    auto append_type_list = [](auto& builder, auto const& type_list) {
        bool first = true;
        for (auto const& child_type : type_list) {
            if (first) {
                first = false;
            } else {
                builder.append(", "sv);
            }

            builder.append(generate_constructor_for_idl_type(child_type));
        }
    };

    switch (type.kind()) {
    case Type::Kind::Plain:
        return ByteString::formatted("make_ref_counted<IDL::Type>(\"{}\", {})", type.name(), type.is_nullable());
    case Type::Kind::Parameterized: {
        auto const& parameterized_type = type.as_parameterized();
        StringBuilder builder;
        builder.appendff("make_ref_counted<IDL::ParameterizedType>(\"{}\", {}, Vector<NonnullRefPtr<IDL::Type const>> {{", type.name(), type.is_nullable());
        append_type_list(builder, parameterized_type.parameters());
        builder.append("})"sv);
        return builder.to_byte_string();
    }
    case Type::Kind::Union: {
        auto const& union_type = type.as_union();
        StringBuilder builder;
        builder.appendff("make_ref_counted<IDL::UnionType>(\"{}\", {}, Vector<NonnullRefPtr<IDL::Type const>> {{", type.name(), type.is_nullable());
        append_type_list(builder, union_type.member_types());
        builder.append("})"sv);
        return builder.to_byte_string();
    }
    }

    VERIFY_NOT_REACHED();
}

// https://webidl.spec.whatwg.org/#dfn-distinguishing-argument-index
static size_t resolve_distinguishing_argument_index(Interface const& interface, Vector<EffectiveOverloadSet::Item> const& items, size_t argument_count)
{
    for (auto argument_index = 0u; argument_index < argument_count; ++argument_index) {
        bool found_indistinguishable = false;

        for (auto first_item_index = 0u; first_item_index < items.size(); ++first_item_index) {
            for (auto second_item_index = first_item_index + 1; second_item_index < items.size(); ++second_item_index) {
                if (!items[first_item_index].types[argument_index]->is_distinguishable_from(interface, items[second_item_index].types[argument_index])) {
                    found_indistinguishable = true;
                    break;
                }
            }
            if (found_indistinguishable)
                break;
        }

        if (!found_indistinguishable)
            return argument_index;
    }

    VERIFY_NOT_REACHED();
}

static void generate_overload_arbiter(SourceGenerator& generator, auto const& overload_set, IDL::Interface const& interface, ByteString const& class_name, IsConstructor is_constructor)
{
    auto function_generator = generator.fork();
    if (is_constructor == IsConstructor::Yes)
        function_generator.set("constructor_class", class_name);
    else
        function_generator.set("class_name", class_name);

    function_generator.set("function.name:snakecase", make_input_acceptable_cpp(overload_set.key.to_snakecase()));

    if (is_constructor == IsConstructor::Yes) {
        function_generator.append(R"~~~(
JS::ThrowCompletionOr<JS::NonnullGCPtr<JS::Object>> @constructor_class@::construct(JS::FunctionObject& new_target)
{
    auto& vm = this->vm();
    WebIDL::log_trace(vm, "@constructor_class@::construct");
)~~~");
    } else {
        function_generator.append(R"~~~(
JS_DEFINE_NATIVE_FUNCTION(@class_name@::@function.name:snakecase@)
{
    WebIDL::log_trace(vm, "@class_name@::@function.name:snakecase@");
)~~~");
    }

    function_generator.append(R"~~~(
    Optional<IDL::EffectiveOverloadSet> effective_overload_set;
)~~~");

    auto overloads_set = compute_the_effective_overload_set(overload_set);
    auto maximum_argument_count = 0u;
    for (auto const& overload : overloads_set)
        maximum_argument_count = max(maximum_argument_count, overload.types.size());
    function_generator.set("max_argument_count", ByteString::number(maximum_argument_count));
    function_generator.appendln("    switch (min(@max_argument_count@, vm.argument_count())) {");

    // Generate the effective overload set for each argument count.
    // This skips part of the Overload Resolution Algorithm https://webidl.spec.whatwg.org/#es-overloads
    // Namely, since that discards any overloads that don't have the exact number of arguments that were given,
    // we simply only provide the overloads that do have that number of arguments.
    for (auto argument_count = 0u; argument_count <= maximum_argument_count; ++argument_count) {
        Vector<EffectiveOverloadSet::Item> effective_overload_set;
        for (auto const& overload : overloads_set) {
            if (overload.types.size() == argument_count)
                effective_overload_set.append(overload);
        }

        if (effective_overload_set.size() == 0)
            continue;

        auto distinguishing_argument_index = 0u;
        if (effective_overload_set.size() > 1)
            distinguishing_argument_index = resolve_distinguishing_argument_index(interface, effective_overload_set, argument_count);

        function_generator.set("current_argument_count", ByteString::number(argument_count));
        function_generator.set("overload_count", ByteString::number(effective_overload_set.size()));
        function_generator.appendln(R"~~~(
    case @current_argument_count@: {
        Vector<IDL::EffectiveOverloadSet::Item> overloads;
        overloads.ensure_capacity(@overload_count@);
)~~~");

        for (auto& overload : effective_overload_set) {
            StringBuilder types_builder;
            types_builder.append("Vector<NonnullRefPtr<IDL::Type const>> { "sv);
            StringBuilder optionality_builder;
            optionality_builder.append("Vector<IDL::Optionality> { "sv);

            for (auto i = 0u; i < overload.types.size(); ++i) {
                if (i > 0) {
                    types_builder.append(", "sv);
                    optionality_builder.append(", "sv);
                }

                types_builder.append(generate_constructor_for_idl_type(overload.types[i]));

                optionality_builder.append("IDL::Optionality::"sv);
                switch (overload.optionality_values[i]) {
                case Optionality::Required:
                    optionality_builder.append("Required"sv);
                    break;
                case Optionality::Optional:
                    optionality_builder.append("Optional"sv);
                    break;
                case Optionality::Variadic:
                    optionality_builder.append("Variadic"sv);
                    break;
                }
            }

            types_builder.append("}"sv);
            optionality_builder.append("}"sv);

            function_generator.set("overload.callable_id", ByteString::number(overload.callable_id));
            function_generator.set("overload.types", types_builder.to_byte_string());
            function_generator.set("overload.optionality_values", optionality_builder.to_byte_string());

            function_generator.appendln("        overloads.empend(@overload.callable_id@, @overload.types@, @overload.optionality_values@);");
        }

        function_generator.set("overload_set.distinguishing_argument_index", ByteString::number(distinguishing_argument_index));
        function_generator.append(R"~~~(
        effective_overload_set.emplace(move(overloads), @overload_set.distinguishing_argument_index@);
        break;
    }
)~~~");
    }

    function_generator.append(R"~~~(
    }

    if (!effective_overload_set.has_value())
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::OverloadResolutionFailed);

    auto chosen_overload = TRY(WebIDL::resolve_overload(vm, effective_overload_set.value()));
    switch (chosen_overload.callable_id) {
)~~~");

    for (auto i = 0u; i < overload_set.value.size(); ++i) {
        function_generator.set("overload_id", ByteString::number(i));
        function_generator.append(R"~~~(
    case @overload_id@:
)~~~");
        if (is_constructor == IsConstructor::Yes) {
            function_generator.append(R"~~~(
        return construct@overload_id@(new_target);
)~~~");
        } else {
            function_generator.append(R"~~~(
        return @function.name:snakecase@@overload_id@(vm);
)~~~");
        }
    }

    function_generator.append(R"~~~(
    default:
        VERIFY_NOT_REACHED();
    }
}
)~~~");
}

static void generate_html_constructor(SourceGenerator& generator, IDL::Constructor const& constructor, IDL::Interface const& interface)
{
    auto constructor_generator = generator.fork();
    // NOTE: A HTMLConstrcuctor must not have any parameters.
    constructor_generator.set("constructor.length", "0");

    // https://html.spec.whatwg.org/multipage/dom.html#html-element-constructors
    // NOTE: The active function object in this context is always going to be the current constructor that has just been called.

    // The [HTMLConstructor] extended attribute must take no arguments, and must only appear on constructor operations. It must
    // appear only once on a constructor operation, and the interface must contain only the single, annotated constructor
    // operation, and no others
    if (interface.constructors.size() != 1) {
        dbgln("Interface {}'s constructor annotated with [HTMLConstructor] must be the only constructor", interface.name);
        VERIFY_NOT_REACHED();
    }

    if (!constructor.parameters.is_empty()) {
        dbgln("Interface {}'s constructor marked with [HTMLConstructor] must not have any parameters", interface.name);
        VERIFY_NOT_REACHED();
    }

    constructor_generator.append(R"~~~(
    auto& window = verify_cast<HTML::Window>(HTML::current_global_object());

    // 1. Let registry be the current global object's CustomElementRegistry object.
    auto registry = TRY(throw_dom_exception_if_needed(vm, [&] { return window.custom_elements(); }));

    // 2. If NewTarget is equal to the active function object, then throw a TypeError.
    if (&new_target == vm.active_function_object())
        return vm.throw_completion<JS::TypeError>("Cannot directly construct an HTML element, it must be inherited"sv);

    // 3. Let definition be the entry in registry with constructor equal to NewTarget. If there is no such definition, then throw a TypeError.
    auto definition = registry->get_definition_from_new_target(new_target);
    if (!definition)
        return vm.throw_completion<JS::TypeError>("There is no custom element definition assigned to the given constructor"sv);

    // 4. Let is value be null.
    Optional<String> is_value;

    // 5. If definition's local name is equal to definition's name (i.e., definition is for an autonomous custom element), then:
    if (definition->local_name() == definition->name()) {
        // 1. If the active function object is not HTMLElement, then throw a TypeError.
)~~~");

    if (interface.name != "HTMLElement") {
        constructor_generator.append(R"~~~(
        return vm.throw_completion<JS::TypeError>("Autonomous custom elements can only inherit from HTMLElement"sv);
)~~~");
    } else {
        constructor_generator.append(R"~~~(
        // Do nothing, as this is the HTMLElement constructor.
)~~~");
    }

    constructor_generator.append(R"~~~(
    }

    // 6. Otherwise (i.e., if definition is for a customized built-in element):
    else {
        // 1. Let valid local names be the list of local names for elements defined in this specification or in other applicable specifications that use the active function object as their element interface.
        static auto valid_local_names = MUST(DOM::valid_local_names_for_given_html_element_interface("@name@"sv));

        // 2. If valid local names does not contain definition's local name, then throw a TypeError.
        if (!valid_local_names.contains_slow(definition->local_name()))
            return vm.throw_completion<JS::TypeError>(MUST(String::formatted("Local name '{}' of customized built-in element is not a valid local name for @name@"sv, definition->local_name())));

        // 3. Set is value to definition's name.
        is_value = definition->name();
    }

    // 7. If definition's construction stack is empty, then:
    if (definition->construction_stack().is_empty()) {
        // 1. Let element be the result of internally creating a new object implementing the interface to which the active function object corresponds, given the current Realm Record and NewTarget.
        // 2. Set element's node document to the current global object's associated Document.
        // 3. Set element's namespace to the HTML namespace.
        // 4. Set element's namespace prefix to null.
        // 5. Set element's local name to definition's local name.
        auto element = realm.heap().allocate<@fully_qualified_name@>(realm, window.associated_document(), DOM::QualifiedName { definition->local_name(), {}, Namespace::HTML });

        // https://webidl.spec.whatwg.org/#internally-create-a-new-object-implementing-the-interface
        // Important steps from "internally create a new object implementing the interface"
        // 3.2: Let prototype be ? Get(newTarget, "prototype").
        auto prototype = TRY(new_target.get(vm.names.prototype));

        // 3.3. If Type(prototype) is not Object, then:
        if (!prototype.is_object()) {
            // 1. Let targetRealm be ? GetFunctionRealm(newTarget).
            auto* target_realm = TRY(JS::get_function_realm(vm, new_target));

            // 2. Set prototype to the interface prototype object for interface in targetRealm.
            VERIFY(target_realm);
            prototype = &Bindings::ensure_web_prototype<@prototype_class@>(*target_realm, "@name@"_fly_string);
        }

        // 7. Set instance.[[Prototype]] to prototype.
        VERIFY(prototype.is_object());
        MUST(element->internal_set_prototype_of(&prototype.as_object()));

        // 6. Set element's custom element state to "custom".
        // 7. Set element's custom element definition to definition.
        // 8. Set element's is value to is value.
        element->setup_custom_element_from_constructor(*definition, is_value);

        // 9. Return element.
        return *element;
    }

    // 8. Let prototype be ? Get(NewTarget, "prototype").
    auto prototype = TRY(new_target.get(vm.names.prototype));

    // 9. If Type(prototype) is not Object, then:
    if (!prototype.is_object()) {
        // 1. Let realm be ? GetFunctionRealm(NewTarget).
        auto* function_realm = TRY(JS::get_function_realm(vm, new_target));

        // 2. Set prototype to the interface prototype object of realm whose interface is the same as the interface of the active function object.
        VERIFY(function_realm);
        prototype = &Bindings::ensure_web_prototype<@prototype_class@>(*function_realm, "@name@"_fly_string);
    }

    VERIFY(prototype.is_object());

    // 10. Let element be the last entry in definition's construction stack.
    auto& element = definition->construction_stack().last();

    // 11. If element is an already constructed marker, then throw an "InvalidStateError" DOMException.
    if (element.has<HTML::AlreadyConstructedCustomElementMarker>())
        return JS::throw_completion(WebIDL::InvalidStateError::create(realm, "Custom element has already been constructed"_fly_string));

    // 12. Perform ? element.[[SetPrototypeOf]](prototype).
    auto actual_element = element.get<JS::Handle<DOM::Element>>();
    TRY(actual_element->internal_set_prototype_of(&prototype.as_object()));

    // 13. Replace the last entry in definition's construction stack with an already constructed marker.
    definition->construction_stack().last() = HTML::AlreadyConstructedCustomElementMarker {};

    // 14. Return element.
    return *actual_element;
}
)~~~");
}

static void generate_constructor(SourceGenerator& generator, IDL::Constructor const& constructor, IDL::Interface const& interface, bool is_html_constructor)
{
    auto constructor_generator = generator.fork();
    constructor_generator.set("constructor_class", interface.constructor_class);
    constructor_generator.set("interface_fully_qualified_name", interface.fully_qualified_name);
    constructor_generator.set("overload_suffix", constructor.is_overloaded ? ByteString::number(constructor.overload_index) : ByteString::empty());

    constructor_generator.append(R"~~~(
JS::ThrowCompletionOr<JS::NonnullGCPtr<JS::Object>> @constructor_class@::construct@overload_suffix@([[maybe_unused]] FunctionObject& new_target)
{
    WebIDL::log_trace(vm(), "@constructor_class@::construct@overload_suffix@");
)~~~");

    generator.append(R"~~~(
    auto& vm = this->vm();
    auto& realm = *vm.current_realm();
)~~~");

    if (is_html_constructor) {
        generate_html_constructor(generator, constructor, interface);
    } else {
        generator.append(R"~~~(
    // To internally create a new object implementing the interface @name@:

    // 3.2. Let prototype be ? Get(newTarget, "prototype").
    auto prototype = TRY(new_target.get(vm.names.prototype));

    // 3.3. If Type(prototype) is not Object, then:
    if (!prototype.is_object()) {    
        // 1. Let targetRealm be ? GetFunctionRealm(newTarget).
        auto* target_realm = TRY(JS::get_function_realm(vm, new_target));

        // 2. Set prototype to the interface prototype object for interface in targetRealm.
        VERIFY(target_realm);
        prototype = &Bindings::ensure_web_prototype<@prototype_class@>(*target_realm, "@name@"_fly_string);
    }

    // 4. Let instance be MakeBasicObject( « [[Prototype]], [[Extensible]], [[Realm]], [[PrimaryInterface]] »).
    // 5. Set instance.[[Realm]] to realm.
    // 6. Set instance.[[PrimaryInterface]] to interface.
)~~~");
        if (!constructor.parameters.is_empty()) {
            generate_argument_count_check(generator, constructor.name, constructor.shortest_length());

            StringBuilder arguments_builder;
            generate_arguments(generator, constructor.parameters, arguments_builder, interface);
            constructor_generator.set(".constructor_arguments", arguments_builder.string_view());

            constructor_generator.append(R"~~~(
    auto impl = TRY(throw_dom_exception_if_needed(vm, [&] { return @fully_qualified_name@::construct_impl(realm, @.constructor_arguments@); }));
)~~~");
        } else {
            constructor_generator.append(R"~~~(
    auto impl = TRY(throw_dom_exception_if_needed(vm, [&] { return @fully_qualified_name@::construct_impl(realm); }));
)~~~");
        }

        constructor_generator.append(R"~~~(
    // 7. Set instance.[[Prototype]] to prototype.
    VERIFY(prototype.is_object());
    impl->set_prototype(&prototype.as_object());

    // FIXME: Steps 8...11. of the "internally create a new object implementing the interface @name@" algorithm
    // (https://webidl.spec.whatwg.org/#js-platform-objects) are currently not handled, or are handled within @fully_qualified_name@::construct_impl().
    //  8. Let interfaces be the inclusive inherited interfaces of interface.
    //  9. For every interface ancestor interface in interfaces:
    //    9.1. Let unforgeables be the value of the [[Unforgeables]] slot of the interface object of ancestor interface in realm.
    //    9.2. Let keys be ! unforgeables.[[OwnPropertyKeys]]().
    //    9.3. For each element key of keys:
    //      9.3.1. Let descriptor be ! unforgeables.[[GetOwnProperty]](key).
    //      9.3.2. Perform ! DefinePropertyOrThrow(instance, key, descriptor).
    //  10. If interface is declared with the [Global] extended attribute, then:
    //    10.1. Define the regular operations of interface on instance, given realm.
    //    10.2. Define the regular attributes of interface on instance, given realm.
    //    10.3. Define the iteration methods of interface on instance given realm.
    //    10.4. Define the asynchronous iteration methods of interface on instance given realm.
    //    10.5. Define the global property references on instance, given realm.
    //    10.6. Set instance.[[SetPrototypeOf]] as defined in § 3.8.1 [[SetPrototypeOf]].
    //  11. Otherwise, if interfaces contains an interface which supports indexed properties, named properties, or both:
    //    11.1. Set instance.[[GetOwnProperty]] as defined in § 3.9.1 [[GetOwnProperty]].
    //    11.2. Set instance.[[Set]] as defined in § 3.9.2 [[Set]].
    //    11.3. Set instance.[[DefineOwnProperty]] as defined in § 3.9.3 [[DefineOwnProperty]].
    //    11.4. Set instance.[[Delete]] as defined in § 3.9.4 [[Delete]].
    //    11.5. Set instance.[[PreventExtensions]] as defined in § 3.9.5 [[PreventExtensions]].
    //    11.6. Set instance.[[OwnPropertyKeys]] as defined in § 3.9.6 [[OwnPropertyKeys]].

    return *impl;
}
)~~~");
    }
}

static void generate_constructors(SourceGenerator& generator, IDL::Interface const& interface)
{
    auto shortest_length = interface.constructors.is_empty() ? 0u : NumericLimits<size_t>::max();
    bool has_html_constructor = false;
    for (auto const& constructor : interface.constructors) {
        shortest_length = min(shortest_length, constructor.shortest_length());

        if (constructor.extended_attributes.contains("HTMLConstructor"sv)) {
            has_html_constructor = true;
            break;
        }
    }

    if (has_html_constructor && interface.constructors.size() != 1) {
        dbgln("Interface {}'s constructor annotated with [HTMLConstructor] must be the only constructor", interface.name);
        VERIFY_NOT_REACHED();
    }

    generator.set("constructor.length", ByteString::number(shortest_length));

    // Implementation: Constructors
    if (interface.constructors.is_empty()) {
        // No constructor
        generator.append(R"~~~(
JS::ThrowCompletionOr<JS::NonnullGCPtr<JS::Object>> @constructor_class@::construct([[maybe_unused]] FunctionObject& new_target)
{
    WebIDL::log_trace(vm(), "@constructor_class@::construct");
)~~~");
        generator.set("constructor.length", "0");
        generator.append(R"~~~(
    return vm().throw_completion<JS::TypeError>(JS::ErrorType::NotAConstructor, "@namespaced_name@");
}
)~~~");
    } else {
        for (auto& constructor : interface.constructors) {
            generate_constructor(generator, constructor, interface, has_html_constructor);
        }
    }
    for (auto const& overload_set : interface.constructor_overload_sets) {
        if (overload_set.value.size() == 1)
            continue;
        generate_overload_arbiter(generator, overload_set, interface, interface.constructor_class, IsConstructor::Yes);
    }
}

static void generate_enumerations(HashMap<ByteString, Enumeration> const& enumerations, StringBuilder& builder)
{
    SourceGenerator generator { builder };

    for (auto const& it : enumerations) {
        if (!it.value.is_original_definition)
            continue;
        auto enum_generator = generator.fork();
        enum_generator.set("enum.type.name", it.key);
        enum_generator.append(R"~~~(
enum class @enum.type.name@ {
)~~~");
        for (auto const& entry : it.value.translated_cpp_names) {
            enum_generator.set("enum.entry", entry.value);
            enum_generator.append(R"~~~(
    @enum.entry@,
)~~~");
        }

        enum_generator.append(R"~~~(
};
)~~~");

        enum_generator.append(R"~~~(
inline String idl_enum_to_string(@enum.type.name@ value)
{
    switch (value) {
)~~~");
        for (auto const& entry : it.value.translated_cpp_names) {
            enum_generator.set("enum.entry", entry.value);
            enum_generator.set("enum.string", entry.key);
            enum_generator.append(R"~~~(
    case @enum.type.name@::@enum.entry@:
        return "@enum.string@"_string;
)~~~");
        }
        enum_generator.append(R"~~~(
    }
    VERIFY_NOT_REACHED();
}
)~~~");
    }
}

static void generate_prototype_or_global_mixin_declarations(IDL::Interface const& interface, StringBuilder& builder)
{
    SourceGenerator generator { builder };

    for (auto const& overload_set : interface.overload_sets) {
        auto function_generator = generator.fork();
        function_generator.set("function.name:snakecase", make_input_acceptable_cpp(overload_set.key.to_snakecase()));
        function_generator.append(R"~~~(
    JS_DECLARE_NATIVE_FUNCTION(@function.name:snakecase@);
        )~~~");
        if (overload_set.value.size() > 1) {
            for (auto i = 0u; i < overload_set.value.size(); ++i) {
                function_generator.set("overload_suffix", ByteString::number(i));
                function_generator.append(R"~~~(
    JS_DECLARE_NATIVE_FUNCTION(@function.name:snakecase@@overload_suffix@);
)~~~");
            }
        }
    }

    if (interface.has_stringifier) {
        auto stringifier_generator = generator.fork();
        stringifier_generator.append(R"~~~(
    JS_DECLARE_NATIVE_FUNCTION(to_string);
        )~~~");
    }

    if (interface.pair_iterator_types.has_value()) {
        auto iterator_generator = generator.fork();
        iterator_generator.append(R"~~~(
    JS_DECLARE_NATIVE_FUNCTION(entries);
    JS_DECLARE_NATIVE_FUNCTION(for_each);
    JS_DECLARE_NATIVE_FUNCTION(keys);
    JS_DECLARE_NATIVE_FUNCTION(values);
        )~~~");
    }

    if (interface.set_entry_type.has_value()) {
        auto setlike_generator = generator.fork();

        setlike_generator.append(R"~~~(
    JS_DECLARE_NATIVE_FUNCTION(get_size);
    JS_DECLARE_NATIVE_FUNCTION(entries);
    JS_DECLARE_NATIVE_FUNCTION(values);
    JS_DECLARE_NATIVE_FUNCTION(for_each);
    JS_DECLARE_NATIVE_FUNCTION(has);
)~~~");
        if (!interface.overload_sets.contains("add"sv) && !interface.is_set_readonly) {
            setlike_generator.append(R"~~~(
    JS_DECLARE_NATIVE_FUNCTION(add);
)~~~");
        }
        if (!interface.overload_sets.contains("delete"sv) && !interface.is_set_readonly) {
            setlike_generator.append(R"~~~(
    JS_DECLARE_NATIVE_FUNCTION(delete_);
)~~~");
        }
        if (!interface.overload_sets.contains("clear"sv) && !interface.is_set_readonly) {
            setlike_generator.append(R"~~~(
    JS_DECLARE_NATIVE_FUNCTION(clear);
)~~~");
        }
    }

    for (auto& attribute : interface.attributes) {
        if (attribute.extended_attributes.contains("FIXME"))
            continue;
        auto attribute_generator = generator.fork();
        attribute_generator.set("attribute.name:snakecase", attribute.name.to_snakecase());
        attribute_generator.append(R"~~~(
    JS_DECLARE_NATIVE_FUNCTION(@attribute.name:snakecase@_getter);
)~~~");

        if (!attribute.readonly || attribute.extended_attributes.contains("Replaceable"sv) || attribute.extended_attributes.contains("PutForwards"sv)) {
            attribute_generator.append(R"~~~(
    JS_DECLARE_NATIVE_FUNCTION(@attribute.name:snakecase@_setter);
)~~~");
        }
    }

    generator.append(R"~~~(

};

)~~~");

    generate_enumerations(interface.enumerations, builder);
}

// https://webidl.spec.whatwg.org/#create-an-inheritance-stack
static Vector<Interface const&> create_an_inheritance_stack(IDL::Interface const& start_interface)
{
    // 1. Let stack be a new stack.
    Vector<Interface const&> inheritance_chain;

    // 2. Push I onto stack.
    inheritance_chain.append(start_interface);

    // 3. While I inherits from an interface,
    auto const* current_interface = &start_interface;
    while (current_interface && !current_interface->parent_name.is_empty()) {
        // 1. Let I be that interface.
        auto imported_interface_iterator = start_interface.imported_modules.find_if([&current_interface](IDL::Interface const& imported_interface) {
            return imported_interface.name == current_interface->parent_name;
        });

        // Inherited interfaces must have their IDL files imported.
        VERIFY(imported_interface_iterator != start_interface.imported_modules.end());

        // 2. Push I onto stack.
        inheritance_chain.append(*imported_interface_iterator);

        current_interface = &*imported_interface_iterator;
    }

    // 4. Return stack.
    return inheritance_chain;
}

// https://webidl.spec.whatwg.org/#collect-attribute-values-of-an-inheritance-stack
static void collect_attribute_values_of_an_inheritance_stack(SourceGenerator& function_generator, Vector<Interface const&> const& inheritance_chain)
{
    // 1. Let I be the result of popping from stack.
    // 3. If stack is not empty, then invoke collect attribute values of an inheritance stack given object, stack, and map.
    for (auto const& interface_in_chain : inheritance_chain.in_reverse()) {
        // 2. Invoke collect attribute values given object, I, and map.
        // https://webidl.spec.whatwg.org/#collect-attribute-values
        // 1. If a toJSON operation with a [Default] extended attribute is declared on I, then for each exposed regular attribute attr that is an interface member of I, in order:
        auto to_json_iterator = interface_in_chain.functions.find_if([](IDL::Function const& function) {
            return function.name == "toJSON" && function.extended_attributes.contains("Default");
        });

        if (to_json_iterator == interface_in_chain.functions.end())
            continue;

        // FIXME: Check if the attributes are exposed.

        // 1. Let id be the identifier of attr.
        // 2. Let value be the result of running the getter steps of attr with object as this.

        // 3. If value is a JSON type, then set map[id] to value.
        // Since we are flatly generating the attributes, the consequent is replaced with these steps from "default toJSON steps":
        // 5. For each key → value of map,
        //    1. Let k be key converted to an ECMAScript value.
        //    2. Let v be value converted to an ECMAScript value.
        //    3. Perform ! CreateDataProperty(result, k, v).

        // NOTE: Functions, constructors and static functions cannot be JSON types, so they're not checked here.

        for (auto& attribute : interface_in_chain.attributes) {
            if (attribute.extended_attributes.contains("FIXME"))
                continue;
            if (!attribute.type->is_json(interface_in_chain))
                continue;

            auto attribute_generator = function_generator.fork();
            auto return_value_name = ByteString::formatted("{}_retval", attribute.name.to_snakecase());

            attribute_generator.set("attribute.name", attribute.name);
            attribute_generator.set("attribute.return_value_name", return_value_name);

            if (attribute.extended_attributes.contains("ImplementedAs")) {
                auto implemented_as = attribute.extended_attributes.get("ImplementedAs").value();
                attribute_generator.set("attribute.cpp_name", implemented_as);
            } else {
                attribute_generator.set("attribute.cpp_name", attribute.name.to_snakecase());
            }

            if (attribute.extended_attributes.contains("Reflect")) {
                auto attribute_name = attribute.extended_attributes.get("Reflect").value();
                if (attribute_name.is_empty())
                    attribute_name = attribute.name;
                attribute_name = make_input_acceptable_cpp(attribute_name);

                attribute_generator.set("attribute.reflect_name", attribute_name);
            } else {
                attribute_generator.set("attribute.reflect_name", attribute.name.to_snakecase());
            }

            if (attribute.extended_attributes.contains("Reflect")) {
                if (attribute.type->name() != "boolean") {
                    attribute_generator.append(R"~~~(
    auto @attribute.return_value_name@ = impl->get_attribute_value(HTML::AttributeNames::@attribute.reflect_name@);
)~~~");
                } else {
                    attribute_generator.append(R"~~~(
    auto @attribute.return_value_name@ = impl->has_attribute(HTML::AttributeNames::@attribute.reflect_name@);
)~~~");
                }
            } else {
                attribute_generator.append(R"~~~(
    auto @attribute.return_value_name@ = TRY(throw_dom_exception_if_needed(vm, [&] { return impl->@attribute.cpp_name@(); }));
)~~~");
            }

            generate_wrap_statement(attribute_generator, return_value_name, attribute.type, interface_in_chain, ByteString::formatted("auto {}_wrapped =", return_value_name));

            attribute_generator.append(R"~~~(
    MUST(result->create_data_property("@attribute.name@", @attribute.return_value_name@_wrapped));
)~~~");
        }

        for (auto& constant : interface_in_chain.constants) {
            auto constant_generator = function_generator.fork();
            constant_generator.set("constant.name", constant.name);

            generate_wrap_statement(constant_generator, constant.value, constant.type, interface_in_chain, ByteString::formatted("auto constant_{}_value =", constant.name));

            constant_generator.append(R"~~~(
    MUST(result->create_data_property("@constant.name@", constant_@constant.name@_value));
)~~~");
        }
    }
}

// https://webidl.spec.whatwg.org/#default-tojson-steps
static void generate_default_to_json_function(SourceGenerator& generator, ByteString const& class_name, IDL::Interface const& start_interface)
{
    // NOTE: This is done heavily out of order since the spec mixes parse time and run time type information together.

    auto function_generator = generator.fork();
    function_generator.set("class_name", class_name);

    // 4. Let result be OrdinaryObjectCreate(%Object.prototype%).
    function_generator.append(R"~~~(
JS_DEFINE_NATIVE_FUNCTION(@class_name@::to_json)
{
    WebIDL::log_trace(vm, "@class_name@::to_json");
    auto& realm = *vm.current_realm();
    auto* impl = TRY(impl_from(vm));

    auto result = JS::Object::create(realm, realm.intrinsics().object_prototype());
)~~~");

    // 1. Let map be a new ordered map.
    // NOTE: Instead of making a map, we flatly generate the attributes.

    // 2. Let stack be the result of creating an inheritance stack for interface I.
    auto inheritance_chain = create_an_inheritance_stack(start_interface);

    // 3. Invoke collect attribute values of an inheritance stack given this, stack, and map.
    collect_attribute_values_of_an_inheritance_stack(function_generator, inheritance_chain);

    // NOTE: Step 5 is done as part of collect_attribute_values_of_an_inheritance_stack, due to us flatly generating the attributes.

    // 6. Return result.
    function_generator.append(R"~~~(
    return result;
}
)~~~");
}

static void generate_named_properties_object_declarations(IDL::Interface const& interface, StringBuilder& builder)
{
    SourceGenerator generator { builder };

    generator.set("named_properties_class", ByteString::formatted("{}Properties", interface.name));

    generator.append(R"~~~(
class @named_properties_class@ : public JS::Object {
    JS_OBJECT(@named_properties_class@, JS::Object);
    JS_DECLARE_ALLOCATOR(@named_properties_class@);
public:
    explicit @named_properties_class@(JS::Realm&);
    virtual void initialize(JS::Realm&) override;
    virtual ~@named_properties_class@() override;

    JS::Realm& realm() const { return m_realm; }
private:
    virtual JS::ThrowCompletionOr<Optional<JS::PropertyDescriptor>> internal_get_own_property(JS::PropertyKey const&) const override;
    virtual JS::ThrowCompletionOr<bool> internal_define_own_property(JS::PropertyKey const&, JS::PropertyDescriptor const&) override;
    virtual JS::ThrowCompletionOr<bool> internal_delete(JS::PropertyKey const&) override;
    virtual JS::ThrowCompletionOr<bool> internal_set_prototype_of(JS::Object* prototype) override;
    virtual JS::ThrowCompletionOr<bool> internal_prevent_extensions() override;

    virtual void visit_edges(Visitor&) override;

    JS::NonnullGCPtr<JS::Realm> m_realm; // [[Realm]]
};
)~~~");
}

static void generate_named_properties_object_definitions(IDL::Interface const& interface, StringBuilder& builder)
{
    SourceGenerator generator { builder };

    generator.set("name", interface.name);
    generator.set("parent_name", interface.parent_name);
    generator.set("prototype_base_class", interface.prototype_base_class);
    generator.set("named_properties_class", ByteString::formatted("{}Properties", interface.name));

    // https://webidl.spec.whatwg.org/#create-a-named-properties-object
    generator.append(R"~~~(
#include <LibWeb/WebIDL/AbstractOperations.h>

JS_DEFINE_ALLOCATOR(@named_properties_class@);

@named_properties_class@::@named_properties_class@(JS::Realm& realm)
  : JS::Object(realm, nullptr, MayInterfereWithIndexedPropertyAccess::Yes)
  , m_realm(realm)
{
}

@named_properties_class@::~@named_properties_class@()
{
}

void @named_properties_class@::initialize(JS::Realm& realm)
{
    auto& vm = realm.vm();

    // The class string of a named properties object is the concatenation of the interface's identifier and the string "Properties".
    define_direct_property(vm.well_known_symbol_to_string_tag(), JS::PrimitiveString::create(vm, "@named_properties_class@"_string), JS::Attribute::Configurable);
)~~~");

    // 1. Let proto be null
    // 2. If interface is declared to inherit from another interface, then set proto to the interface prototype object in realm for the inherited interface.
    // 3. Otherwise, set proto to realm.[[Intrinsics]].[[%Object.prototype%]].
    // NOTE: Steps 4-9 handled by constructor + other overridden functions
    // 10. Set obj.[[Prototype]] to proto.
    if (interface.prototype_base_class == "ObjectPrototype") {
        generator.append(R"~~~(

    set_prototype(realm.intrinsics().object_prototype());
)~~~");
    } else {
        generator.append(R"~~~(

    set_prototype(&ensure_web_prototype<@prototype_base_class@>(realm, "@parent_name@"_fly_string));
)~~~");
    }

    generator.append(R"~~~(
};

// https://webidl.spec.whatwg.org/#named-properties-object-getownproperty
JS::ThrowCompletionOr<Optional<JS::PropertyDescriptor>> @named_properties_class@::internal_get_own_property(JS::PropertyKey const& property_name) const
{
    auto& vm = this->vm();
    auto& realm = this->realm();

    // 1. Let A be the interface for the named properties object O.
    using A = @name@;

    // 2. Let object be O.[[Realm]]'s global object.
    // 3. Assert: object implements A.
    auto& object = verify_cast<A>(realm.global_object());

    // 4. If the result of running the named property visibility algorithm with property name P and object object is true, then:
    if (TRY(object.is_named_property_exposed_on_object(property_name))) {
        auto property_name_string = MUST(FlyString::from_deprecated_fly_string(property_name.to_string()));

        // 1. Let operation be the operation used to declare the named property getter.
        // 2. Let value be an uninitialized variable.
        // 3. If operation was defined without an identifier, then set value to the result of performing the steps listed in the interface description to determine the value of a named property with P as the name.
        // 4. Otherwise, operation was defined with an identifier. Set value to the result of performing the method steps of operation with « P » as the only argument value.
        auto value = TRY(throw_dom_exception_if_needed(vm, [&] { return object.named_item_value(property_name_string); }));

        // 5. Let desc be a newly created Property Descriptor with no fields.
        JS::PropertyDescriptor descriptor;

        // 6. Set desc.[[Value]] to the result of converting value to an ECMAScript value.
        descriptor.value = value;
)~~~");
    if (interface.extended_attributes.contains("LegacyUnenumerableNamedProperties"))
        generator.append(R"~~~(
        // 7. If A implements an interface with the [LegacyUnenumerableNamedProperties] extended attribute, then set desc.[[Enumerable]] to false, otherwise set it to true.
        descriptor.enumerable = true;
)~~~");
    else {
        generator.append(R"~~~(
        // 7. If A implements an interface with the [LegacyUnenumerableNamedProperties] extended attribute, then set desc.[[Enumerable]] to false, otherwise set it to true.
        descriptor.enumerable = false;
)~~~");
    }
    generator.append(R"~~~(
        // 8. Set desc.[[Writable]] to true and desc.[[Configurable]] to true.
        descriptor.writable = true;
        descriptor.configurable = true;

        // 9. Return desc.
        return descriptor;
    }

    // 5. Return OrdinaryGetOwnProperty(O, P).
    return JS::Object::internal_get_own_property(property_name);
}

// https://webidl.spec.whatwg.org/#named-properties-object-defineownproperty
JS::ThrowCompletionOr<bool> @named_properties_class@::internal_define_own_property(JS::PropertyKey const&, JS::PropertyDescriptor const&)
{
    // 1. Return false.
    return false;
}

// https://webidl.spec.whatwg.org/#named-properties-object-delete
JS::ThrowCompletionOr<bool> @named_properties_class@::internal_delete(JS::PropertyKey const&)
{
    // 1. Return false.
    return false;
}

// https://webidl.spec.whatwg.org/#named-properties-object-setprototypeof
JS::ThrowCompletionOr<bool> @named_properties_class@::internal_set_prototype_of(JS::Object* prototype)
{
    // 1. Return ? SetImmutablePrototype(O, V).
    return set_immutable_prototype(prototype);
}

// https://webidl.spec.whatwg.org/#named-properties-object-preventextensions
JS::ThrowCompletionOr<bool> @named_properties_class@::internal_prevent_extensions()
{
    // 1. Return false.
    // Note: this keeps named properties object extensible by making [[PreventExtensions]] fail.
    return false;
}

void @named_properties_class@::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_realm);
}
)~~~");
}

// https://webidl.spec.whatwg.org/#interface-prototype-object
static void generate_prototype_or_global_mixin_definitions(IDL::Interface const& interface, StringBuilder& builder)
{
    SourceGenerator generator { builder };

    auto is_global_interface = interface.extended_attributes.contains("Global");
    auto class_name = is_global_interface ? interface.global_mixin_class : interface.prototype_class;
    generator.set("name", interface.name);
    generator.set("namespaced_name", interface.namespaced_name);
    generator.set("class_name", class_name);
    generator.set("fully_qualified_name", interface.fully_qualified_name);
    generator.set("parent_name", interface.parent_name);
    generator.set("prototype_base_class", interface.prototype_base_class);
    generator.set("prototype_name", interface.prototype_class); // Used for Global Mixin

    if (interface.pair_iterator_types.has_value()) {
        generator.set("iterator_name", ByteString::formatted("{}Iterator", interface.name));
    }

    if (is_global_interface) {
        generator.set("named_properties_class", ByteString::formatted("{}Properties", interface.name));
        // Doing this with macros is not super nice, but simplifies codegen a lot.
        generator.append(R"~~~(
#define define_direct_property (object.define_direct_property)
#define define_native_accessor (object.define_native_accessor)
#define define_native_function (object.define_native_function)
#define set_prototype (object.set_prototype)

void @class_name@::initialize(JS::Realm& realm, JS::Object& object)
{
)~~~");
    } else {
        generator.append(R"~~~(
void @class_name@::initialize(JS::Realm& realm)
{
)~~~");
    }

    // FIXME: Currently almost everything gets default_attributes but it should be configurable per attribute.
    //        See the spec links for details
    generator.append(R"~~~(

    [[maybe_unused]] auto& vm = realm.vm();
    [[maybe_unused]] u8 default_attributes = JS::Attribute::Enumerable | JS::Attribute::Configurable | JS::Attribute::Writable;

)~~~");

    if (interface.name == "DOMException"sv) {
        generator.append(R"~~~(

    set_prototype(realm.intrinsics().error_prototype());
)~~~");
    }

    else if (interface.prototype_base_class == "ObjectPrototype") {
        generator.append(R"~~~(

    set_prototype(realm.intrinsics().object_prototype());

)~~~");
    } else if (is_global_interface && interface.supports_named_properties()) {
        generator.append(R"~~~(
    set_prototype(&ensure_web_prototype<@prototype_name@>(realm, "@name@"_fly_string));
)~~~");
    } else {
        generator.append(R"~~~(

    set_prototype(&ensure_web_prototype<@prototype_base_class@>(realm, "@parent_name@"_fly_string));

)~~~");
    }

    if (interface.has_unscopable_member) {
        generator.append(R"~~~(
    auto unscopable_object = JS::Object::create(realm, nullptr);
)~~~");
    }

    // https://webidl.spec.whatwg.org/#es-attributes
    for (auto& attribute : interface.attributes) {
        if (attribute.extended_attributes.contains("FIXME")) {
            auto fixme_attribute_generator = generator.fork();
            fixme_attribute_generator.set("attribute.name", attribute.name);
            fixme_attribute_generator.append(R"~~~(
    define_direct_property("@attribute.name@", JS::js_undefined(), default_attributes | JS::Attribute::Unimplemented);
            )~~~");
            continue;
        }

        auto attribute_generator = generator.fork();
        attribute_generator.set("attribute.name", attribute.name);
        attribute_generator.set("attribute.getter_callback", attribute.getter_callback_name);

        if (!attribute.readonly || attribute.extended_attributes.contains("Replaceable"sv) || attribute.extended_attributes.contains("PutForwards"sv))
            attribute_generator.set("attribute.setter_callback", attribute.setter_callback_name);
        else
            attribute_generator.set("attribute.setter_callback", "nullptr");

        if (attribute.extended_attributes.contains("Unscopable")) {
            attribute_generator.append(R"~~~(
    MUST(unscopable_object->create_data_property("@attribute.name@", JS::Value(true)));
)~~~");
        }

        attribute_generator.append(R"~~~(
    define_native_accessor(realm, "@attribute.name@", @attribute.getter_callback@, @attribute.setter_callback@, default_attributes);
)~~~");
    }

    for (auto& function : interface.functions) {
        if (function.extended_attributes.contains("FIXME")) {
            auto fixme_function_generator = generator.fork();
            fixme_function_generator.set("function.name", function.name);
            fixme_function_generator.append(R"~~~(
        define_direct_property("@function.name@", JS::js_undefined(), default_attributes | JS::Attribute::Unimplemented);
            )~~~");
        }
    }

    // https://webidl.spec.whatwg.org/#es-constants
    for (auto& constant : interface.constants) {
        // FIXME: Do constants need to be added to the unscopable list?

        auto constant_generator = generator.fork();
        constant_generator.set("constant.name", constant.name);

        generate_wrap_statement(constant_generator, constant.value, constant.type, interface, ByteString::formatted("auto constant_{}_value =", constant.name));

        constant_generator.append(R"~~~(
    define_direct_property("@constant.name@", constant_@constant.name@_value, JS::Attribute::Enumerable);
)~~~");
    }

    // https://webidl.spec.whatwg.org/#es-operations
    for (auto const& overload_set : interface.overload_sets) {
        auto function_generator = generator.fork();
        function_generator.set("function.name", overload_set.key);
        function_generator.set("function.name:snakecase", make_input_acceptable_cpp(overload_set.key.to_snakecase()));
        function_generator.set("function.length", ByteString::number(get_shortest_function_length(overload_set.value)));

        // FIXME: What if only some of the overloads are Unscopable?
        if (any_of(overload_set.value, [](auto const& function) { return function.extended_attributes.contains("Unscopable"); })) {
            function_generator.append(R"~~~(
    MUST(unscopable_object->create_data_property("@function.name@", JS::Value(true)));
)~~~");
        }

        function_generator.append(R"~~~(
    define_native_function(realm, "@function.name@", @function.name:snakecase@, @function.length@, default_attributes);
)~~~");
    }

    if (interface.has_stringifier) {
        // FIXME: Do stringifiers need to be added to the unscopable list?

        auto stringifier_generator = generator.fork();
        stringifier_generator.append(R"~~~(
    define_native_function(realm, "toString", to_string, 0, default_attributes);
)~~~");
    }

    // https://webidl.spec.whatwg.org/#define-the-iteration-methods
    // This applies to this if block and the following if block.
    if (interface.indexed_property_getter.has_value()) {
        auto iterator_generator = generator.fork();
        iterator_generator.append(R"~~~(
    define_direct_property(vm.well_known_symbol_iterator(), realm.intrinsics().array_prototype()->get_without_side_effects(vm.names.values), JS::Attribute::Configurable | JS::Attribute::Writable);
)~~~");

        if (interface.value_iterator_type.has_value()) {
            iterator_generator.append(R"~~~(
    define_direct_property(vm.names.entries, realm.intrinsics().array_prototype()->get_without_side_effects(vm.names.entries), default_attributes);
    define_direct_property(vm.names.keys, realm.intrinsics().array_prototype()->get_without_side_effects(vm.names.keys), default_attributes);
    define_direct_property(vm.names.values, realm.intrinsics().array_prototype()->get_without_side_effects(vm.names.values), default_attributes);
    define_direct_property(vm.names.forEach, realm.intrinsics().array_prototype()->get_without_side_effects(vm.names.forEach), default_attributes);
)~~~");
        }
    }

    if (interface.pair_iterator_types.has_value()) {
        // FIXME: Do pair iterators need to be added to the unscopable list?

        auto iterator_generator = generator.fork();
        iterator_generator.append(R"~~~(
    define_native_function(realm, vm.names.entries, entries, 0, default_attributes);
    define_native_function(realm, vm.names.forEach, for_each, 1, default_attributes);
    define_native_function(realm, vm.names.keys, keys, 0, default_attributes);
    define_native_function(realm, vm.names.values, values, 0, default_attributes);

    define_direct_property(vm.well_known_symbol_iterator(), get_without_side_effects(vm.names.entries), JS::Attribute::Configurable | JS::Attribute::Writable);
)~~~");
    }

    // https://webidl.spec.whatwg.org/#js-setlike
    if (interface.set_entry_type.has_value()) {

        auto setlike_generator = generator.fork();

        setlike_generator.append(R"~~~(
    define_native_accessor(realm, vm.names.size, get_size, nullptr, JS::Attribute::Enumerable | JS::Attribute::Configurable);
    define_native_function(realm, vm.names.entries, entries, 0, default_attributes);
    // NOTE: Keys intentionally returns values for setlike
    define_native_function(realm, vm.names.keys, values, 0, default_attributes);
    define_native_function(realm, vm.names.values, values, 0, default_attributes);
    define_direct_property(vm.well_known_symbol_iterator(), get_without_side_effects(vm.names.values), JS::Attribute::Configurable | JS::Attribute::Writable);
    define_native_function(realm, vm.names.forEach, for_each, 1, default_attributes);
    define_native_function(realm, vm.names.has, has, 1, default_attributes);
)~~~");

        if (!interface.overload_sets.contains("add"sv) && !interface.is_set_readonly) {
            setlike_generator.append(R"~~~(
    define_native_function(realm, vm.names.add, add, 1, default_attributes);
)~~~");
        }
        if (!interface.overload_sets.contains("delete"sv) && !interface.is_set_readonly) {
            setlike_generator.append(R"~~~(
    define_native_function(realm, vm.names.delete_, delete_, 1, default_attributes);
)~~~");
        }
        if (!interface.overload_sets.contains("clear"sv) && !interface.is_set_readonly) {
            setlike_generator.append(R"~~~(
    define_native_function(realm, vm.names.clear, clear, 0, default_attributes);
)~~~");
        }
    }

    if (interface.has_unscopable_member) {
        generator.append(R"~~~(
    define_direct_property(vm.well_known_symbol_unscopables(), unscopable_object, JS::Attribute::Configurable);
)~~~");
    }

    generator.append(R"~~~(
    define_direct_property(vm.well_known_symbol_to_string_tag(), JS::PrimitiveString::create(vm, "@namespaced_name@"_string), JS::Attribute::Configurable);
)~~~");

    if (!is_global_interface) {
        generator.append(R"~~~(
    Base::initialize(realm);
)~~~");
    }

    generator.append(R"~~~(
}
)~~~");

    if (!interface.attributes.is_empty() || !interface.functions.is_empty() || interface.has_stringifier) {
        generator.append(R"~~~(
[[maybe_unused]] static JS::ThrowCompletionOr<@fully_qualified_name@*> impl_from(JS::VM& vm)
{
    auto this_value = vm.this_value();
    JS::Object* this_object = nullptr;
    if (this_value.is_nullish())
        this_object = &vm.current_realm()->global_object();
    else
        this_object = TRY(this_value.to_object(vm));
)~~~");

        if (interface.name.is_one_of("EventTarget", "Window")) {
            generator.append(R"~~~(
    if (is<HTML::Window>(this_object)) {
        return static_cast<HTML::Window*>(this_object);
    }
    if (is<HTML::WindowProxy>(this_object)) {
        return static_cast<HTML::WindowProxy*>(this_object)->window().ptr();
    }
)~~~");
        }

        generator.append(R"~~~(
    if (!is<@fully_qualified_name@>(this_object))
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "@namespaced_name@");
    return static_cast<@fully_qualified_name@*>(this_object);
}
)~~~");
    }

    for (auto& attribute : interface.attributes) {
        if (attribute.extended_attributes.contains("FIXME"))
            continue;
        auto attribute_generator = generator.fork();
        attribute_generator.set("attribute.name", attribute.name);
        attribute_generator.set("attribute.getter_callback", attribute.getter_callback_name);
        attribute_generator.set("attribute.setter_callback", attribute.setter_callback_name);

        if (attribute.extended_attributes.contains("ImplementedAs")) {
            auto implemented_as = attribute.extended_attributes.get("ImplementedAs").value();
            attribute_generator.set("attribute.cpp_name", implemented_as);
        } else {
            attribute_generator.set("attribute.cpp_name", attribute.name.to_snakecase());
        }

        if (attribute.extended_attributes.contains("Reflect")) {
            auto attribute_name = attribute.extended_attributes.get("Reflect").value();
            if (attribute_name.is_empty())
                attribute_name = attribute.name;
            attribute_name = make_input_acceptable_cpp(attribute_name);

            attribute_generator.set("attribute.reflect_name", attribute_name);
        } else {
            attribute_generator.set("attribute.reflect_name", attribute.name.to_snakecase());
        }

        // For [CEReactions]: https://html.spec.whatwg.org/multipage/custom-elements.html#cereactions

        attribute_generator.append(R"~~~(
JS_DEFINE_NATIVE_FUNCTION(@class_name@::@attribute.getter_callback@)
{
    WebIDL::log_trace(vm, "@class_name@::@attribute.getter_callback@");
    [[maybe_unused]] auto& realm = *vm.current_realm();
    [[maybe_unused]] auto* impl = TRY(impl_from(vm));
)~~~");
        if (attribute.extended_attributes.contains("CEReactions")) {
            // 1. Push a new element queue onto this object's relevant agent's custom element reactions stack.
            attribute_generator.append(R"~~~(
    auto& relevant_agent = HTML::relevant_agent(*impl);
    auto* custom_data = verify_cast<Bindings::WebEngineCustomData>(relevant_agent.custom_data());
    auto& reactions_stack = custom_data->custom_element_reactions_stack;
    reactions_stack.element_queue_stack.append({});
)~~~");
        }

        // https://html.spec.whatwg.org/multipage/common-dom-interfaces.html#reflecting-content-attributes-in-idl-attributes
        if (attribute.extended_attributes.contains("Reflect")) {
            if (attribute.type->name() == "DOMString") {
                if (!attribute.type->is_nullable()) {
                    // If a reflected IDL attribute has the type DOMString:
                    // * The getter steps are:

                    // 1. Let element be the result of running this's get the element.
                    // NOTE: this is "impl" above

                    // 2. Let contentAttributeValue be the result of running this's get the content attribute.
                    attribute_generator.append(R"~~~(
    auto contentAttributeValue = impl->attribute(HTML::AttributeNames::@attribute.reflect_name@);
)~~~");

                    // 3. Let attributeDefinition be the attribute definition of element's content attribute whose namespace is null
                    //    and local name is the reflected content attribute name.
                    // NOTE: this is "attribute" above

                    // NOTE: We do steps 5 and 6 here to have a field to assign to
                    // 5. If contentAttributeValue is null, then return the empty string.
                    // 6. Return contentAttributeValue.
                    attribute_generator.append(R"~~~(
    auto retval = contentAttributeValue.value_or(String {});
)~~~");

                    // 4. If attributeDefinition indicates it is an enumerated attribute and the reflected IDL attribute is defined to be limited to only known values:
                    if (attribute.extended_attributes.contains("Enumerated")) {
                        auto valid_enumerations_type = attribute.extended_attributes.get("Enumerated").value();
                        auto valid_enumerations = interface.enumerations.get(valid_enumerations_type).value();

                        auto missing_value_default = valid_enumerations.extended_attributes.get("MissingValueDefault");
                        auto invalid_value_default = valid_enumerations.extended_attributes.get("InvalidValueDefault");

                        attribute_generator.set("missing_enum_default_value", missing_value_default.has_value() ? missing_value_default.value().view() : ""sv);
                        attribute_generator.set("invalid_enum_default_value", invalid_value_default.has_value() ? invalid_value_default.value().view() : ""sv);
                        attribute_generator.set("valid_enum_values", MUST(String::join(", "sv, valid_enumerations.values.values(), "\"{}\"_string"sv)));

                        // 1. If contentAttributeValue does not correspond to any state of attributeDefinition (e.g., it is null and there is no missing value default),
                        //    or that it is in a state of attributeDefinition with no associated keyword value, then return the empty string.
                        //    NOTE: @invalid_enum_default_value@ is set to the empty string if it isn't present.
                        attribute_generator.append(R"~~~(
    if (!contentAttributeValue.has_value())
        retval = "@missing_enum_default_value@"_string;

    Array valid_values { @valid_enum_values@ };

    auto found = false;
    for (auto const& value : valid_values) {
        if (value.equals_ignoring_ascii_case(retval)) {
            found = true;
            retval = value;
            break;
        }
    }

    if (!found)
        retval = "@invalid_enum_default_value@"_string;
    )~~~");

                        // 2. Return the canonical keyword for the state of attributeDefinition that contentAttributeValue corresponds to.
                        // NOTE: This is known to be a valid keyword at this point, so we can just return "retval"
                    }
                } else {
                    // If a reflected IDL attribute has the type DOMString?:
                    // * The getter steps are:

                    // 1. Let element be the result of running this's get the element.
                    // NOTE: this is "impl" above

                    // 2. Let contentAttributeValue be the result of running this's get the content attribute.
                    // 8. Return the canonical keyword for the state of attributeDefinition that contentAttributeValue corresponds to.
                    // NOTE: We run step 8 here to have a field to assign to
                    attribute_generator.append(R"~~~(
    auto retval = impl->attribute(HTML::AttributeNames::@attribute.reflect_name@);
)~~~");

                    // 3. Let attributeDefinition be the attribute definition of element's content attribute whose namespace is null
                    //    and local name is the reflected content attribute name.
                    // NOTE: this is "attribute" above

                    // 4. Assert: attributeDefinition indicates it is an enumerated attribute.
                    // 5. Assert: the reflected IDL attribute is limited to only known values.
                    // NOTE: This is checked by the "Enumerated" extended attribute
                    auto is_enumerated = attribute.extended_attributes.contains("Enumerated");
                    VERIFY(is_enumerated);

                    // 6. Assert: contentAttributeValue corresponds to a state of attributeDefinition.
                    auto valid_enumerations_type = attribute.extended_attributes.get("Enumerated").value();
                    auto valid_enumerations = interface.enumerations.get(valid_enumerations_type).value();

                    auto missing_value_default = valid_enumerations.extended_attributes.get("MissingValueDefault");
                    auto invalid_value_default = valid_enumerations.extended_attributes.get("InvalidValueDefault");

                    attribute_generator.set("missing_enum_default_value", missing_value_default.has_value() ? missing_value_default.value().view() : ""sv);
                    attribute_generator.set("invalid_enum_default_value", invalid_value_default.has_value() ? invalid_value_default.value().view() : ""sv);
                    attribute_generator.set("valid_enum_values", MUST(String::join(", "sv, valid_enumerations.values.values(), "\"{}\"_string"sv)));

                    attribute_generator.append(R"~~~(
    Array valid_values { @valid_enum_values@ };
    )~~~");
                    if (invalid_value_default.has_value()) {
                        attribute_generator.append(R"~~~(

    if (retval.has_value()) {
        auto found = false;
        for (auto const& value : valid_values) {
            if (value.equals_ignoring_ascii_case(retval.value())) {
                found = true;
                retval = value;
                break;
            }
        }

        if (!found)
            retval = "@invalid_enum_default_value@"_string;
    }
    )~~~");
                    }

                    if (missing_value_default.has_value()) {
                        attribute_generator.append(R"~~~(
    if (!retval.has_value())
        retval = "@missing_enum_default_value@"_string;
    )~~~");
                    }

                    attribute_generator.append(R"~~~(
    VERIFY(!retval.has_value() || valid_values.contains_slow(retval.value()));
)~~~");
                    // FIXME: 7. If contentAttributeValue corresponds to a state of attributeDefinition with no associated keyword value, then return null.
                }
            }
            // If a reflected IDL attribute has the type boolean:
            else if (attribute.type->name() == "boolean") {
                // The getter steps are:
                // 1. Let contentAttributeValue be the result of running this's get the content attribute.
                // 2. If contentAttributeValue is null, then return false
                attribute_generator.append(R"~~~(
    auto retval = impl->has_attribute(HTML::AttributeNames::@attribute.reflect_name@);
)~~~");
            }
            // If a reflected IDL attribute has the type long:
            else if (attribute.type->name() == "long") {
                // The getter steps are:
                // 1. Let contentAttributeValue be the result of running this's get the content attribute.
                // 2. If contentAttributeValue is not null:
                //    1. Let parsedValue be the result of integer parsing contentAttributeValue if the reflected IDL attribute is not limited to only non-negative numbers;
                //       otherwise the result of non-negative integer parsing contentAttributeValue.
                //    2. If parsedValue is not an error and is within the long range, then return parsedValue.
                attribute_generator.append(R"~~~(
    i32 retval = 0;
    auto content_attribute_value = impl->get_attribute(HTML::AttributeNames::@attribute.reflect_name@);
    if (content_attribute_value.has_value()) {
        auto maybe_parsed_value = Web::HTML::parse_integer(*content_attribute_value);
        if (maybe_parsed_value.has_value())
            retval = *maybe_parsed_value;
    }
)~~~");
            }
            // If a reflected IDL attribute has the type unsigned long,
            // FIXME: optionally limited to only positive numbers, limited to only positive numbers with fallback, or clamped to the range [clampedMin, clampedMax], and optionally with a default value defaultValue:
            else if (attribute.type->name() == "unsigned long") {
                // The getter steps are:
                // 1. Let contentAttributeValue be the result of running this's get the content attribute.
                // 2. Let minimum be 0.
                // FIXME: 3. If the reflected IDL attribute is limited to only positive numbers or limited to only positive numbers with fallback, then set minimum to 1.
                // FIXME: 4. If the reflected IDL attribute is clamped to the range, then set minimum to clampedMin.
                // 5. Let maximum be 2147483647 if the reflected IDL attribute is not clamped to the range; otherwise clampedMax.
                // 6. If contentAttributeValue is not null:
                //    1. Let parsedValue be the result of non-negative integer parsing contentAttributeValue.
                //       2. If parsedValue is not an error and is in the range minimum to maximum, inclusive, then return parsedValue.
                //       FIXME: 3. If parsedValue is not an error and the reflected IDL attribute is clamped to the range:
                //              FIXME: 1. If parsedValue is less than minimum, then return minimum.
                //              FIXME: 2. Return maximum.
                attribute_generator.append(R"~~~(
    u32 retval = 0;
    auto content_attribute_value = impl->get_attribute(HTML::AttributeNames::@attribute.reflect_name@);
    u32 minimum = 0;
    u32 maximum = 2147483647;
    if (content_attribute_value.has_value()) {
        auto parsed_value = Web::HTML::parse_non_negative_integer(*content_attribute_value);
        if (parsed_value.has_value()) {
            if (*parsed_value >= minimum && *parsed_value <= maximum) {
                retval = *parsed_value;
            }
        }
    }
)~~~");
            } else {
                attribute_generator.append(R"~~~(
    auto retval = impl->get_attribute_value(HTML::AttributeNames::@attribute.reflect_name@);
)~~~");
            }

            if (attribute.extended_attributes.contains("CEReactions")) {
                // 2. Run the originally-specified steps for this construct, catching any exceptions. If the steps return a value, let value be the returned value. If they throw an exception, let exception be the thrown exception.
                // 3. Let queue be the result of popping from this object's relevant agent's custom element reactions stack.
                // 4. Invoke custom element reactions in queue.
                // 5. If an exception exception was thrown by the original steps, rethrow exception.
                // 6. If a value value was returned from the original steps, return value.
                attribute_generator.append(R"~~~(
    auto queue = reactions_stack.element_queue_stack.take_last();
    Bindings::invoke_custom_element_reactions(queue);
)~~~");
            }
        } else {
            if (!attribute.extended_attributes.contains("CEReactions")) {
                attribute_generator.append(R"~~~(
    auto retval = TRY(throw_dom_exception_if_needed(vm, [&] { return impl->@attribute.cpp_name@(); }));
)~~~");
            } else {
                // 2. Run the originally-specified steps for this construct, catching any exceptions. If the steps return a value, let value be the returned value. If they throw an exception, let exception be the thrown exception.
                // 3. Let queue be the result of popping from this object's relevant agent's custom element reactions stack.
                // 4. Invoke custom element reactions in queue.
                // 5. If an exception exception was thrown by the original steps, rethrow exception.
                // 6. If a value value was returned from the original steps, return value.
                attribute_generator.append(R"~~~(
    auto retval_or_exception = throw_dom_exception_if_needed(vm, [&] { return impl->@attribute.cpp_name@(); });

    auto queue = reactions_stack.element_queue_stack.take_last();
    Bindings::invoke_custom_element_reactions(queue);

    if (retval_or_exception.is_error())
        return retval_or_exception.release_error();

    auto retval = retval_or_exception.release_value();
)~~~");
            }
        }

        generate_return_statement(generator, *attribute.type, interface);

        attribute_generator.append(R"~~~(
}
)~~~");

        if (!attribute.readonly) {
            // For [CEReactions]: https://html.spec.whatwg.org/multipage/custom-elements.html#cereactions

            attribute_generator.append(R"~~~(
JS_DEFINE_NATIVE_FUNCTION(@class_name@::@attribute.setter_callback@)
{
    WebIDL::log_trace(vm, "@class_name@::@attribute.setter_callback@");
    [[maybe_unused]] auto& realm = *vm.current_realm();
    auto* impl = TRY(impl_from(vm));

    auto value = vm.argument(0);
)~~~");

            if (attribute.extended_attributes.contains("CEReactions")) {
                // 1. Push a new element queue onto this object's relevant agent's custom element reactions stack.
                attribute_generator.append(R"~~~(
    auto& relevant_agent = HTML::relevant_agent(*impl);
    auto* custom_data = verify_cast<Bindings::WebEngineCustomData>(relevant_agent.custom_data());
    auto& reactions_stack = custom_data->custom_element_reactions_stack;
    reactions_stack.element_queue_stack.append({});
)~~~");
            }

            generate_to_cpp(generator, attribute, "value", "", "cpp_value", interface, attribute.extended_attributes.contains("LegacyNullToEmptyString"));

            if (attribute.extended_attributes.contains("Reflect")) {
                if (attribute.type->name() == "boolean") {
                    attribute_generator.append(R"~~~(
    if (!cpp_value)
        impl->remove_attribute(HTML::AttributeNames::@attribute.reflect_name@);
    else
        MUST(impl->set_attribute(HTML::AttributeNames::@attribute.reflect_name@, String {}));
)~~~");
                } else if (attribute.type->is_integer() && !attribute.type->is_nullable()) {
                    attribute_generator.append(R"~~~(
    MUST(impl->set_attribute(HTML::AttributeNames::@attribute.reflect_name@, MUST(String::number(cpp_value))));
)~~~");
                } else if (attribute.type->is_nullable()) {
                    attribute_generator.append(R"~~~(
    if (!cpp_value.has_value())
        impl->remove_attribute(HTML::AttributeNames::@attribute.reflect_name@);
    else
        MUST(impl->set_attribute(HTML::AttributeNames::@attribute.reflect_name@, cpp_value.value()));
)~~~");
                } else {
                    attribute_generator.append(R"~~~(
MUST(impl->set_attribute(HTML::AttributeNames::@attribute.reflect_name@, cpp_value));
)~~~");
                }

                if (attribute.extended_attributes.contains("CEReactions")) {
                    // 2. Run the originally-specified steps for this construct, catching any exceptions. If the steps return a value, let value be the returned value. If they throw an exception, let exception be the thrown exception.
                    // 3. Let queue be the result of popping from this object's relevant agent's custom element reactions stack.
                    // 4. Invoke custom element reactions in queue.
                    // 5. If an exception exception was thrown by the original steps, rethrow exception.
                    // 6. If a value value was returned from the original steps, return value.
                    attribute_generator.append(R"~~~(
    auto queue = reactions_stack.element_queue_stack.take_last();
    Bindings::invoke_custom_element_reactions(queue);
)~~~");
                }
            } else {
                if (!attribute.extended_attributes.contains("CEReactions")) {
                    attribute_generator.append(R"~~~(
    TRY(throw_dom_exception_if_needed(vm, [&] { return impl->set_@attribute.cpp_name@(cpp_value); }));
)~~~");
                } else {
                    // 2. Run the originally-specified steps for this construct, catching any exceptions. If the steps return a value, let value be the returned value. If they throw an exception, let exception be the thrown exception.
                    // 3. Let queue be the result of popping from this object's relevant agent's custom element reactions stack.
                    // 4. Invoke custom element reactions in queue.
                    // 5. If an exception exception was thrown by the original steps, rethrow exception.
                    // 6. If a value value was returned from the original steps, return value.
                    attribute_generator.append(R"~~~(
    auto maybe_exception = throw_dom_exception_if_needed(vm, [&] { return impl->set_@attribute.cpp_name@(cpp_value); });

    auto queue = reactions_stack.element_queue_stack.take_last();
    Bindings::invoke_custom_element_reactions(queue);

    if (maybe_exception.is_error())
        return maybe_exception.release_error();
)~~~");
                }
            }

            attribute_generator.append(R"~~~(
    return JS::js_undefined();
}
)~~~");
        } else if (attribute.extended_attributes.contains("Replaceable"sv)) {
            if (interface.name == "Window"sv) {
                attribute_generator.append(R"~~~(
JS_DEFINE_NATIVE_FUNCTION(@class_name@::@attribute.setter_callback@)
{
    WebIDL::log_trace(vm, "@class_name@::@attribute.setter_callback@");
    auto this_value = vm.this_value();
    JS::GCPtr<Window> window;
    if (this_value.is_object()) {
        if (is<WindowProxy>(this_value.as_object())) {
            auto& window_proxy = static_cast<WindowProxy&>(this_value.as_object());
            window = window_proxy.window();
        } else if (is<Window>(this_value.as_object())) {
            window = &static_cast<Window&>(this_value.as_object());
        }
    }

    if (window) {
        TRY(window->internal_define_own_property("@attribute.name@", JS::PropertyDescriptor { .value = vm.argument(0), .writable = true }));
        return JS::js_undefined();
    }

    return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "@namespaced_name@");
}
)~~~");
            } else {

                attribute_generator.append(R"~~~(
JS_DEFINE_NATIVE_FUNCTION(@class_name@::@attribute.setter_callback@)
{
    WebIDL::log_trace(vm, "@class_name@::@attribute.setter_callback@");
    auto this_value = vm.this_value();
    if (!this_value.is_object() || !is<@fully_qualified_name@>(this_value.as_object()))
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "@namespaced_name@");
    TRY(this_value.as_object().internal_define_own_property("@attribute.name@", JS::PropertyDescriptor { .value = vm.argument(0), .writable = true }));
    return JS::js_undefined();
}
)~~~");
            }
        } else if (auto put_forwards_identifier = attribute.extended_attributes.get("PutForwards"sv); put_forwards_identifier.has_value()) {
            attribute_generator.set("put_forwards_identifier"sv, *put_forwards_identifier);

            attribute_generator.append(R"~~~(
JS_DEFINE_NATIVE_FUNCTION(@class_name@::@attribute.setter_callback@)
{
    WebIDL::log_trace(vm, "@class_name@::@attribute.setter_callback@");
    auto* impl = TRY(impl_from(vm));
    auto value = vm.argument(0);

    auto receiver = TRY(throw_dom_exception_if_needed(vm, [&]() { return impl->@attribute.cpp_name@(); }));
    TRY(receiver->set(JS::PropertyKey { "@put_forwards_identifier@" }, value, JS::Object::ShouldThrowExceptions::Yes));

    return JS::js_undefined();
}
)~~~");
        }
    }

    // Implementation: Functions
    for (auto& function : interface.functions) {
        if (function.extended_attributes.contains("FIXME"))
            continue;
        if (function.extended_attributes.contains("Default")) {
            if (function.name == "toJSON"sv && function.return_type->name() == "object"sv) {
                generate_default_to_json_function(generator, class_name, interface);
                continue;
            }

            dbgln("Unknown default operation: {} {}()", function.return_type->name(), function.name);
            VERIFY_NOT_REACHED();
        }

        generate_function(generator, function, StaticFunction::No, class_name, interface.fully_qualified_name, interface);
    }

    for (auto const& overload_set : interface.overload_sets) {
        if (overload_set.value.size() == 1)
            continue;
        generate_overload_arbiter(generator, overload_set, interface, class_name, IsConstructor::No);
    }

    if (interface.has_stringifier) {
        auto stringifier_generator = generator.fork();
        stringifier_generator.set("class_name", class_name);
        if (interface.stringifier_attribute.has_value())
            stringifier_generator.set("attribute.cpp_getter_name", interface.stringifier_attribute->to_snakecase());

        stringifier_generator.append(R"~~~(
JS_DEFINE_NATIVE_FUNCTION(@class_name@::to_string)
{
    WebIDL::log_trace(vm, "@class_name@::to_string");
    [[maybe_unused]] auto& realm = *vm.current_realm();
    auto* impl = TRY(impl_from(vm));

)~~~");
        if (interface.stringifier_attribute.has_value()) {
            stringifier_generator.append(R"~~~(
    auto retval = TRY(throw_dom_exception_if_needed(vm, [&] { return impl->@attribute.cpp_getter_name@(); }));
)~~~");
        } else {
            stringifier_generator.append(R"~~~(
    auto retval = TRY(throw_dom_exception_if_needed(vm, [&] { return impl->to_string(); }));
)~~~");
        }
        stringifier_generator.append(R"~~~(

    return JS::PrimitiveString::create(vm, move(retval));
}
)~~~");
    }

    if (interface.pair_iterator_types.has_value()) {
        auto iterator_generator = generator.fork();
        iterator_generator.append(R"~~~(
JS_DEFINE_NATIVE_FUNCTION(@class_name@::entries)
{
    WebIDL::log_trace(vm, "@class_name@::entries");
    auto* impl = TRY(impl_from(vm));

    return TRY(throw_dom_exception_if_needed(vm, [&] { return @iterator_name@::create(*impl, Object::PropertyKind::KeyAndValue); }));
}

JS_DEFINE_NATIVE_FUNCTION(@class_name@::for_each)
{
    WebIDL::log_trace(vm, "@class_name@::for_each");
    [[maybe_unused]] auto& realm = *vm.current_realm();
    auto* impl = TRY(impl_from(vm));

    auto callback = vm.argument(0);
    if (!callback.is_function())
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAFunction, callback.to_string_without_side_effects());

    auto this_value = vm.this_value();
    TRY(impl->for_each([&](auto key, auto value) -> JS::ThrowCompletionOr<void> {
)~~~");
        generate_variable_statement(iterator_generator, "wrapped_key", interface.pair_iterator_types->get<0>(), "key", interface);
        generate_variable_statement(iterator_generator, "wrapped_value", interface.pair_iterator_types->get<1>(), "value", interface);
        iterator_generator.append(R"~~~(
        TRY(call(vm, callback.as_function(), vm.argument(1), wrapped_value, wrapped_key, this_value));
        return {};
    }));

    return JS::js_undefined();
}

JS_DEFINE_NATIVE_FUNCTION(@class_name@::keys)
{
    WebIDL::log_trace(vm, "@class_name@::keys");
    auto* impl = TRY(impl_from(vm));

    return TRY(throw_dom_exception_if_needed(vm, [&] { return @iterator_name@::create(*impl, Object::PropertyKind::Key);  }));
}

JS_DEFINE_NATIVE_FUNCTION(@class_name@::values)
{
    WebIDL::log_trace(vm, "@class_name@::values");
    auto* impl = TRY(impl_from(vm));

    return TRY(throw_dom_exception_if_needed(vm, [&] { return @iterator_name@::create(*impl, Object::PropertyKind::Value); }));
}
)~~~");
    }

    if (interface.set_entry_type.has_value()) {
        auto setlike_generator = generator.fork();
        setlike_generator.set("value_type", interface.set_entry_type.value()->name());
        setlike_generator.append(R"~~~(
// https://webidl.spec.whatwg.org/#js-set-size
JS_DEFINE_NATIVE_FUNCTION(@class_name@::get_size)
{
    WebIDL::log_trace(vm, "@class_name@::size");
    auto* impl = TRY(impl_from(vm));

    JS::NonnullGCPtr<JS::Set> set = impl->set_entries();

    return set->set_size();
}

// https://webidl.spec.whatwg.org/#js-set-entries
JS_DEFINE_NATIVE_FUNCTION(@class_name@::entries)
{
    WebIDL::log_trace(vm, "@class_name@::entries");
    auto& realm = *vm.current_realm();
    auto* impl = TRY(impl_from(vm));

    JS::NonnullGCPtr<JS::Set> set = impl->set_entries();

    return TRY(throw_dom_exception_if_needed(vm, [&] { return JS::SetIterator::create(realm, *set, Object::PropertyKind::KeyAndValue); }));
}

// https://webidl.spec.whatwg.org/#js-set-values
JS_DEFINE_NATIVE_FUNCTION(@class_name@::values)
{
    WebIDL::log_trace(vm, "@class_name@::values");
    auto& realm = *vm.current_realm();
    auto* impl = TRY(impl_from(vm));

    JS::NonnullGCPtr<JS::Set> set = impl->set_entries();

    return TRY(throw_dom_exception_if_needed(vm, [&] { return JS::SetIterator::create(realm, *set, Object::PropertyKind::Value); }));
}

// https://webidl.spec.whatwg.org/#js-set-forEach
JS_DEFINE_NATIVE_FUNCTION(@class_name@::for_each)
{
    WebIDL::log_trace(vm, "@class_name@::for_each");
    auto* impl = TRY(impl_from(vm));

    JS::NonnullGCPtr<JS::Set> set = impl->set_entries();

    auto callback = vm.argument(0);
    if (!callback.is_function())
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAFunction, callback.to_string_without_side_effects());

    for (auto& entry : *set) {
        auto value = entry.key;
        TRY(call(vm, callback.as_function(), vm.argument(1), value, value, impl));
    }

    return JS::js_undefined();
}

// https://webidl.spec.whatwg.org/#js-set-has
JS_DEFINE_NATIVE_FUNCTION(@class_name@::has)
{
    WebIDL::log_trace(vm, "@class_name@::has");
    auto* impl = TRY(impl_from(vm));

    JS::NonnullGCPtr<JS::Set> set = impl->set_entries();

    auto value_arg = vm.argument(0);
    if (!value_arg.is_object() && !is<@value_type@>(value_arg.as_object())) {
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "@value_type@");
    }

    // FIXME: If value is -0, set value to +0.
    // What? Which interfaces have a number as their set type?

    return set->set_has(value_arg);
}
)~~~");

        if (!interface.overload_sets.contains("add"sv) && !interface.is_set_readonly) {
            setlike_generator.append(R"~~~(
// https://webidl.spec.whatwg.org/#js-set-add
JS_DEFINE_NATIVE_FUNCTION(@class_name@::add)
{
    WebIDL::log_trace(vm, "@class_name@::add");
    auto* impl = TRY(impl_from(vm));

    JS::NonnullGCPtr<JS::Set> set = impl->set_entries();

    auto value_arg = vm.argument(0);
    if (!value_arg.is_object() && !is<@value_type@>(value_arg.as_object())) {
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "@value_type@");
    }

    // FIXME: If value is -0, set value to +0.
    // What? Which interfaces have a number as their set type?

    set->set_add(value_arg);

    return impl;
}
)~~~");
        }
        if (!interface.overload_sets.contains("delete"sv) && !interface.is_set_readonly) {
            setlike_generator.append(R"~~~(
// https://webidl.spec.whatwg.org/#js-set-delete
JS_DEFINE_NATIVE_FUNCTION(@class_name@::delete_)
{
    WebIDL::log_trace(vm, "@class_name@::delete_");
    auto* impl = TRY(impl_from(vm));

    JS::NonnullGCPtr<JS::Set> set = impl->set_entries();

    auto value_arg = vm.argument(0);
    if (!value_arg.is_object() && !is<@value_type@>(value_arg.as_object())) {
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "@value_type@");
    }

    // FIXME: If value is -0, set value to +0.
    // What? Which interfaces have a number as their set type?

    return set->set_remove(value_arg);
}
)~~~");
        }
        if (!interface.overload_sets.contains("clear"sv) && !interface.is_set_readonly) {
            setlike_generator.append(R"~~~(
// https://webidl.spec.whatwg.org/#js-set-clear
JS_DEFINE_NATIVE_FUNCTION(@class_name@::clear)
{
    WebIDL::log_trace(vm, "@class_name@::clear");
    auto* impl = TRY(impl_from(vm));

    JS::NonnullGCPtr<JS::Set> set = impl->set_entries();

    set->set_clear();

    return JS::js_undefined();
}
)~~~");
        }
    }
}

void generate_namespace_header(IDL::Interface const& interface, StringBuilder& builder)
{
    SourceGenerator generator { builder };

    generator.set("namespace_class", interface.namespace_class);

    generator.append(R"~~~(
#pragma once

#include <LibJS/Runtime/Object.h>

namespace Web::Bindings {

class @namespace_class@ final : public JS::Object {
    JS_OBJECT(@namespace_class@, JS::Object);
    JS_DECLARE_ALLOCATOR(@namespace_class@);
public:
    explicit @namespace_class@(JS::Realm&);
    virtual void initialize(JS::Realm&) override;
    virtual ~@namespace_class@() override;

private:
)~~~");

    if (interface.extended_attributes.contains("WithGCVisitor"sv)) {
        generator.append(R"~~~(
    virtual void visit_edges(JS::Cell::Visitor&) override;
)~~~");
    }

    if (interface.extended_attributes.contains("WithFinalizer"sv)) {
        generator.append(R"~~~(
    virtual void finalize() override;
)~~~");
    }

    for (auto const& overload_set : interface.overload_sets) {
        auto function_generator = generator.fork();
        function_generator.set("function.name:snakecase", make_input_acceptable_cpp(overload_set.key.to_snakecase()));
        function_generator.append(R"~~~(
    JS_DECLARE_NATIVE_FUNCTION(@function.name:snakecase@);
)~~~");
        if (overload_set.value.size() > 1) {
            for (auto i = 0u; i < overload_set.value.size(); ++i) {
                function_generator.set("overload_suffix", ByteString::number(i));
                function_generator.append(R"~~~(
    JS_DECLARE_NATIVE_FUNCTION(@function.name:snakecase@@overload_suffix@);
)~~~");
            }
        }
    }

    generator.append(R"~~~(
};

} // namespace Web::Bindings
)~~~");
}

static void generate_using_namespace_definitions(SourceGenerator& generator)
{
    generator.append(R"~~~(
    // FIXME: This is a total hack until we can figure out the namespace for a given type somehow.
    using namespace Web::Animations;
    using namespace Web::CSS;
    using namespace Web::DOM;
    using namespace Web::Crypto;
    using namespace Web::DOMParsing;
    using namespace Web::DOMURL;
    using namespace Web::Encoding;
    using namespace Web::Fetch;
    using namespace Web::FileAPI;
    using namespace Web::Geometry;
    using namespace Web::HighResolutionTime;
    using namespace Web::HTML;
    using namespace Web::IndexedDB;
    using namespace Web::Internals;
    using namespace Web::IntersectionObserver;
    using namespace Web::RequestIdleCallback;
    using namespace Web::ResizeObserver;
    using namespace Web::Selection;
    using namespace Web::NavigationTiming;
    using namespace Web::PerformanceTimeline;
    using namespace Web::UserTiming;
    using namespace Web::Streams;
    using namespace Web::SVG;
    using namespace Web::UIEvents;
    using namespace Web::XHR;
    using namespace Web::WebAssembly;
    using namespace Web::WebAudio;
    using namespace Web::WebGL;
    using namespace Web::WebIDL;
)~~~"sv);
}

void generate_namespace_implementation(IDL::Interface const& interface, StringBuilder& builder)
{
    SourceGenerator generator { builder };

    generator.set("name", interface.name);
    generator.set("namespace_class", interface.namespace_class);
    generator.set("interface_name", interface.name);

    generator.append(R"~~~(
#include <AK/Function.h>
#include <LibIDL/Types.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/DataView.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/Value.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibWeb/Bindings/@namespace_class@.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/OverloadResolution.h>
#include <LibWeb/WebIDL/Tracing.h>
#include <LibWeb/WebIDL/Types.h>

)~~~");

    emit_includes_for_all_imports(interface, generator, interface.pair_iterator_types.has_value());

    generate_using_namespace_definitions(generator);

    generator.append(R"~~~(
namespace Web::Bindings {

JS_DEFINE_ALLOCATOR(@namespace_class@);

@namespace_class@::@namespace_class@(JS::Realm& realm)
    : Object(ConstructWithPrototypeTag::Tag, realm.intrinsics().object_prototype())
{
}

@namespace_class@::~@namespace_class@()
{
}

void @namespace_class@::initialize(JS::Realm& realm)
{
    [[maybe_unused]] auto& vm = this->vm();
    [[maybe_unused]] u8 default_attributes = JS::Attribute::Enumerable;

    Base::initialize(realm);

    define_direct_property(vm.well_known_symbol_to_string_tag(), JS::PrimitiveString::create(vm, "@interface_name@"_string), JS::Attribute::Configurable);

)~~~");

    // https://webidl.spec.whatwg.org/#es-operations
    for (auto const& overload_set : interface.overload_sets) {
        auto function_generator = generator.fork();
        function_generator.set("function.name", overload_set.key);
        function_generator.set("function.name:snakecase", make_input_acceptable_cpp(overload_set.key.to_snakecase()));
        function_generator.set("function.length", ByteString::number(get_shortest_function_length(overload_set.value)));

        function_generator.append(R"~~~(
    define_native_function(realm, "@function.name@", @function.name:snakecase@, @function.length@, default_attributes);
)~~~");
    }

    generator.append(R"~~~(
}
)~~~");

    if (interface.extended_attributes.contains("WithGCVisitor"sv)) {
        generator.append(R"~~~(
void @namespace_class@::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    @name@::visit_edges(*this, visitor);
}
)~~~");
    }

    if (interface.extended_attributes.contains("WithFinalizer"sv)) {
        generator.append(R"~~~(
void @namespace_class@::finalize()
{
    @name@::finalize(*this);
}
)~~~");
    }

    for (auto const& function : interface.functions) {
        if (function.extended_attributes.contains("FIXME"))
            continue;
        generate_function(generator, function, StaticFunction::Yes, interface.namespace_class, interface.name, interface);
    }
    for (auto const& overload_set : interface.overload_sets) {
        if (overload_set.value.size() == 1)
            continue;
        generate_overload_arbiter(generator, overload_set, interface, interface.namespace_class, IsConstructor::No);
    }

    generator.append(R"~~~(
} // namespace Web::Bindings
)~~~");
}

void generate_constructor_header(IDL::Interface const& interface, StringBuilder& builder)
{
    SourceGenerator generator { builder };

    generator.set("constructor_class", interface.constructor_class);

    generator.append(R"~~~(
#pragma once

#include <LibJS/Runtime/NativeFunction.h>

namespace Web::Bindings {

class @constructor_class@ : public JS::NativeFunction {
    JS_OBJECT(@constructor_class@, JS::NativeFunction);
    JS_DECLARE_ALLOCATOR(@constructor_class@);
public:
    explicit @constructor_class@(JS::Realm&);
    virtual void initialize(JS::Realm&) override;
    virtual ~@constructor_class@() override;

    virtual JS::ThrowCompletionOr<JS::Value> call() override;
    virtual JS::ThrowCompletionOr<JS::NonnullGCPtr<JS::Object>> construct(JS::FunctionObject& new_target) override;

private:
    virtual bool has_constructor() const override { return true; }
)~~~");

    for (auto& attribute : interface.static_attributes) {
        auto attribute_generator = generator.fork();
        attribute_generator.set("attribute.name:snakecase", attribute.name.to_snakecase());
        attribute_generator.append(R"~~~(
    JS_DECLARE_NATIVE_FUNCTION(@attribute.name:snakecase@_getter);
)~~~");

        if (!attribute.readonly) {
            attribute_generator.append(R"~~~(
    JS_DECLARE_NATIVE_FUNCTION(@attribute.name:snakecase@_setter);
)~~~");
        }
    }

    for (auto const& overload_set : interface.constructor_overload_sets) {
        auto constructor_generator = generator.fork();
        if (overload_set.value.size() > 1) {
            for (auto i = 0u; i < overload_set.value.size(); ++i) {
                constructor_generator.set("overload_suffix", ByteString::number(i));
                constructor_generator.append(R"~~~(
    JS::ThrowCompletionOr<JS::NonnullGCPtr<JS::Object>> construct@overload_suffix@(JS::FunctionObject& new_target);
)~~~");
            }
        }
    }

    for (auto const& overload_set : interface.static_overload_sets) {
        auto function_generator = generator.fork();
        function_generator.set("function.name:snakecase", make_input_acceptable_cpp(overload_set.key.to_snakecase()));
        function_generator.append(R"~~~(
    JS_DECLARE_NATIVE_FUNCTION(@function.name:snakecase@);
)~~~");
        if (overload_set.value.size() > 1) {
            for (auto i = 0u; i < overload_set.value.size(); ++i) {
                function_generator.set("overload_suffix", ByteString::number(i));
                function_generator.append(R"~~~(
    JS_DECLARE_NATIVE_FUNCTION(@function.name:snakecase@@overload_suffix@);
)~~~");
            }
        }
    }

    generator.append(R"~~~(
};

} // namespace Web::Bindings
)~~~");
}

void generate_constructor_implementation(IDL::Interface const& interface, StringBuilder& builder)
{
    SourceGenerator generator { builder };

    generator.set("name", interface.name);
    generator.set("namespaced_name", interface.namespaced_name);
    generator.set("prototype_class", interface.prototype_class);
    generator.set("constructor_class", interface.constructor_class);
    generator.set("fully_qualified_name", interface.fully_qualified_name);

    generator.append(R"~~~(
#include <LibIDL/Types.h>
#include <LibJS/Heap/Heap.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/DataView.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/@constructor_class@.h>
#include <LibWeb/Bindings/@prototype_class@.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Intrinsics.h>
#if __has_include(<LibWeb/Animations/@name@.h>)
#    include <LibWeb/Animations/@name@.h>
#elif __has_include(<LibWeb/Crypto/@name@.h>)
#    include <LibWeb/Crypto/@name@.h>
#elif __has_include(<LibWeb/CSS/@name@.h>)
#    include <LibWeb/CSS/@name@.h>
#elif __has_include(<LibWeb/DOM/@name@.h>)
#    include <LibWeb/DOM/@name@.h>
#elif __has_include(<LibWeb/Encoding/@name@.h>)
#    include <LibWeb/Encoding/@name@.h>
#elif __has_include(<LibWeb/Fetch/@name@.h>)
#    include <LibWeb/Fetch/@name@.h>
#elif __has_include(<LibWeb/FileAPI/@name@.h>)
#    include <LibWeb/FileAPI/@name@.h>
#elif __has_include(<LibWeb/Geometry/@name@.h>)
#    include <LibWeb/Geometry/@name@.h>
#elif __has_include(<LibWeb/HTML/@name@.h>)
#    include <LibWeb/HTML/@name@.h>
#elif __has_include(<LibWeb/UIEvents/@name@.h>)
#    include <LibWeb/UIEvents/@name@.h>
#elif __has_include(<LibWeb/HighResolutionTime/@name@.h>)
#    include <LibWeb/HighResolutionTime/@name@.h>
#elif __has_include(<LibWeb/IntersectionObserver/@name@.h>)
#    include <LibWeb/IntersectionObserver/@name@.h>
#elif __has_include(<LibWeb/NavigationTiming/@name@.h>)
#    include <LibWeb/NavigationTiming/@name@.h>
#elif __has_include(<LibWeb/RequestIdleCallback/@name@.h>)
#    include <LibWeb/RequestIdleCallback/@name@.h>
#elif __has_include(<LibWeb/ResizeObserver/@name@.h>)
#    include <LibWeb/ResizeObserver/@name@.h>
#elif __has_include(<LibWeb/SVG/@name@.h>)
#    include <LibWeb/SVG/@name@.h>
#elif __has_include(<LibWeb/Selection/@name@.h>)
#    include <LibWeb/Selection/@name@.h>
#elif __has_include(<LibWeb/WebSockets/@name@.h>)
#    include <LibWeb/WebSockets/@name@.h>
#elif __has_include(<LibWeb/XHR/@name@.h>)
#    include <LibWeb/XHR/@name@.h>
#elif __has_include(<LibWeb/DOMURL/@name@.h>)
#    include <LibWeb/DOMURL/@name@.h>
#endif
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWeb/WebIDL/OverloadResolution.h>
#include <LibWeb/WebIDL/Tracing.h>
#include <LibWeb/WebIDL/Types.h>

)~~~");

    if (interface.constructors.size() == 1) {
        auto& constructor = interface.constructors[0];
        if (constructor.extended_attributes.contains("HTMLConstructor"sv)) {
            generator.append(R"~~~(
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/HTML/CustomElements/CustomElementRegistry.h>
#include <LibWeb/HTML/CustomElements/CustomElementDefinition.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Namespace.h>
)~~~");
        }
    }

    emit_includes_for_all_imports(interface, generator, interface.pair_iterator_types.has_value());

    generate_using_namespace_definitions(generator);

    generator.append(R"~~~(
namespace Web::Bindings {

JS_DEFINE_ALLOCATOR(@constructor_class@);

@constructor_class@::@constructor_class@(JS::Realm& realm)
    : NativeFunction("@name@"sv, realm.intrinsics().function_prototype())
{
}

@constructor_class@::~@constructor_class@()
{
}

JS::ThrowCompletionOr<JS::Value> @constructor_class@::call()
{
    return vm().throw_completion<JS::TypeError>(JS::ErrorType::ConstructorWithoutNew, "@namespaced_name@");
}

)~~~");

    generate_constructors(generator, interface);

    generator.append(R"~~~(

void @constructor_class@::initialize(JS::Realm& realm)
{
    auto& vm = this->vm();
    [[maybe_unused]] u8 default_attributes = JS::Attribute::Enumerable;

    Base::initialize(realm);
    define_direct_property(vm.names.prototype, &ensure_web_prototype<@prototype_class@>(realm, "@namespaced_name@"_fly_string), 0);
    define_direct_property(vm.names.length, JS::Value(@constructor.length@), JS::Attribute::Configurable);

)~~~");

    for (auto& constant : interface.constants) {
        auto constant_generator = generator.fork();
        constant_generator.set("constant.name", constant.name);

        generate_wrap_statement(constant_generator, constant.value, constant.type, interface, ByteString::formatted("auto constant_{}_value =", constant.name));

        constant_generator.append(R"~~~(
    define_direct_property("@constant.name@", constant_@constant.name@_value, JS::Attribute::Enumerable);
)~~~");
    }

    for (auto& attribute : interface.static_attributes) {
        auto attribute_generator = generator.fork();
        attribute_generator.set("attribute.name", attribute.name);
        attribute_generator.set("attribute.getter_callback", attribute.getter_callback_name);
        attribute_generator.set("attribute.getter_callback", attribute.getter_callback_name);

        if (!attribute.readonly)
            attribute_generator.set("attribute.setter_callback", attribute.setter_callback_name);
        else
            attribute_generator.set("attribute.setter_callback", "nullptr");

        attribute_generator.append(R"~~~(
    define_native_accessor(realm, "@attribute.name@", @attribute.getter_callback@, @attribute.setter_callback@, default_attributes);
)~~~");
    }

    // https://webidl.spec.whatwg.org/#es-operations
    for (auto const& overload_set : interface.static_overload_sets) {
        auto function_generator = generator.fork();
        function_generator.set("function.name", overload_set.key);
        function_generator.set("function.name:snakecase", make_input_acceptable_cpp(overload_set.key.to_snakecase()));
        function_generator.set("function.length", ByteString::number(get_shortest_function_length(overload_set.value)));

        function_generator.append(R"~~~(
    define_native_function(realm, "@function.name@", @function.name:snakecase@, @function.length@, default_attributes);
)~~~");
    }

    generator.append(R"~~~(
}
)~~~");

    // Implementation: Static Attributes
    for (auto& attribute : interface.static_attributes) {
        auto attribute_generator = generator.fork();
        attribute_generator.set("attribute.name", attribute.name);
        attribute_generator.set("attribute.getter_callback", attribute.getter_callback_name);
        attribute_generator.set("attribute.setter_callback", attribute.setter_callback_name);

        if (attribute.extended_attributes.contains("ImplementedAs")) {
            auto implemented_as = attribute.extended_attributes.get("ImplementedAs").value();
            attribute_generator.set("attribute.cpp_name", implemented_as);
        } else {
            attribute_generator.set("attribute.cpp_name", attribute.name.to_snakecase());
        }

        attribute_generator.append(R"~~~(
JS_DEFINE_NATIVE_FUNCTION(@constructor_class@::@attribute.getter_callback@)
{
    WebIDL::log_trace(vm, "@constructor_class@::@attribute.getter_callback@");
    auto retval = TRY(throw_dom_exception_if_needed(vm, [&] { return @fully_qualified_name@::@attribute.cpp_name@(vm); }));
)~~~");

        generate_return_statement(generator, *attribute.type, interface);

        attribute_generator.append(R"~~~(
}
)~~~");

        // FIXME: Add support for static attribute setters.
    }

    // Implementation: Static Functions
    for (auto& function : interface.static_functions) {
        if (function.extended_attributes.contains("FIXME"))
            continue;
        generate_function(generator, function, StaticFunction::Yes, interface.constructor_class, interface.fully_qualified_name, interface);
    }
    for (auto const& overload_set : interface.static_overload_sets) {
        if (overload_set.value.size() == 1)
            continue;
        generate_overload_arbiter(generator, overload_set, interface, interface.constructor_class, IsConstructor::No);
    }

    generator.append(R"~~~(
} // namespace Web::Bindings
)~~~");
}

void generate_prototype_header(IDL::Interface const& interface, StringBuilder& builder)
{
    SourceGenerator generator { builder };

    generator.set("prototype_class", interface.prototype_class);

    generator.append(R"~~~(
#pragma once

#include <LibJS/Runtime/Object.h>

namespace Web::Bindings {

class @prototype_class@ : public JS::Object {
    JS_OBJECT(@prototype_class@, JS::Object);
    JS_DECLARE_ALLOCATOR(@prototype_class@);
public:
    explicit @prototype_class@(JS::Realm&);
    virtual void initialize(JS::Realm&) override;
    virtual ~@prototype_class@() override;
private:
)~~~");

    // Generate an empty prototype object for global interfaces.
    auto is_global_interface = interface.extended_attributes.contains("Global");
    if (is_global_interface) {
        generator.append(R"~~~(
};
)~~~");
        if (interface.supports_named_properties()) {
            generate_named_properties_object_declarations(interface, builder);
        }
    } else {
        generate_prototype_or_global_mixin_declarations(interface, builder);
    }

    generator.append(R"~~~(
} // namespace Web::Bindings
    )~~~");
}

void generate_prototype_implementation(IDL::Interface const& interface, StringBuilder& builder)
{
    SourceGenerator generator { builder };

    generator.set("parent_name", interface.parent_name);
    generator.set("prototype_class", interface.prototype_class);
    generator.set("prototype_base_class", interface.prototype_base_class);

    generator.append(R"~~~(
#include <AK/Function.h>
#include <LibIDL/Types.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/DataView.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/Value.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibWeb/Bindings/@prototype_class@.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/IDLEventListener.h>
#include <LibWeb/DOM/NodeFilter.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/HTML/Numbers.h>
#include <LibWeb/HTML/Origin.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/Tracing.h>
#include <LibWeb/WebIDL/OverloadResolution.h>
#include <LibWeb/WebIDL/Types.h>

#if __has_include(<LibWeb/Bindings/@prototype_base_class@.h>)
#    include <LibWeb/Bindings/@prototype_base_class@.h>
#endif

)~~~");

    bool has_ce_reactions = false;
    for (auto const& function : interface.functions) {
        if (function.extended_attributes.contains("FIXME"))
            continue;
        if (function.extended_attributes.contains("CEReactions")) {
            has_ce_reactions = true;
            break;
        }
    }

    if (!has_ce_reactions) {
        for (auto const& attribute : interface.attributes) {
            if (attribute.extended_attributes.contains("CEReactions")) {
                has_ce_reactions = true;
                break;
            }
        }
    }

    if (!has_ce_reactions && interface.indexed_property_setter.has_value() && interface.indexed_property_setter->extended_attributes.contains("CEReactions"))
        has_ce_reactions = true;

    if (!has_ce_reactions && interface.named_property_setter.has_value() && interface.named_property_setter->extended_attributes.contains("CEReactions"))
        has_ce_reactions = true;

    if (!has_ce_reactions && interface.named_property_deleter.has_value() && interface.named_property_deleter->extended_attributes.contains("CEReactions"))
        has_ce_reactions = true;

    if (has_ce_reactions) {
        generator.append(R"~~~(
#include <LibWeb/Bindings/MainThreadVM.h>
)~~~");
    }

    emit_includes_for_all_imports(interface, generator, interface.pair_iterator_types.has_value());

    generate_using_namespace_definitions(generator);

    generator.append(R"~~~(
namespace Web::Bindings {

JS_DEFINE_ALLOCATOR(@prototype_class@);

@prototype_class@::@prototype_class@([[maybe_unused]] JS::Realm& realm))~~~");
    if (interface.name == "DOMException") {
        // https://webidl.spec.whatwg.org/#es-DOMException-specialness
        // Object.getPrototypeOf(DOMException.prototype) === Error.prototype
        generator.append(R"~~~(
    : Object(ConstructWithPrototypeTag::Tag, realm.intrinsics().error_prototype())
)~~~");
    } else if (!interface.parent_name.is_empty()) {
        generator.append(R"~~~(
    : Object(realm, nullptr)
)~~~");
    } else {
        generator.append(R"~~~(
    : Object(ConstructWithPrototypeTag::Tag, realm.intrinsics().object_prototype())
)~~~");
    }

    generator.append(R"~~~(
{
}

@prototype_class@::~@prototype_class@()
{
}
)~~~");

    // Generate a mostly empty prototype object for global interfaces.
    auto is_global_interface = interface.extended_attributes.contains("Global");
    if (is_global_interface) {
        generator.append(R"~~~(
void @prototype_class@::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
)~~~");
        if (interface.supports_named_properties()) {
            generator.set("named_properties_class", ByteString::formatted("{}Properties", interface.name));
            generator.set("namespaced_name", interface.namespaced_name);
            generator.append(R"~~~(
    define_direct_property(vm().well_known_symbol_to_string_tag(), JS::PrimitiveString::create(vm(), "@namespaced_name@"_string), JS::Attribute::Configurable);
    set_prototype(&ensure_web_prototype<@prototype_class@>(realm, "@named_properties_class@"_fly_string));
)~~~");
        }
        generator.append(R"~~~(
}
)~~~");
        if (interface.supports_named_properties())
            generate_named_properties_object_definitions(interface, builder);
    } else {
        generate_prototype_or_global_mixin_definitions(interface, builder);
    }

    generator.append(R"~~~(
} // namespace Web::Bindings
)~~~");
}

void generate_iterator_prototype_header(IDL::Interface const& interface, StringBuilder& builder)
{
    VERIFY(interface.pair_iterator_types.has_value());
    SourceGenerator generator { builder };

    generator.set("prototype_class", ByteString::formatted("{}IteratorPrototype", interface.name));

    generator.append(R"~~~(
#pragma once

#include <LibJS/Runtime/Object.h>

namespace Web::Bindings {

class @prototype_class@ : public JS::Object {
    JS_OBJECT(@prototype_class@, JS::Object);
    JS_DECLARE_ALLOCATOR(@prototype_class@);
public:
    explicit @prototype_class@(JS::Realm&);
    virtual void initialize(JS::Realm&) override;
    virtual ~@prototype_class@() override;

private:
    JS_DECLARE_NATIVE_FUNCTION(next);
};

} // namespace Web::Bindings
    )~~~");
}

void generate_iterator_prototype_implementation(IDL::Interface const& interface, StringBuilder& builder)
{
    VERIFY(interface.pair_iterator_types.has_value());
    SourceGenerator generator { builder };

    generator.set("name", ByteString::formatted("{}Iterator", interface.name));
    generator.set("parent_name", interface.parent_name);
    generator.set("prototype_class", ByteString::formatted("{}IteratorPrototype", interface.name));
    generator.set("prototype_base_class", interface.prototype_base_class);
    generator.set("fully_qualified_name", ByteString::formatted("{}Iterator", interface.fully_qualified_name));
    generator.set("possible_include_path", ByteString::formatted("{}Iterator", interface.name.replace("::"sv, "/"sv, ReplaceMode::All)));

    generator.append(R"~~~(
#include <AK/Function.h>
#include <AK/TypeCasts.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/@prototype_class@.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebIDL/Tracing.h>

#if __has_include(<LibWeb/@possible_include_path@.h>)
#    include <LibWeb/@possible_include_path@.h>
#endif
)~~~");

    emit_includes_for_all_imports(interface, generator, true);

    generate_using_namespace_definitions(generator);

    generator.append(R"~~~(
namespace Web::Bindings {

JS_DEFINE_ALLOCATOR(@prototype_class@);

@prototype_class@::@prototype_class@(JS::Realm& realm)
    : Object(ConstructWithPrototypeTag::Tag, realm.intrinsics().iterator_prototype())
{
}

@prototype_class@::~@prototype_class@()
{
}

void @prototype_class@::initialize(JS::Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);
    define_native_function(realm, vm.names.next, next, 0, JS::Attribute::Writable | JS::Attribute::Enumerable | JS::Attribute::Configurable);
    define_direct_property(vm.well_known_symbol_to_string_tag(), JS::PrimitiveString::create(vm, "Iterator"_string), JS::Attribute::Configurable);
}

static JS::ThrowCompletionOr<@fully_qualified_name@*> impl_from(JS::VM& vm)
{
    auto this_object = TRY(vm.this_value().to_object(vm));
    if (!is<@fully_qualified_name@>(*this_object))
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "@name@");
    return static_cast<@fully_qualified_name@*>(this_object.ptr());
}

JS_DEFINE_NATIVE_FUNCTION(@prototype_class@::next)
{
    WebIDL::log_trace(vm, "@prototype_class@::next");
    auto* impl = TRY(impl_from(vm));
    return TRY(throw_dom_exception_if_needed(vm, [&] { return impl->next(); }));
}

} // namespace Web::Bindings
)~~~");
}

void generate_global_mixin_header(IDL::Interface const& interface, StringBuilder& builder)
{
    SourceGenerator generator { builder };

    generator.set("class_name", interface.global_mixin_class);

    generator.append(R"~~~(
#pragma once

#include <LibJS/Runtime/Object.h>

namespace Web::Bindings {

class @class_name@ {
public:
    void initialize(JS::Realm&, JS::Object&);
    @class_name@();
    virtual ~@class_name@();

private:
)~~~");

    generate_prototype_or_global_mixin_declarations(interface, builder);

    generator.append(R"~~~(
} // namespace Web::Bindings
    )~~~");
}

void generate_global_mixin_implementation(IDL::Interface const& interface, StringBuilder& builder)
{
    SourceGenerator generator { builder };

    generator.set("class_name", interface.global_mixin_class);
    generator.set("prototype_name", interface.prototype_class);

    generator.append(R"~~~(
#include <AK/Function.h>
#include <LibIDL/Types.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/DataView.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/Value.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibWeb/Bindings/@class_name@.h>
#include <LibWeb/Bindings/@prototype_name@.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/IDLEventListener.h>
#include <LibWeb/DOM/NodeFilter.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/HTML/Origin.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/WebIDL/AbstractOperations.h>
#include <LibWeb/WebIDL/OverloadResolution.h>
#include <LibWeb/WebIDL/Tracing.h>
#include <LibWeb/WebIDL/Types.h>

)~~~");

    emit_includes_for_all_imports(interface, generator, interface.pair_iterator_types.has_value());

    generate_using_namespace_definitions(generator);

    generator.append(R"~~~(
namespace Web::Bindings {

@class_name@::@class_name@() = default;
@class_name@::~@class_name@() = default;
)~~~");

    generate_prototype_or_global_mixin_definitions(interface, builder);

    generator.append(R"~~~(
} // namespace Web::Bindings
    )~~~");
}
}
