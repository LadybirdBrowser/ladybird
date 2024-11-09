/*
 * Copyright (c) 2020, Sergey Bugaev <bugaevc@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Concepts.h>
#include <AK/Function.h>
#include <AK/Vector.h>
#include <LibMain/Main.h>
#include <stdio.h>

namespace Core {

class ArgsParser {
public:
    ArgsParser();

    enum class Required {
        Yes,
        No
    };

    enum class FailureBehavior {
        PrintUsageAndExit,
        PrintUsage,
        Exit,
        Ignore,
    };

    enum class OptionArgumentMode {
        None,
        Optional,
        Required,
    };

    /// When an option is hidden.
    /// If the hide mode is not None, then it's always hidden from the usage/synopsis.
    enum class OptionHideMode {
        None,
        Markdown,
        CommandLineAndMarkdown,
    };

    struct Option {
        OptionArgumentMode argument_mode { OptionArgumentMode::Required };
        char const* help_string { nullptr };
        char const* long_name { nullptr };
        char short_name { 0 };
        char const* value_name { nullptr };
        Function<ErrorOr<bool>(StringView)> accept_value;
        OptionHideMode hide_mode { OptionHideMode::None };

        ByteString name_for_display() const
        {
            if (long_name)
                return ByteString::formatted("--{}", long_name);
            return ByteString::formatted("-{:c}", short_name);
        }
    };

    struct Arg {
        char const* help_string { nullptr };
        char const* name { nullptr };
        int min_values { 0 };
        int max_values { 1 };
        Function<ErrorOr<bool>(StringView)> accept_value;
    };

    bool parse(Span<StringView> arguments, FailureBehavior failure_behavior = FailureBehavior::PrintUsageAndExit);
    bool parse(Main::Arguments const& arguments, FailureBehavior failure_behavior = FailureBehavior::PrintUsageAndExit)
    {
        return parse(arguments.strings, failure_behavior);
    }

    // *Without* trailing newline!
    void set_general_help(char const* help_string) { m_general_help = help_string; }
    void set_stop_on_first_non_option(bool stop_on_first_non_option) { m_stop_on_first_non_option = stop_on_first_non_option; }
    void print_usage(FILE*, StringView argv0);
    void print_usage_terminal(FILE*, StringView argv0);
    void print_usage_markdown(FILE*, StringView argv0);
    void print_version(FILE*);

    void add_option(Option&&);
    void add_ignored(char const* long_name, char short_name = 0, OptionHideMode hide_mode = OptionHideMode::None);
    void add_option(bool& value, char const* help_string, char const* long_name, char short_name = 0, OptionHideMode hide_mode = OptionHideMode::None);
    /// If the option is present, set the enum to have the given `new_value`.
    template<Enum T>
    void add_option(T& value, T new_value, char const* help_string, char const* long_name, char short_name = 0, OptionHideMode hide_mode = OptionHideMode::None)
    {
        add_option({ .argument_mode = Core::ArgsParser::OptionArgumentMode::None,
            .help_string = help_string,
            .long_name = long_name,
            .short_name = short_name,
            .accept_value = [&](StringView) {
                value = new_value;
                return true;
            },
            .hide_mode = hide_mode });
    }

    template<Integral I>
    void add_option(I& value, char const* help_string, char const* long_name, char short_name, char const* value_name, OptionHideMode hide_mode = OptionHideMode::None)
    {
        Option option {
            OptionArgumentMode::Required,
            help_string,
            long_name,
            short_name,
            value_name,
            [&value](StringView view) -> ErrorOr<bool> {
                Optional<I> opt = view.to_number<I>();
                value = opt.value_or(0);
                return opt.has_value();
            },
            hide_mode,
        };
        add_option(move(option));
    }

    template<Integral I>
    void add_option(Optional<I>& value, char const* help_string, char const* long_name, char short_name, char const* value_name, OptionHideMode hide_mode = OptionHideMode::None)
    {

        Option option {
            OptionArgumentMode::Required,
            help_string,
            long_name,
            short_name,
            value_name,
            [&value](StringView view) -> ErrorOr<bool> {
                value = view.to_number<I>();
                return value.has_value();
            },
            hide_mode,
        };
        add_option(move(option));
    }

    template<Integral I>
    void add_option(Vector<I>& values, char const* help_string, char const* long_name, char short_name, char const* value_name, char separator = ',', OptionHideMode hide_mode = OptionHideMode::None)
    {

        Option option {
            OptionArgumentMode::Required,
            help_string,
            long_name,
            short_name,
            value_name,
            [&values, separator](StringView s) -> ErrorOr<bool> {
                bool parsed_all_values = true;

                s.for_each_split_view(separator, SplitBehavior::Nothing, [&](auto value) {
                    if (auto maybe_value = value.template to_number<I>(); maybe_value.has_value())
                        values.append(*maybe_value);
                    else
                        parsed_all_values = false;
                });

                return parsed_all_values;
            },
            hide_mode
        };

        add_option(move(option));
    }
    void add_option(ByteString& value, char const* help_string, char const* long_name, char short_name, char const* value_name, OptionHideMode hide_mode = OptionHideMode::None);
    void add_option(String& value, char const* help_string, char const* long_name, char short_name, char const* value_name, OptionHideMode hide_mode = OptionHideMode::None);
    void add_option(StringView& value, char const* help_string, char const* long_name, char short_name, char const* value_name, OptionHideMode hide_mode = OptionHideMode::None);
    void add_option(Optional<StringView>& value, char const* help_string, char const* long_name, char short_name, char const* value_name, OptionHideMode hide_mode = OptionHideMode::None);
    void add_option(double& value, char const* help_string, char const* long_name, char short_name, char const* value_name, OptionHideMode hide_mode = OptionHideMode::None);
    void add_option(Optional<double>& value, char const* help_string, char const* long_name, char short_name, char const* value_name, OptionHideMode hide_mode = OptionHideMode::None);
    // Note: This option is being used when we expect the user to use the same option
    // multiple times (e.g. "program --option=example --option=anotherexample ...").
    void add_option(Vector<ByteString>& values, char const* help_string, char const* long_name, char short_name, char const* value_name, OptionHideMode hide_mode = OptionHideMode::None);

    void add_positional_argument(Arg&&);
    void add_positional_argument(ByteString& value, char const* help_string, char const* name, Required required = Required::Yes);
    void add_positional_argument(StringView& value, char const* help_string, char const* name, Required required = Required::Yes);
    void add_positional_argument(String& value, char const* help_string, char const* name, Required required = Required::Yes);
    template<Integral I>
    void add_positional_argument(I& value, char const* help_string, char const* name, Required required = Required::Yes)
    {
        Arg arg {
            help_string,
            name,
            required == Required::Yes ? 1 : 0,
            1,
            [&value](StringView view) -> ErrorOr<bool> {
                Optional<I> opt = view.to_number<I>();
                value = opt.value_or(0);
                return opt.has_value();
            },
        };
        add_positional_argument(move(arg));
    }
    void add_positional_argument(double& value, char const* help_string, char const* name, Required required = Required::Yes);
    void add_positional_argument(Vector<ByteString>& value, char const* help_string, char const* name, Required required = Required::Yes);
    void add_positional_argument(Vector<StringView>& value, char const* help_string, char const* name, Required required = Required::Yes);
    void add_positional_argument(Vector<String>& value, char const* help_string, char const* name, Required required = Required::Yes);

private:
    void autocomplete(FILE*, StringView program_name, ReadonlySpan<StringView> remaining_arguments);

    Vector<Option> m_options;
    Vector<Arg> m_positional_args;

    bool m_show_help { false };
    bool m_show_version { false };
    bool m_perform_autocomplete { false };
    char const* m_general_help { nullptr };
    bool m_stop_on_first_non_option { false };
};

}
