/*
 * Copyright (c) 2022, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/LexicalPath.h>
#include <AK/SourceGenerator.h>
#include <AK/StringBuilder.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/File.h>
#include <LibCore/MappedFile.h>
#include <LibIDL/ExposedTo.h>
#include <LibIDL/IDLParser.h>
#include <LibIDL/Types.h>
#include <LibMain/Main.h>

struct InterfaceSets {
    Vector<IDL::Interface&> intrinsics;
    Vector<IDL::Interface&> window_exposed;
    Vector<IDL::Interface&> dedicated_worker_exposed;
    Vector<IDL::Interface&> shared_worker_exposed;
    Vector<IDL::Interface&> shadow_realm_exposed;
    // TODO: service_worker_exposed
};

static ErrorOr<void> add_to_interface_sets(IDL::Interface&, InterfaceSets& interface_sets);
static ByteString s_error_string;

struct LegacyConstructor {
    ByteString name;
    ByteString constructor_class;
};

static void consume_whitespace(GenericLexer& lexer)
{
    bool consumed = true;
    while (consumed) {
        consumed = lexer.consume_while(is_ascii_space).length() > 0;

        if (lexer.consume_specific("//"sv)) {
            lexer.consume_until('\n');
            lexer.ignore();
            consumed = true;
        }
    }
}

static Optional<LegacyConstructor> const& lookup_legacy_constructor(IDL::Interface const& interface)
{
    static HashMap<StringView, Optional<LegacyConstructor>> s_legacy_constructors;
    if (auto cache = s_legacy_constructors.get(interface.name); cache.has_value())
        return cache.value();

    auto attribute = interface.extended_attributes.get("LegacyFactoryFunction"sv);
    if (!attribute.has_value()) {
        s_legacy_constructors.set(interface.name, {});
        return s_legacy_constructors.get(interface.name).value();
    }

    GenericLexer function_lexer(attribute.value());
    consume_whitespace(function_lexer);

    auto name = function_lexer.consume_until([](auto ch) { return is_ascii_space(ch) || ch == '('; });
    auto constructor_class = ByteString::formatted("{}Constructor", name);

    s_legacy_constructors.set(interface.name, LegacyConstructor { name, move(constructor_class) });
    return s_legacy_constructors.get(interface.name).value();
}

static ErrorOr<void> generate_intrinsic_definitions_header(StringView output_path, InterfaceSets const& interface_sets)
{
    StringBuilder builder;
    SourceGenerator generator(builder);

    generator.append(R"~~~(
#pragma once

#include <AK/Types.h>

namespace Web::Bindings {

enum class InterfaceName : u16 {
    Unknown = 0,
)~~~");

    for (size_t i = 0; i < interface_sets.intrinsics.size(); ++i) {
        auto const& interface = interface_sets.intrinsics[i];
        size_t index = i + 1; // 0 is reserved for Unknown

        generator.set("interface_name", interface.name);
        generator.set("index", String::number(index));

        generator.append(R"~~~(
    @interface_name@ = @index@,)~~~");
    }

    generator.append(R"~~~(
};

bool is_exposed(InterfaceName, JS::Realm&);

}
)~~~");

    auto generated_intrinsics_path = LexicalPath(output_path).append("IntrinsicDefinitions.h"sv).string();
    auto generated_intrinsics_file = TRY(Core::File::open(generated_intrinsics_path, Core::File::OpenMode::Write));
    TRY(generated_intrinsics_file->write_until_depleted(generator.as_string_view().bytes()));

    return {};
}

static ErrorOr<void> generate_intrinsic_definitions_implementation(StringView output_path, InterfaceSets const& interface_sets)
{
    StringBuilder builder;
    SourceGenerator generator(builder);

    generator.append(R"~~~(
#include <LibGC/DeferGC.h>
#include <LibJS/Runtime/Object.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/DedicatedWorkerGlobalScope.h>
#include <LibWeb/HTML/SharedWorkerGlobalScope.h>
#include <LibWeb/HTML/ShadowRealmGlobalScope.h>)~~~");

    for (auto& interface : interface_sets.intrinsics) {
        auto gen = generator.fork();
        gen.set("namespace_class", interface.namespace_class);
        gen.set("prototype_class", interface.prototype_class);
        gen.set("constructor_class", interface.constructor_class);

        if (interface.is_namespace) {
            gen.append(R"~~~(
#include <LibWeb/Bindings/@namespace_class@.h>)~~~");
        } else {
            gen.append(R"~~~(
#include <LibWeb/Bindings/@constructor_class@.h>
#include <LibWeb/Bindings/@prototype_class@.h>)~~~");

            if (auto const& legacy_constructor = lookup_legacy_constructor(interface); legacy_constructor.has_value()) {
                gen.set("legacy_constructor_class", legacy_constructor->constructor_class);
                gen.append(R"~~~(
#include <LibWeb/Bindings/@legacy_constructor_class@.h>)~~~");
            }
        }
    }

    generator.append(R"~~~(

namespace Web::Bindings {
)~~~");

    auto add_namespace = [&](SourceGenerator& gen, StringView name, StringView namespace_class) {
        gen.set("interface_name", name);
        gen.set("namespace_class", namespace_class);

        gen.append(R"~~~(
template<>
void Intrinsics::create_web_namespace<@namespace_class@>(JS::Realm& realm)
{
    auto namespace_object = realm.create<@namespace_class@>(realm);
    m_namespaces.set("@interface_name@"_fly_string, namespace_object);

    [[maybe_unused]] static constexpr u8 attr = JS::Attribute::Writable | JS::Attribute::Configurable;)~~~");

        for (auto& interface : interface_sets.intrinsics) {
            if (interface.extended_attributes.get("LegacyNamespace"sv) != name)
                continue;

            gen.set("owned_interface_name", interface.name);
            gen.set("owned_prototype_class", interface.prototype_class);

            gen.append(R"~~~(
    namespace_object->define_intrinsic_accessor("@owned_interface_name@"_utf16_fly_string, attr, [](auto& realm) -> JS::Value { return &Bindings::ensure_web_constructor<@owned_prototype_class@>(realm, "@interface_name@.@owned_interface_name@"_fly_string); });)~~~");
        }

        gen.append(R"~~~(
}
)~~~");
    };

    generator.append(R"~~~(
static bool is_secure_context_interface(InterfaceName name)
{
    switch (name) {
)~~~");
    for (auto const& interface : interface_sets.intrinsics) {
        if (!interface.extended_attributes.contains("SecureContext"))
            continue;

        generator.set("secure_context_interface_name", interface.name);
        generator.append(R"~~~(
    case InterfaceName::@secure_context_interface_name@:)~~~");
    }
    generator.append(R"~~~(
        return true;
    default:
        return false;
    }
}
)~~~");

    auto generate_global_exposed = [&generator](StringView global_name, Vector<IDL::Interface&> const& interface_set) {
        generator.set("global_name", global_name);
        generator.append(R"~~~(
static bool is_@global_name@_exposed(InterfaceName name)
{
    switch (name) {
)~~~");
        for (auto const& interface : interface_set) {
            auto gen = generator.fork();
            gen.set("interface_name", interface.name);
            gen.append(R"~~~(
    case InterfaceName::@interface_name@:)~~~");
        }

        generator.append(R"~~~(
        return true;
    default:
        return false;
    }
}
)~~~");
    };

    generate_global_exposed("window"sv, interface_sets.window_exposed);
    generate_global_exposed("dedicated_worker"sv, interface_sets.dedicated_worker_exposed);
    generate_global_exposed("shared_worker"sv, interface_sets.shared_worker_exposed);
    generate_global_exposed("shadow_realm"sv, interface_sets.shadow_realm_exposed);

    // https://webidl.spec.whatwg.org/#dfn-exposed
    generator.append(R"~~~(
// An interface, callback interface, namespace, or member construct is exposed in a given realm realm if the following steps return true:
// FIXME: Make this compatible with non-interface types.
bool is_exposed(InterfaceName name, JS::Realm& realm)
{
    auto const& global_object = realm.global_object();

    // 1. If construct’s exposure set is not *, and realm.[[GlobalObject]] does not implement an interface that is in construct’s exposure set, then return false.
    if (is<HTML::Window>(global_object)) {
       if (!is_window_exposed(name))
           return false;
    } else if (is<HTML::DedicatedWorkerGlobalScope>(global_object)) {
       if (!is_dedicated_worker_exposed(name))
           return false;
    } else if (is<HTML::SharedWorkerGlobalScope>(global_object)) {
        if (!is_shared_worker_exposed(name))
            return false;
    } else if (is<HTML::ShadowRealmGlobalScope>(global_object)) {
        if (!is_shadow_realm_exposed(name))
            return false;
    } else {
        TODO(); // FIXME: ServiceWorkerGlobalScope and WorkletGlobalScope.
    }

    // 2. If realm’s settings object is not a secure context, and construct is conditionally exposed on
    //    [SecureContext], then return false.
    if (is_secure_context_interface(name) && HTML::is_non_secure_context(principal_host_defined_environment_settings_object(realm)))
        return false;

    // FIXME: 3. If realm’s settings object’s cross-origin isolated capability is false, and construct is
    //           conditionally exposed on [CrossOriginIsolated], then return false.

    // 4. Return true.
    return true;
}

)~~~");

    auto add_interface = [](SourceGenerator& gen, StringView name, StringView prototype_class, StringView constructor_class, Optional<LegacyConstructor> const& legacy_constructor, StringView named_properties_class) {
        gen.set("interface_name", name);
        gen.set("prototype_class", prototype_class);
        gen.set("constructor_class", constructor_class);

        gen.append(R"~~~(
template<>
WEB_API void Intrinsics::create_web_prototype_and_constructor<@prototype_class@>(JS::Realm& realm)
{
    auto& vm = realm.vm();

)~~~");
        if (!named_properties_class.is_empty()) {
            gen.set("named_properties_class", named_properties_class);
            gen.append(R"~~~(
    auto named_properties_object = realm.create<@named_properties_class@>(realm);
    m_prototypes.set("@named_properties_class@"_fly_string, named_properties_object);

)~~~");
        }
        gen.append(R"~~~(
    auto prototype = realm.create<@prototype_class@>(realm);
    m_prototypes.set("@interface_name@"_fly_string, prototype);

    auto constructor = realm.create<@constructor_class@>(realm);
    m_constructors.set("@interface_name@"_fly_string, constructor);

    prototype->define_direct_property(vm.names.constructor, constructor.ptr(), JS::Attribute::Writable | JS::Attribute::Configurable);
)~~~");

        if (legacy_constructor.has_value()) {
            gen.set("legacy_interface_name", legacy_constructor->name);
            gen.set("legacy_constructor_class", legacy_constructor->constructor_class);
            gen.append(R"~~~(
    auto legacy_constructor = realm.create<@legacy_constructor_class@>(realm);
    m_constructors.set("@legacy_interface_name@"_fly_string, legacy_constructor);)~~~");
        }

        gen.append(R"~~~(
}
)~~~");
    };

    for (auto& interface : interface_sets.intrinsics) {
        auto gen = generator.fork();

        String named_properties_class;
        if (interface.extended_attributes.contains("Global") && interface.supports_named_properties()) {
            named_properties_class = MUST(String::formatted("{}Properties", interface.name));
        }

        if (interface.is_namespace)
            add_namespace(gen, interface.name, interface.namespace_class);
        else
            add_interface(gen, interface.namespaced_name, interface.prototype_class, interface.constructor_class, lookup_legacy_constructor(interface), named_properties_class);
    }

    generator.append(R"~~~(
}
)~~~");

    auto generated_intrinsics_path = LexicalPath(output_path).append("IntrinsicDefinitions.cpp"sv).string();
    auto generated_intrinsics_file = TRY(Core::File::open(generated_intrinsics_path, Core::File::OpenMode::Write));
    TRY(generated_intrinsics_file->write_until_depleted(generator.as_string_view().bytes()));

    return {};
}

static ErrorOr<void> generate_exposed_interface_header(StringView class_name, StringView output_path)
{
    StringBuilder builder;
    SourceGenerator generator(builder);

    generator.set("global_object_snake_name", ByteString(class_name).to_snakecase());
    generator.append(R"~~~(
#pragma once

#include <LibJS/Forward.h>

namespace Web::Bindings {

void add_@global_object_snake_name@_exposed_interfaces(JS::Object&);

}

)~~~");

    auto generated_header_path = LexicalPath(output_path).append(ByteString::formatted("{}ExposedInterfaces.h", class_name)).string();
    auto generated_header_file = TRY(Core::File::open(generated_header_path, Core::File::OpenMode::Write));
    TRY(generated_header_file->write_until_depleted(generator.as_string_view().bytes()));

    return {};
}

static ErrorOr<void> generate_exposed_interface_implementation(StringView class_name, StringView output_path, Vector<IDL::Interface&>& exposed_interfaces)
{
    StringBuilder builder;
    SourceGenerator generator(builder);

    generator.set("global_object_name", class_name);
    generator.set("global_object_snake_name", ByteString(class_name).to_snakecase());

    generator.append(R"~~~(
#include <LibJS/Runtime/Object.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/@global_object_name@ExposedInterfaces.h>
#include <LibWeb/HTML/Scripting/Environments.h>
)~~~");
    for (auto& interface : exposed_interfaces) {
        auto gen = generator.fork();
        gen.set("namespace_class", interface.namespace_class);
        gen.set("prototype_class", interface.prototype_class);
        gen.set("constructor_class", interface.constructor_class);

        if (interface.is_namespace) {
            gen.append(R"~~~(#include <LibWeb/Bindings/@namespace_class@.h>
)~~~");
        } else {

            gen.append(R"~~~(#include <LibWeb/Bindings/@constructor_class@.h>
#include <LibWeb/Bindings/@prototype_class@.h>
)~~~");

            if (auto const& legacy_constructor = lookup_legacy_constructor(interface); legacy_constructor.has_value()) {
                gen.set("legacy_constructor_class", legacy_constructor->constructor_class);
                gen.append(R"~~~(#include <LibWeb/Bindings/@legacy_constructor_class@.h>
)~~~");
            }
        }
    }

    generator.append(R"~~~(
namespace Web::Bindings {

void add_@global_object_snake_name@_exposed_interfaces(JS::Object& global)
{
    static constexpr u8 attr = JS::Attribute::Writable | JS::Attribute::Configurable;
    [[maybe_unused]] bool is_secure_context = HTML::is_secure_context(HTML::relevant_principal_settings_object(global));
)~~~");

    auto add_interface = [class_name](SourceGenerator& gen, IDL::Interface const& interface) {
        auto legacy_constructor = lookup_legacy_constructor(interface);
        Optional<ByteString const&> legacy_alias_name;
        if (class_name == "Window"sv)
            legacy_alias_name = interface.extended_attributes.get("LegacyWindowAlias"sv);

        gen.set("interface_name", interface.namespaced_name);
        gen.set("prototype_class", interface.prototype_class);

        if (interface.extended_attributes.contains("SecureContext")) {
            gen.append(R"~~~(
    if (is_secure_context) {)~~~");
        }

        gen.append(R"~~~(
    global.define_intrinsic_accessor("@interface_name@"_utf16_fly_string, attr, [](auto& realm) -> JS::Value { return &ensure_web_constructor<@prototype_class@>(realm, "@interface_name@"_fly_string); });)~~~");

        // https://webidl.spec.whatwg.org/#LegacyWindowAlias
        if (legacy_alias_name.has_value()) {
            if (legacy_alias_name->starts_with('(')) {
                auto legacy_alias_names = legacy_alias_name->substring_view(1).split_view(',');
                for (auto legacy_alias_name : legacy_alias_names) {
                    gen.set("interface_alias_name", legacy_alias_name.trim_whitespace());
                    gen.append(R"~~~(
    global.define_intrinsic_accessor("@interface_alias_name@"_utf16_fly_string, attr, [](auto& realm) -> JS::Value { return &ensure_web_constructor<@prototype_class@>(realm, "@interface_name@"_fly_string); });)~~~");
                }
            } else {
                gen.set("interface_alias_name", *legacy_alias_name);
                gen.append(R"~~~(
    global.define_intrinsic_accessor("@interface_alias_name@"_utf16_fly_string, attr, [](auto& realm) -> JS::Value { return &ensure_web_constructor<@prototype_class@>(realm, "@interface_name@"_fly_string); });)~~~");
            }
        }

        if (legacy_constructor.has_value()) {
            gen.set("legacy_interface_name", legacy_constructor->name);
            gen.append(R"~~~(
    global.define_intrinsic_accessor("@legacy_interface_name@"_utf16_fly_string, attr, [](auto& realm) -> JS::Value { return &ensure_web_constructor<@prototype_class@>(realm, "@legacy_interface_name@"_fly_string); });)~~~");
        }

        if (interface.extended_attributes.contains("SecureContext")) {
            gen.append(R"~~~(
    })~~~");
        }
    };

    auto add_namespace = [](SourceGenerator& gen, StringView name, StringView namespace_class) {
        gen.set("interface_name", name);
        gen.set("namespace_class", namespace_class);

        gen.append(R"~~~(
    global.define_intrinsic_accessor("@interface_name@"_utf16_fly_string, attr, [](auto& realm) -> JS::Value { return &ensure_web_namespace<@namespace_class@>(realm, "@interface_name@"_fly_string); });)~~~");
    };

    for (auto& interface : exposed_interfaces) {
        auto gen = generator.fork();

        if (interface.is_namespace) {
            add_namespace(gen, interface.name, interface.namespace_class);
        } else if (!interface.extended_attributes.contains("LegacyNamespace"sv)) {
            if (interface.extended_attributes.contains("LegacyNoInterfaceObject")) {
                continue;
            }
            add_interface(gen, interface);
        }
    }

    generator.append(R"~~~(
}

}
)~~~");

    auto generated_implementation_path = LexicalPath(output_path).append(ByteString::formatted("{}ExposedInterfaces.cpp", class_name)).string();
    auto generated_implementation_file = TRY(Core::File::open(generated_implementation_path, Core::File::OpenMode::Write));
    TRY(generated_implementation_file->write_until_depleted(generator.as_string_view().bytes()));

    return {};
}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    Core::ArgsParser args_parser;

    StringView output_path;
    Vector<ByteString> base_paths;
    Vector<ByteString> paths;

    args_parser.add_option(output_path, "Path to output generated files into", "output-path", 'o', "output-path");
    args_parser.add_option(Core::ArgsParser::Option {
        .argument_mode = Core::ArgsParser::OptionArgumentMode::Required,
        .help_string = "Path to root of IDL file tree(s)",
        .long_name = "base-path",
        .short_name = 'b',
        .value_name = "base-path",
        .accept_value = [&](StringView s) {
            base_paths.append(s);
            return true;
        },
    });
    args_parser.add_positional_argument(paths, "Paths of every IDL file that could be Exposed", "paths");
    args_parser.parse(arguments);

    VERIFY(!paths.is_empty());
    VERIFY(!base_paths.is_empty());

    if (paths.first().starts_with("@"sv)) {
        // Response file
        auto file_or_error = Core::File::open(paths.first().substring_view(1), Core::File::OpenMode::Read);
        paths.remove(0);
        VERIFY(paths.is_empty());

        if (file_or_error.is_error()) {
            s_error_string = ByteString::formatted("Unable to open response file {}", paths.first());
            return Error::from_string_view(s_error_string.view());
        }
        auto file = file_or_error.release_value();
        auto string = TRY(file->read_until_eof());
        for (auto const& path : StringView(string).split_view('\n')) {
            if (path.is_empty())
                continue;
            paths.append(path);
        }
    }

    Vector<ByteString> lexical_bases;
    for (auto const& base_path : base_paths) {
        VERIFY(!base_path.is_empty());
        lexical_bases.append(base_path);
    }

    // Read in all IDL files, we must own the storage for all of these for the lifetime of the program
    Vector<NonnullOwnPtr<Core::MappedFile>> files;
    files.ensure_capacity(paths.size());
    for (ByteString const& path : paths) {
        auto file_or_error = Core::MappedFile::map(path, Core::MappedFile::Mode::ReadOnly);
        if (file_or_error.is_error()) {
            s_error_string = ByteString::formatted("Unable to open file {}", path);
            return Error::from_string_view(s_error_string.view());
        }
        files.append(file_or_error.release_value());
    }
    VERIFY(paths.size() == files.size());

    Vector<IDL::Parser> parsers;
    InterfaceSets interface_sets;

    for (size_t i = 0; i < paths.size(); ++i) {
        auto const& path = paths[i];
        StringView file_contents = files[i]->bytes();
        IDL::Parser parser(path, file_contents, lexical_bases);
        auto& interface = parser.parse();
        if (interface.name.is_empty()) {
            s_error_string = ByteString::formatted("Interface for file {} missing", path);
            return Error::from_string_view(s_error_string.view());
        }

        TRY(add_to_interface_sets(interface, interface_sets));
        parsers.append(move(parser));
    }

    TRY(generate_intrinsic_definitions_header(output_path, interface_sets));
    TRY(generate_intrinsic_definitions_implementation(output_path, interface_sets));

    TRY(generate_exposed_interface_header("Window"sv, output_path));
    TRY(generate_exposed_interface_header("DedicatedWorker"sv, output_path));
    TRY(generate_exposed_interface_header("SharedWorker"sv, output_path));
    TRY(generate_exposed_interface_header("ShadowRealm"sv, output_path));
    // TODO: ServiceWorkerExposed.h

    TRY(generate_exposed_interface_implementation("Window"sv, output_path, interface_sets.window_exposed));
    TRY(generate_exposed_interface_implementation("DedicatedWorker"sv, output_path, interface_sets.dedicated_worker_exposed));
    TRY(generate_exposed_interface_implementation("SharedWorker"sv, output_path, interface_sets.shared_worker_exposed));
    TRY(generate_exposed_interface_implementation("ShadowRealm"sv, output_path, interface_sets.shadow_realm_exposed));
    // TODO: ServiceWorkerExposed.cpp

    return 0;
}

ErrorOr<void> add_to_interface_sets(IDL::Interface& interface, InterfaceSets& interface_sets)
{
    // TODO: Add service worker exposed and audio worklet exposed

    auto maybe_exposed = interface.extended_attributes.get("Exposed");
    if (!maybe_exposed.has_value()) {
        s_error_string = ByteString::formatted("Interface {} is missing extended attribute Exposed", interface.name);
        return Error::from_string_view(s_error_string.view());
    }
    auto whom = TRY(IDL::parse_exposure_set(interface.name, *maybe_exposed));

    interface_sets.intrinsics.append(interface);

    if (has_flag(whom, IDL::ExposedTo::Window))
        interface_sets.window_exposed.append(interface);

    if (has_flag(whom, IDL::ExposedTo::DedicatedWorker))
        interface_sets.dedicated_worker_exposed.append(interface);

    if (has_flag(whom, IDL::ExposedTo::SharedWorker))
        interface_sets.shared_worker_exposed.append(interface);

    if (has_flag(whom, IDL::ExposedTo::ShadowRealm))
        interface_sets.shadow_realm_exposed.append(interface);

    return {};
}
