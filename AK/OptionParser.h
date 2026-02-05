/*
 * Copyright (c) 2020, Sergey Bugaev <bugaevc@serenityos.org>
 * Copyright (c) 2023, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Format.h>
#include <AK/StringView.h>
#include <AK/Vector.h>

namespace AK {

class OptionParser {
public:
    enum class ArgumentRequirement {
        NoArgument,
        HasOptionalArgument,
        HasRequiredArgument,
    };
    // Note: This is weird, but this class is used as a backend for getopt, so we're mirroring getopt here.
    struct Option {
        StringView name;
        ArgumentRequirement requirement;
        int* flag;
        int val;
    };

    struct GetOptResult {
        int result;                        // Whatever getopt is supposed to return.
        Optional<int> optopt_value;        // The new contents of `optopt' after this call
        Optional<StringView> optarg_value; // The new contents of `optarg' after this call
        size_t consumed_args;
    };

    GetOptResult getopt(Span<StringView> args, StringView short_options, Span<Option const> long_options, Optional<int&> out_long_option_index);
    void reset_state();

private:
    Optional<ArgumentRequirement> lookup_short_option_requirement(char option) const;
    int handle_short_option();

    Optional<Option const&> lookup_long_option(StringView raw) const;
    int handle_long_option();

    void shift_argv();
    bool find_next_option();

    StringView current_arg() const
    {
        if (m_arg_index >= m_args.size())
            return {};

        return m_args[m_arg_index];
    }

    template<typename... Args>
    static void reportln(CheckedFormatString<Args...> format_string, Args&&... args)
    {
        warnln(format_string.view(), forward<Args>(args)...);
    }

    // NOTE: These are ephemeral, and effectively only last for one call of `getopt()'.
    Span<StringView> m_args {};
    StringView m_short_options {};
    Span<Option const> m_long_options {};
    mutable Optional<int&> m_out_long_option_index {};
    mutable Optional<int> m_optopt_value {};
    mutable Optional<StringView> m_optarg_value {};

    size_t m_arg_index { 0 };
    size_t m_skipped_arguments { 0 };
    size_t m_consumed_args { 0 };
    size_t m_index_into_multioption_argument { 0 };
    bool m_stop_on_first_non_option { false };
};

}

#if USING_AK_GLOBALLY
using AK::OptionParser;
#endif
