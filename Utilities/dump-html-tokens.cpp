/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/ElapsedTimer.h>
#include <LibCore/File.h>
#include <LibMain/Main.h>
#include <LibWeb/HTML/Parser/HTMLToken.h>
#include <LibWeb/HTML/Parser/HTMLTokenizer.h>

using Web::HTML::HTMLToken;
using Web::HTML::HTMLTokenizer;

static String format_position(HTMLToken::Position const& pos)
{
    return MUST(String::formatted("{}:{}", pos.line, pos.column));
}

static void dump_token(HTMLToken const& token)
{
    switch (token.type()) {
    case HTMLToken::Type::DOCTYPE: {
        auto const& doctype = token.doctype_data();
        out("DOCTYPE");
        if (!doctype.missing_name)
            out(" name=\"{}\"", doctype.name);
        if (!doctype.missing_public_identifier)
            out(" public_id=\"{}\"", doctype.public_identifier);
        if (!doctype.missing_system_identifier)
            out(" system_id=\"{}\"", doctype.system_identifier);
        if (doctype.force_quirks)
            out(" force_quirks");
        outln(" @{}-{}", format_position(token.start_position()), format_position(token.end_position()));
        break;
    }
    case HTMLToken::Type::StartTag: {
        if (token.is_self_closing())
            out("StartTag <{}/> [", token.tag_name());
        else
            out("StartTag <{}> [", token.tag_name());
        bool first = true;
        token.for_each_attribute([&](auto& attribute) {
            if (!first)
                out(" ");
            first = false;
            out("{}=\"{}\"", attribute.local_name, attribute.value);
            return IterationDecision::Continue;
        });
        outln("] @{}-{}", format_position(token.start_position()), format_position(token.end_position()));
        break;
    }
    case HTMLToken::Type::EndTag:
        outln("EndTag </{}> @{}-{}", token.tag_name(), format_position(token.start_position()), format_position(token.end_position()));
        break;
    case HTMLToken::Type::Comment:
        outln("Comment \"{}\" @{}-{}", token.comment(), format_position(token.start_position()), format_position(token.end_position()));
        break;
    case HTMLToken::Type::Character: {
        auto code_point = token.code_point();
        if (code_point >= 0x20 && code_point < 0x7F)
            outln("Character U+{:04X} '{:c}' @{}", code_point, static_cast<char>(code_point), format_position(token.start_position()));
        else
            outln("Character U+{:04X} @{}", code_point, format_position(token.start_position()));
        break;
    }
    case HTMLToken::Type::EndOfFile:
        outln("EndOfFile @{}", format_position(token.start_position()));
        break;
    case HTMLToken::Type::Invalid:
        VERIFY_NOT_REACHED();
    }
}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    StringView file_path;
    StringView initial_state_name;
    bool silent = false;
    int iterations = 1;

    Core::ArgsParser args_parser;
    args_parser.set_general_help(
        "Tokenize HTML and dump tokens in a canonical format. "
        "Use --silent and --iterations for perf work on the tokenizer itself.");
    args_parser.add_positional_argument(file_path, "Path to HTML file (or - for stdin)", "file", Core::ArgsParser::Required::No);
    args_parser.add_option(initial_state_name, "Initial tokenizer state", "initial-state", 's', "state");
    args_parser.add_option(silent, "Don't print tokens (for benchmarking)", "silent", 'q');
    args_parser.add_option(iterations, "Run the tokenizer N times and report timing", "iterations", 'n', "count");
    args_parser.parse(arguments);

    if (iterations < 1) {
        warnln("--iterations must be at least 1");
        return 1;
    }

    Optional<HTMLTokenizer::State> initial_state;
    if (!initial_state_name.is_empty()) {
#define __ENUMERATE_TOKENIZER_STATE(s) \
    if (initial_state_name == #s##sv)  \
        initial_state = HTMLTokenizer::State::s;
        ENUMERATE_TOKENIZER_STATES
#undef __ENUMERATE_TOKENIZER_STATE
        if (!initial_state.has_value()) {
            warnln("Unknown tokenizer state: '{}'", initial_state_name);
            return 1;
        }
    }

    ByteBuffer input_data;
    if (file_path.is_empty() || file_path == "-"sv) {
        auto stdin_file = TRY(Core::File::standard_input());
        input_data = TRY(stdin_file->read_until_eof());
    } else {
        auto file = TRY(Core::File::open(file_path, Core::File::OpenMode::Read));
        input_data = TRY(file->read_until_eof());
    }

    StringView input { input_data };

    auto timer = Core::ElapsedTimer::start_new();
    u64 total_tokens = 0;

    for (int i = 0; i < iterations; i++) {
        HTMLTokenizer tokenizer { input, "UTF-8"sv };
        if (initial_state.has_value())
            tokenizer.switch_to(initial_state.value());

        while (true) {
            auto maybe_token = tokenizer.next_token();
            if (!maybe_token.has_value())
                break;
            ++total_tokens;
            if (!silent && i == 0)
                dump_token(maybe_token.value());
            if (maybe_token->is_end_of_file())
                break;
        }
    }

    if (iterations > 1 || silent) {
        auto elapsed_ms = timer.elapsed_milliseconds();
        warnln("input={}B iterations={} tokens/iter={} total={}ms ({:.3f}ms/iter)",
            input_data.size(),
            iterations,
            total_tokens / iterations,
            elapsed_ms,
            static_cast<double>(elapsed_ms) / iterations);
    }

    return 0;
}
