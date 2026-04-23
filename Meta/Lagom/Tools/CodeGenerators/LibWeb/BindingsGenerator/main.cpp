/*
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2022, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "IDLGenerators.h"
#include <AK/Assertions.h>
#include <AK/Debug.h>
#include <AK/HashTable.h>
#include <AK/LexicalPath.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/File.h>
#include <LibCore/MappedFile.h>
#include <LibIDL/IDLParser.h>
#include <LibIDL/Types.h>

template<typename GeneratorFunction>
static ErrorOr<void> write_if_changed(GeneratorFunction generator_function, IDL::Interface const& interface, StringView file_path)
{
    StringBuilder output_builder;
    generator_function(interface, output_builder);

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

    return {};
}

static ErrorOr<void> generate_depfile(StringView depfile_path, ReadonlySpan<ByteString> dependency_paths, ReadonlySpan<ByteString> output_files)
{
    auto depfile = TRY(Core::File::open_file_or_standard_stream(depfile_path, Core::File::OpenMode::Write));

    StringBuilder depfile_builder;
    bool first_output = true;
    for (auto const& s : output_files) {
        if (!first_output)
            depfile_builder.append(' ');

        depfile_builder.append(s);
        first_output = false;
    }
    depfile_builder.append(':');
    for (auto const& path : dependency_paths) {
        depfile_builder.append(" \\\n "sv);
        depfile_builder.append(path);
    }
    depfile_builder.append('\n');
    return depfile->write_until_depleted(depfile_builder.string_view().bytes());
}

static ByteString cpp_namespace_for_module_path(ByteString const& module_own_path)
{
    auto path = LexicalPath { module_own_path };
    auto parts = path.parts_view();
    for (size_t i = 0; i + 2 < parts.size(); ++i) {
        if (parts[i] != "LibWeb"sv)
            continue;

        return parts[i + 1].to_byte_string();
    }

    if (parts.size() >= 2)
        return parts[parts.size() - 2].to_byte_string();

    return {};
}

static void assign_fully_qualified_name(IDL::Interface& interface)
{
    auto namespace_name = cpp_namespace_for_module_path(interface.module_own_path);
    if (namespace_name.is_empty()) {
        interface.fully_qualified_name = interface.implemented_name;
        return;
    }

    interface.fully_qualified_name = ByteString::formatted("{}::{}", namespace_name, interface.implemented_name);
}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    Core::ArgsParser args_parser;
    Vector<ByteString> paths;
    StringView output_path = "-"sv;
    StringView depfile_path;

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
    args_parser.add_positional_argument(paths, "IDL file(s)", "idl-files");
    args_parser.parse(arguments);

    if (paths.first().starts_with("@"sv)) {
        auto file = TRY(Core::File::open(paths.first().substring_view(1), Core::File::OpenMode::Read));
        paths.remove(0);
        VERIFY(paths.is_empty());

        auto string = TRY(file->read_until_eof());
        for (auto const& path : StringView(string).split_view('\n')) {
            if (path.is_empty())
                continue;
            paths.append(path);
        }
    }

    IDL::Context context;
    Vector<ByteString> dependency_paths;
    HashTable<ByteString> seen_dependency_paths;
    Vector<ByteString> output_files;

    auto append_dependency_path = [&](ByteString const& dependency_path) {
        if (seen_dependency_paths.set(dependency_path) != AK::HashSetResult::InsertedNewEntry)
            return;
        dependency_paths.append(dependency_path);
    };

    for (auto const& path : paths) {
        auto file = TRY(Core::MappedFile::map(path, Core::MappedFile::Mode::ReadOnly));
        auto module = IDL::Parser::parse(path, file->bytes(), context);
        append_dependency_path(path);
    }

    context.resolve();

    for (auto& interface : context.owned_interfaces)
        assign_fully_qualified_name(*interface);

    for (auto const& module : context.owned_modules) {
        if (!module->interface.has_value())
            continue;

        auto& interface = module->interface.value();
        if (!interface.will_generate_code())
            continue;

        if constexpr (BINDINGS_GENERATOR_DEBUG)
            interface.dump();

        auto lexical_path = LexicalPath { module->module_own_path };
        auto path_prefix = LexicalPath::join(output_path, lexical_path.basename(LexicalPath::StripExtension::Yes));
        auto header_path = ByteString::formatted("{}.h", path_prefix);
        auto implementation_path = ByteString::formatted("{}.cpp", path_prefix);

        TRY(write_if_changed(&IDL::generate_header, interface, header_path));
        TRY(write_if_changed(&IDL::generate_implementation, interface, implementation_path));

        output_files.append(header_path);
        output_files.append(implementation_path);
    }

    if (!depfile_path.is_empty()) {
        TRY(generate_depfile(depfile_path, dependency_paths, output_files));
    }
    return 0;
}
