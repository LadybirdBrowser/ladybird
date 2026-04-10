/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/File.h>
#include <LibMain/Main.h>
#include <LibWeb/CSS/Parser/Tokenizer.h>

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    StringView encoding = "utf-8"sv;
    StringView input_path;

    Core::ArgsParser args_parser;
    args_parser.add_option(encoding, "Source encoding label", "encoding", 'e', "encoding");
    args_parser.add_positional_argument(input_path, "Path to the CSS input file", "input", Core::ArgsParser::Required::Yes);
    args_parser.parse(arguments);

    auto file = TRY(Core::File::open(input_path, Core::File::OpenMode::Read));
    auto input = TRY(file->read_until_eof());
    auto tokens = Web::CSS::Parser::Tokenizer::tokenize(input, encoding);

    for (auto const& token : tokens)
        outln("{}", token.to_debug_string());

    return 0;
}
