/*
 * Copyright (c) 2020, Sergey Bugaev <bugaevc@serenityos.org>
 * Copyright (c) 2022, Matthias Zimmerman <matthias291999@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <AK/JsonObject.h>
#include <AK/OptionParser.h>
#include <AK/String.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/Version.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#define TRY_OR_ERROR_IF_NOT_OOM(expr, user_input)                                              \
    ({                                                                                         \
        auto&& _value = expr;                                                                  \
        if (_value.is_error() && _value.error().is_errno() && _value.error().code() == ENOMEM) \
            return _value.release_error();                                                     \
        if (_value.is_error()) {                                                               \
            warnln("Error while processing argument '{}': {}", user_input, _value.error());    \
            return false;                                                                      \
        }                                                                                      \
        _value.release_value();                                                                \
    })

namespace Core {

ArgsParser::ArgsParser()
{
    add_option(m_show_help, "Display help message and exit", "help", 0, OptionHideMode::Markdown);
    add_option(m_show_version, "Print version", "version", 0, OptionHideMode::Markdown);
    add_option(m_perform_autocomplete, "Perform autocompletion", "complete", 0, OptionHideMode::CommandLineAndMarkdown);
}

bool ArgsParser::parse(Span<StringView> arguments, FailureBehavior failure_behavior)
{
    auto fail_impl = [this, failure_behavior](StringView name) {
        if (failure_behavior == FailureBehavior::PrintUsage || failure_behavior == FailureBehavior::PrintUsageAndExit)
            print_usage(stderr, name);
        if (failure_behavior == FailureBehavior::Exit || failure_behavior == FailureBehavior::PrintUsageAndExit)
            exit(1);
    };

    if (arguments.is_empty()) {
        fail_impl("<exe>"sv);
        return false;
    }

    auto fail = [name = arguments[0], &fail_impl] { fail_impl(name); };

    OptionParser parser;

    Vector<OptionParser::Option> long_options;
    StringBuilder short_options_builder;

    if (m_stop_on_first_non_option)
        short_options_builder.append('+');

    int index_of_found_long_option = -1;

    for (size_t i = 0; i < m_options.size(); i++) {
        auto& opt = m_options[i];
        if (opt.long_name) {
            OptionParser::Option long_opt {
                { opt.long_name, strlen(opt.long_name) },
                opt.argument_mode == OptionArgumentMode::Required
                    ? OptionParser::ArgumentRequirement::HasRequiredArgument
                    : opt.argument_mode == OptionArgumentMode::Optional
                    ? OptionParser::ArgumentRequirement::HasOptionalArgument
                    : OptionParser::ArgumentRequirement::NoArgument,
                &index_of_found_long_option,
                static_cast<int>(i)
            };
            long_options.append(long_opt);
        }
        if (opt.short_name) {
            short_options_builder.append(opt.short_name);
            if (opt.argument_mode != OptionArgumentMode::None)
                short_options_builder.append(':');
            // Note: This is a GNU extension.
            if (opt.argument_mode == OptionArgumentMode::Optional)
                short_options_builder.append(':');
        }
    }

    auto short_options = short_options_builder.to_byte_string();
    size_t option_index = 1;
    while (true) {
        auto result = parser.getopt(arguments.slice(1), short_options, long_options, {});
        option_index += result.consumed_args;

        auto c = result.result;
        if (c == -1) {
            // We have reached the end.
            break;
        }

        if (c == '?') {
            // There was an error, and getopt() has already
            // printed its error message.
            fail();
            return false;
        }

        // Let's see what option we just found.
        Option* found_option = nullptr;
        if (c == 0) {
            // It was a long option.
            VERIFY(index_of_found_long_option >= 0);
            found_option = &m_options[index_of_found_long_option];
            index_of_found_long_option = -1;
        } else {
            // It was a short option, look it up.
            auto it = m_options.find_if([c](auto& opt) { return c == opt.short_name; });
            VERIFY(!it.is_end());
            found_option = &*it;
        }
        VERIFY(found_option);

        StringView arg = found_option->argument_mode != OptionArgumentMode::None ? result.optarg_value.value_or({}) : StringView {};
        if (!MUST(found_option->accept_value(arg))) {
            warnln("\033[31mInvalid value for option \033[1m{}\033[22m\033[0m", found_option->name_for_display());
            fail();
            return false;
        }
    }

    // We're done processing options.
    // Now let's show version or help if requested, or perform autocompletion if needed.

    if (m_show_version) {
        print_version(stdout);
        if (failure_behavior == FailureBehavior::Exit || failure_behavior == FailureBehavior::PrintUsageAndExit)
            exit(0);
        return false;
    }

    if (m_show_help) {
        print_usage(stdout, arguments[0]);
        if (failure_behavior == FailureBehavior::Exit || failure_behavior == FailureBehavior::PrintUsageAndExit)
            exit(0);
        return false;
    }

    if (m_perform_autocomplete) {
        autocomplete(stdout, arguments[0], arguments.slice(option_index));
        if (failure_behavior == FailureBehavior::Exit || failure_behavior == FailureBehavior::PrintUsageAndExit)
            exit(0);
        return false;
    }

    // Now let's parse positional arguments.

    int values_left = arguments.size() - option_index;
    Vector<int, 16> num_values_for_arg;
    num_values_for_arg.resize(m_positional_args.size(), true);
    int total_values_required = 0;
    for (size_t i = 0; i < m_positional_args.size(); i++) {
        auto& arg = m_positional_args[i];
        num_values_for_arg[i] = arg.min_values;
        total_values_required += arg.min_values;
    }

    if (total_values_required > values_left) {
        fail();
        return false;
    }
    int extra_values_to_distribute = values_left - total_values_required;

    for (size_t i = 0; i < m_positional_args.size(); i++) {
        auto& arg = m_positional_args[i];
        int extra_values_to_this_arg = min(arg.max_values - arg.min_values, extra_values_to_distribute);
        num_values_for_arg[i] += extra_values_to_this_arg;
        extra_values_to_distribute -= extra_values_to_this_arg;
        if (extra_values_to_distribute == 0)
            break;
    }

    if (extra_values_to_distribute > 0) {
        // We still have too many values :(
        fail();
        return false;
    }

    for (size_t i = 0; i < m_positional_args.size(); i++) {
        auto& arg = m_positional_args[i];
        for (int j = 0; j < num_values_for_arg[i]; j++) {
            StringView value = arguments[option_index++];
            if (!MUST(arg.accept_value(value))) {
                warnln("Invalid value for argument {}", arg.name);
                fail();
                return false;
            }
        }
    }

    return true;
}

void ArgsParser::print_usage(FILE* file, StringView argv0)
{
    char const* env_preference = getenv("ARGSPARSER_EMIT_MARKDOWN");
    if (env_preference != nullptr && env_preference[0] == '1' && env_preference[1] == 0) {
        print_usage_markdown(file, argv0);
    } else {
        print_usage_terminal(file, argv0);
    }
}

void ArgsParser::print_usage_terminal(FILE* file, StringView argv0)
{
    out(file, "Usage:\n\t\033[1m{}\033[0m", argv0);

    for (auto& opt : m_options) {
        if (opt.hide_mode != OptionHideMode::None)
            continue;
        if (opt.argument_mode == OptionArgumentMode::Required)
            out(file, " [{} {}]", opt.name_for_display(), opt.value_name);
        else if (opt.argument_mode == OptionArgumentMode::Optional)
            out(file, " [{}[{}{}]]", opt.name_for_display(), opt.long_name ? "="sv : ""sv, opt.value_name);
        else
            out(file, " [{}]", opt.name_for_display());
    }
    for (auto& arg : m_positional_args) {
        bool required = arg.min_values > 0;
        bool repeated = arg.max_values > 1;

        if (required && repeated)
            out(file, " <{}...>", arg.name);
        else if (required && !repeated)
            out(file, " <{}>", arg.name);
        else if (!required && repeated)
            out(file, " [{}...]", arg.name);
        else if (!required && !repeated)
            out(file, " [{}]", arg.name);
    }
    outln(file);

    if (m_general_help != nullptr && m_general_help[0] != '\0') {
        outln(file, "\nDescription:");
        outln(file, "{}", m_general_help);
    }

    if (!m_options.is_empty())
        outln(file, "\nOptions:");
    for (auto& opt : m_options) {
        if (opt.hide_mode == OptionHideMode::CommandLineAndMarkdown)
            continue;

        auto print_argument = [&](StringView value_delimiter) {
            if (opt.value_name) {
                if (opt.argument_mode == OptionArgumentMode::Required)
                    out(file, " {}", opt.value_name);
                if (opt.argument_mode == OptionArgumentMode::Optional)
                    out(file, "[{}{}]", value_delimiter, opt.value_name);
            }
        };
        out(file, "\t");
        if (opt.short_name) {
            out(file, "\033[1m-{}\033[0m", opt.short_name);
            print_argument(""sv);
        }
        if (opt.short_name && opt.long_name)
            out(file, ", ");
        if (opt.long_name) {
            out(file, "\033[1m--{}\033[0m", opt.long_name);
            print_argument("="sv);
        }

        if (opt.help_string)
            out(file, "\t{}", opt.help_string);
        outln(file);
    }

    if (!m_positional_args.is_empty())
        outln(file, "\nArguments:");

    for (auto& arg : m_positional_args) {
        out(file, "\t\033[1m{}\033[0m", arg.name);
        if (arg.help_string)
            out(file, "\t{}", arg.help_string);
        outln(file);
    }
}

void ArgsParser::print_usage_markdown(FILE* file, StringView argv0)
{
    outln(file, "## Name\n\n{}", argv0);

    out(file, "\n## Synopsis\n\n```sh\n$ {}", argv0);
    for (auto& opt : m_options) {
        if (opt.hide_mode != OptionHideMode::None)
            continue;

        // FIXME: We allow opt.value_name to be empty even if the option
        //        requires an argument. This should be disallowed as it will
        //        currently display a blank name after the option.
        if (opt.argument_mode == OptionArgumentMode::Required)
            out(file, " [{} {}]", opt.name_for_display(), opt.value_name ?: "");
        else if (opt.argument_mode == OptionArgumentMode::Optional)
            out(file, " [{}[{}{}]]", opt.name_for_display(), opt.long_name ? "="sv : ""sv, opt.value_name);
        else
            out(file, " [{}]", opt.name_for_display());
    }
    for (auto& arg : m_positional_args) {
        bool required = arg.min_values > 0;
        bool repeated = arg.max_values > 1;

        if (required && repeated)
            out(file, " <{}...>", arg.name);
        else if (required && !repeated)
            out(file, " <{}>", arg.name);
        else if (!required && repeated)
            out(file, " [{}...]", arg.name);
        else if (!required && !repeated)
            out(file, " [{}]", arg.name);
    }
    outln(file, "\n```");

    if (m_general_help != nullptr && m_general_help[0] != '\0') {
        outln(file, "\n## Description\n\n{}", m_general_help);
    }

    auto should_display_option = [](Option& opt) {
        return !(opt.hide_mode == OptionHideMode::Markdown || opt.hide_mode == OptionHideMode::CommandLineAndMarkdown);
    };

    size_t options_to_display = 0;
    for (auto& opt : m_options) {
        if (!should_display_option(opt))
            continue;
        options_to_display++;
    }

    if (options_to_display > 0)
        outln(file, "\n## Options\n");
    for (auto& opt : m_options) {
        if (!should_display_option(opt))
            continue;

        auto print_argument = [&](StringView value_delimiter) {
            if (opt.value_name != nullptr) {
                if (opt.argument_mode == OptionArgumentMode::Required)
                    out(file, " {}", opt.value_name);
                if (opt.argument_mode == OptionArgumentMode::Optional)
                    out(file, "[{}{}]", value_delimiter, opt.value_name);
            }
        };
        out(file, "* ");
        if (opt.short_name != '\0') {
            out(file, "`-{}", opt.short_name);
            print_argument(""sv);
            out(file, "`");
        }
        if (opt.short_name != '\0' && opt.long_name != nullptr)
            out(file, ", ");
        if (opt.long_name != nullptr) {
            out(file, "`--{}", opt.long_name);
            print_argument("="sv);
            out(file, "`");
        }

        if (opt.help_string != nullptr)
            out(file, ": {}", opt.help_string);
        outln(file);
    }

    if (!m_positional_args.is_empty())
        outln(file, "\n## Arguments\n");

    for (auto& arg : m_positional_args) {
        out(file, "* `{}`", arg.name);
        if (arg.help_string != nullptr)
            out(file, ": {}", arg.help_string);
        outln(file);
    }
}

void ArgsParser::print_version(FILE* file)
{
    // FIXME: Allow applications to override version string for --version.
    //        Especially useful for Lagom applications
    outln(file, "{}", Core::Version::read_long_version_string());
}

void ArgsParser::add_option(Option&& option)
{
    for (auto const& existing_option : m_options) {
        if (option.long_name && existing_option.long_name == option.long_name) {
            warnln("Error: Multiple options have the long name \"--{}\"", option.long_name);
            dbgln("Error: Multiple options have the long name \"--{}\"", option.long_name);
            VERIFY_NOT_REACHED();
        }
        if (option.short_name && existing_option.short_name == option.short_name) {
            warnln("Error: Multiple options have the short name \"-{}\"", option.short_name);
            dbgln("Error: Multiple options have the short name \"-{}\"", option.short_name);
            VERIFY_NOT_REACHED();
        }
    }
    m_options.append(move(option));
}

void ArgsParser::add_ignored(char const* long_name, char short_name, OptionHideMode hide_mode)
{
    Option option {
        OptionArgumentMode::None,
        "Ignored",
        long_name,
        short_name,
        nullptr,
        [](StringView) -> ErrorOr<bool> {
            return true;
        },
        hide_mode,
    };
    add_option(move(option));
}

void ArgsParser::add_option(bool& value, char const* help_string, char const* long_name, char short_name, OptionHideMode hide_mode)
{
    Option option {
        OptionArgumentMode::None,
        help_string,
        long_name,
        short_name,
        nullptr,
        [&value](StringView s) -> ErrorOr<bool> {
            VERIFY(s.is_empty());
            value = true;
            return true;
        },
        hide_mode,
    };
    add_option(move(option));
}

void ArgsParser::add_option(ByteString& value, char const* help_string, char const* long_name, char short_name, char const* value_name, OptionHideMode hide_mode)
{
    Option option {
        OptionArgumentMode::Required,
        help_string,
        long_name,
        short_name,
        value_name,
        [&value](StringView s) -> ErrorOr<bool> {
            value = s;
            return true;
        },
        hide_mode,
    };
    add_option(move(option));
}

void ArgsParser::add_option(String& value, char const* help_string, char const* long_name, char short_name, char const* value_name, OptionHideMode hide_mode)
{
    Option option {
        OptionArgumentMode::Required,
        help_string,
        long_name,
        short_name,
        value_name,
        [&value](StringView s) -> ErrorOr<bool> {
            value = TRY_OR_ERROR_IF_NOT_OOM(String::from_utf8(s), s);
            return true;
        },
        hide_mode,
    };
    add_option(move(option));
}

void ArgsParser::add_option(StringView& value, char const* help_string, char const* long_name, char short_name, char const* value_name, OptionHideMode hide_mode)
{
    Option option {
        OptionArgumentMode::Required,
        help_string,
        long_name,
        short_name,
        value_name,
        [&value](StringView s) -> ErrorOr<bool> {
            value = s;
            return true;
        },
        hide_mode,
    };
    add_option(move(option));
}

void ArgsParser::add_option(Optional<StringView>& value, char const* help_string, char const* long_name, char short_name, char const* value_name, OptionHideMode hide_mode)
{
    Option option {
        OptionArgumentMode::Required,
        help_string,
        long_name,
        short_name,
        value_name,
        [&value](StringView s) -> ErrorOr<bool> {
            value = s;
            return true;
        },
        hide_mode,
    };
    add_option(move(option));
}

void ArgsParser::add_option(double& value, char const* help_string, char const* long_name, char short_name, char const* value_name, OptionHideMode hide_mode)
{
    Option option {
        OptionArgumentMode::Required,
        help_string,
        long_name,
        short_name,
        value_name,
        [&value](StringView s) -> ErrorOr<bool> {
            auto opt = s.to_number<double>();
            value = opt.value_or(0.0);
            return opt.has_value();
        },
        hide_mode,
    };
    add_option(move(option));
}

void ArgsParser::add_option(Optional<double>& value, char const* help_string, char const* long_name, char short_name, char const* value_name, OptionHideMode hide_mode)
{
    Option option {
        OptionArgumentMode::Required,
        help_string,
        long_name,
        short_name,
        value_name,
        [&value](StringView s) -> ErrorOr<bool> {
            value = s.to_number<double>();
            return value.has_value();
        },
        hide_mode,
    };
    add_option(move(option));
}

void ArgsParser::add_option(Vector<ByteString>& values, char const* help_string, char const* long_name, char short_name, char const* value_name, OptionHideMode hide_mode)
{
    Option option {
        OptionArgumentMode::Required,
        help_string,
        long_name,
        short_name,
        value_name,
        [&values](StringView s) -> ErrorOr<bool> {
            TRY_OR_ERROR_IF_NOT_OOM(values.try_append(s), s);
            return true;
        },
        hide_mode
    };

    add_option(move(option));
}

void ArgsParser::add_positional_argument(Arg&& arg)
{
    m_positional_args.append(move(arg));
}

void ArgsParser::add_positional_argument(ByteString& value, char const* help_string, char const* name, Required required)
{
    Arg arg {
        help_string,
        name,
        required == Required::Yes ? 1 : 0,
        1,
        [&value](StringView s) -> ErrorOr<bool> {
            value = s;
            return true;
        }
    };
    add_positional_argument(move(arg));
}

void ArgsParser::add_positional_argument(StringView& value, char const* help_string, char const* name, Required required)
{
    Arg arg {
        help_string,
        name,
        required == Required::Yes ? 1 : 0,
        1,
        [&value](StringView s) -> ErrorOr<bool> {
            value = s;
            return true;
        }
    };
    add_positional_argument(move(arg));
}

void ArgsParser::add_positional_argument(String& value, char const* help_string, char const* name, Required required)
{
    Arg arg {
        help_string,
        name,
        required == Required::Yes ? 1 : 0,
        1,
        [&value](StringView s) -> ErrorOr<bool> {
            value = TRY_OR_ERROR_IF_NOT_OOM(String::from_utf8(s), s);
            return true;
        }
    };
    add_positional_argument(move(arg));
}

void ArgsParser::add_positional_argument(double& value, char const* help_string, char const* name, Required required)
{
    Arg arg {
        help_string,
        name,
        required == Required::Yes ? 1 : 0,
        1,
        [&value](StringView s) -> ErrorOr<bool> {
            auto opt = s.to_number<double>();
            value = opt.value_or(0.0);
            return opt.has_value();
        }
    };
    add_positional_argument(move(arg));
}

void ArgsParser::add_positional_argument(Vector<ByteString>& values, char const* help_string, char const* name, Required required)
{
    Arg arg {
        help_string,
        name,
        required == Required::Yes ? 1 : 0,
        INT_MAX,
        [&values](StringView s) -> ErrorOr<bool> {
            TRY_OR_ERROR_IF_NOT_OOM(values.try_append(s), s);
            return true;
        }
    };
    add_positional_argument(move(arg));
}

void ArgsParser::add_positional_argument(Vector<StringView>& values, char const* help_string, char const* name, Required required)
{
    Arg arg {
        help_string,
        name,
        required == Required::Yes ? 1 : 0,
        INT_MAX,
        [&values](StringView s) -> ErrorOr<bool> {
            TRY_OR_ERROR_IF_NOT_OOM(values.try_append(s), s);
            return true;
        }
    };
    add_positional_argument(move(arg));
}

void ArgsParser::add_positional_argument(Vector<String>& values, char const* help_string, char const* name, Required required)
{
    Arg arg {
        help_string,
        name,
        required == Required::Yes ? 1 : 0,
        INT_MAX,
        [&values](StringView s) -> ErrorOr<bool> {
            TRY_OR_ERROR_IF_NOT_OOM(values.try_append(TRY(String::from_utf8(s))), s);
            return true;
        }
    };
    add_positional_argument(move(arg));
}

void ArgsParser::autocomplete(FILE* file, StringView program_name, ReadonlySpan<StringView> remaining_arguments)
{
    // We expect the full invocation of the program to be available as positional args,
    // e.g. `foo --bar arg -b` (program invoked as `foo --complete -- foo --bar arg -b`)
    auto first = true;
    auto seen_all_options = false;
    auto skip_next = false;

    StringView argument_to_complete;
    StringView option_to_complete;
    auto completing_option = false;

    for (auto& argument : remaining_arguments) {
        completing_option = false;
        if (skip_next) {
            argument_to_complete = argument;
            skip_next = false;
            continue;
        }

        // Skip over the program name.
        if (first && program_name == argument) {
            first = false;
            continue;
        }

        if (seen_all_options) {
            argument_to_complete = argument;
            continue;
        }

        if (argument.starts_with("--"sv)) {
            option_to_complete = argument;
            completing_option = true;

            if (argument == "--") {
                seen_all_options = true;
                continue;
            }

            // Look for a long option
            auto option_pattern = argument.substring_view(2);
            auto it = m_options.find_if([&](auto& option) { return (option.hide_mode != OptionHideMode::None) && StringView { option.long_name, strlen(option.long_name) } == option_pattern; });
            if (it.is_end())
                continue;

            if (it->argument_mode == OptionArgumentMode::Required)
                skip_next = true;
            continue;
        }

        if (argument.starts_with('-')) {
            option_to_complete = argument;
            completing_option = true;

            if (argument == "-") {
                option_to_complete = argument;
                continue;
            }

            // Look for a short option
            auto option_pattern = argument[argument.length() - 1];
            auto it = m_options.find_if([&](auto& option) { return option.hide_mode != OptionHideMode::None && option.short_name == option_pattern; });

            if (it.is_end())
                continue;

            if (it->argument_mode == OptionArgumentMode::Required)
                skip_next = true;
            continue;
        }
    }

    // We don't know how to complete arguments quite yet.
    if (!completing_option)
        return;

    auto write_completion = [&](auto format, auto& option, auto has_invariant, auto... args) {
        JsonObject object;
        object.set("completion"sv, MUST(String::formatted(StringView { format, strlen(format) }, args...)));
        object.set("static_offset"sv, 0);
        object.set("invariant_offset"sv, has_invariant ? option_to_complete.length() : 0u);
        object.set("display_trivia"sv, StringView { option.help_string, strlen(option.help_string) });
        object.set("trailing_trivia"sv, option.argument_mode == OptionArgumentMode::Required ? " "sv : ""sv);
        outln(file, "{}", object.serialized());
    };

    if (option_to_complete.starts_with("--"sv)) {
        // Complete a long option.
        auto option_pattern = option_to_complete.substring_view(2);
        for (auto& option : m_options) {
            if (option.hide_mode != OptionHideMode::None)
                continue;
            StringView option_string { option.long_name, strlen(option.long_name) };
            if (option_string.starts_with(option_pattern)) {
                write_completion("--{}", option, true, option_string);
            }
        }
    } else {
        // Complete a short option, note that we're not going to attempt to 'match' anything here.
        for (auto& option : m_options) {
            if (option.hide_mode != OptionHideMode::None)
                continue;
            if (option.short_name == 0)
                continue;

            auto has_invariant = option_to_complete == "-";
            write_completion("{}{}", option, has_invariant, has_invariant ? "-" : "", option.short_name);
        }
    }
}

}
