/*
 * Copyright (c) 2020-2021, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "IDLGenerators.h"
#include "Namespaces.h"
#include <AK/Debug.h>
#include <AK/LexicalPath.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/File.h>
#include <LibIDL/IDLParser.h>
#include <LibIDL/Types.h>

ErrorOr<int> serenity_main(Main::Arguments arguments)
{
    Core::ArgsParser args_parser;
    StringView path;
    StringView import_base_path;
    StringView output_path = "-"sv;
    StringView depfile_path;
    StringView depfile_prefix;

    args_parser.add_option(Core::ArgsParser::Option {
        .argument_mode = Core::ArgsParser::OptionArgumentMode::Required,
        .help_string = "Add a header search path passed to the compiler",
        .long_name = "header-include-path",
        .short_name = 'i',
        .value_name = "path",
        .accept_value = [&](StringView s) {
            IDL::g_header_search_paths.append(s);
            return true;
        },
    });
    args_parser.add_option(output_path, "Path to output generated files into", "output-path", 'o', "output-path");
    args_parser.add_option(depfile_path, "Path to write dependency file to", "depfile", 'd', "depfile-path");
    args_parser.add_option(depfile_prefix, "Prefix to prepend to relative paths in dependency file", "depfile-prefix", 'p', "depfile-prefix");
    args_parser.add_positional_argument(path, "IDL file", "idl-file");
    args_parser.add_positional_argument(import_base_path, "Import base path", "import-base-path", Core::ArgsParser::Required::No);
    args_parser.parse(arguments);

    auto idl_file = TRY(Core::File::open(path, Core::File::OpenMode::Read));

    LexicalPath lexical_path(path);
    auto& namespace_ = lexical_path.parts_view().at(lexical_path.parts_view().size() - 2);

    auto data = TRY(idl_file->read_until_eof());

    if (import_base_path.is_null())
        import_base_path = lexical_path.dirname();

    IDL::Parser parser(path, data, import_base_path);
    auto& interface = parser.parse();

    // If the interface name is the same as its namespace, qualify the name in the generated code.
    // e.g. Selection::Selection
    if (IDL::libweb_interface_namespaces.span().contains_slow(namespace_)) {
        StringBuilder builder;
        builder.append(namespace_);
        builder.append("::"sv);
        builder.append(interface.implemented_name);
        interface.fully_qualified_name = builder.to_byte_string();
    } else {
        interface.fully_qualified_name = interface.implemented_name;
    }

    if constexpr (BINDINGS_GENERATOR_DEBUG) {
        dbgln("Attributes:");
        for (auto& attribute : interface.attributes) {
            dbgln("  {}{}{}{} {}",
                attribute.inherit ? "inherit " : "",
                attribute.readonly ? "readonly " : "",
                attribute.type->name(),
                attribute.type->is_nullable() ? "?" : "",
                attribute.name);
        }

        dbgln("Functions:");
        for (auto& function : interface.functions) {
            dbgln("  {}{} {}",
                function.return_type->name(),
                function.return_type->is_nullable() ? "?" : "",
                function.name);
            for (auto& parameter : function.parameters) {
                dbgln("    {}{} {}",
                    parameter.type->name(),
                    parameter.type->is_nullable() ? "?" : "",
                    parameter.name);
            }
        }

        dbgln("Static Functions:");
        for (auto& function : interface.static_functions) {
            dbgln("  static {}{} {}",
                function.return_type->name(),
                function.return_type->is_nullable() ? "?" : "",
                function.name);
            for (auto& parameter : function.parameters) {
                dbgln("    {}{} {}",
                    parameter.type->name(),
                    parameter.type->is_nullable() ? "?" : "",
                    parameter.name);
            }
        }
    }

    StringBuilder output_builder;

    auto write_if_changed = [&](auto generator_function, StringView file_path) -> ErrorOr<void> {
        (*generator_function)(interface, output_builder);

        auto current_file_or_error = Core::File::open(file_path, Core::File::OpenMode::Read);
        if (current_file_or_error.is_error() && current_file_or_error.error().code() != ENOENT)
            return current_file_or_error.release_error();

        ByteBuffer current_contents;
        if (!current_file_or_error.is_error())
            current_contents = TRY(current_file_or_error.value()->read_until_eof());
        // Only write to disk if contents have changed
        if (current_contents != output_builder.string_view().bytes()) {
            auto output_file = TRY(Core::File::open(file_path, Core::File::OpenMode::Write | Core::File::OpenMode::Truncate));
            TRY(output_file->write_until_depleted(output_builder.string_view().bytes()));
        }
        // FIXME: Can we add clear_with_capacity to StringBuilder instead of throwing away the allocated buffer?
        output_builder.clear();
        return {};
    };

    String namespace_header;
    String namespace_implementation;
    String constructor_header;
    String constructor_implementation;
    String prototype_header;
    String prototype_implementation;
    String iterator_prototype_header;
    String iterator_prototype_implementation;
    String global_mixin_header;
    String global_mixin_implementation;

    auto path_prefix = LexicalPath::join(output_path, lexical_path.basename(LexicalPath::StripExtension::Yes));

    if (interface.is_namespace) {
        namespace_header = TRY(String::formatted("{}Namespace.h", path_prefix));
        namespace_implementation = TRY(String::formatted("{}Namespace.cpp", path_prefix));

        TRY(write_if_changed(&IDL::generate_namespace_header, namespace_header));
        TRY(write_if_changed(&IDL::generate_namespace_implementation, namespace_implementation));
    } else {
        constructor_header = TRY(String::formatted("{}Constructor.h", path_prefix));
        constructor_implementation = TRY(String::formatted("{}Constructor.cpp", path_prefix));
        prototype_header = TRY(String::formatted("{}Prototype.h", path_prefix));
        prototype_implementation = TRY(String::formatted("{}Prototype.cpp", path_prefix));

        TRY(write_if_changed(&IDL::generate_constructor_header, constructor_header));
        TRY(write_if_changed(&IDL::generate_constructor_implementation, constructor_implementation));
        TRY(write_if_changed(&IDL::generate_prototype_header, prototype_header));
        TRY(write_if_changed(&IDL::generate_prototype_implementation, prototype_implementation));
    }

    if (interface.pair_iterator_types.has_value()) {
        iterator_prototype_header = TRY(String::formatted("{}IteratorPrototype.h", path_prefix));
        iterator_prototype_implementation = TRY(String::formatted("{}IteratorPrototype.cpp", path_prefix));

        TRY(write_if_changed(&IDL::generate_iterator_prototype_header, iterator_prototype_header));
        TRY(write_if_changed(&IDL::generate_iterator_prototype_implementation, iterator_prototype_implementation));
    }

    if (interface.extended_attributes.contains("Global")) {
        global_mixin_header = TRY(String::formatted("{}GlobalMixin.h", path_prefix));
        global_mixin_implementation = TRY(String::formatted("{}GlobalMixin.cpp", path_prefix));

        TRY(write_if_changed(&IDL::generate_global_mixin_header, global_mixin_header));
        TRY(write_if_changed(&IDL::generate_global_mixin_implementation, global_mixin_implementation));
    }

    if (!depfile_path.is_empty()) {
        auto depfile = TRY(Core::File::open_file_or_standard_stream(depfile_path, Core::File::OpenMode::Write));

        StringBuilder depfile_builder;
        for (StringView s : { constructor_header, constructor_implementation, prototype_header, prototype_implementation, namespace_header, namespace_implementation, iterator_prototype_header, iterator_prototype_implementation, global_mixin_header, global_mixin_implementation }) {
            if (s.is_empty())
                continue;

            if (!depfile_prefix.is_empty())
                depfile_builder.append(LexicalPath::join(depfile_prefix, s).string());
            else
                depfile_builder.append(s);

            break;
        }
        depfile_builder.append(':');
        for (auto const& path : parser.imported_files()) {
            depfile_builder.append(" \\\n "sv);
            depfile_builder.append(path);
        }
        depfile_builder.append('\n');
        TRY(depfile->write_until_depleted(depfile_builder.string_view().bytes()));
    }
    return 0;
}
