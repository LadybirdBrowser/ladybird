/*
 * Copyright (c) 2020, Emanuel Sprung <emanuel.sprung@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "RegexByteCode.h"
#include "RegexMatch.h"
#include "RegexOptions.h"
#include "RegexParser.h"

#include <AK/Forward.h>
#include <AK/GenericLexer.h>
#include <AK/HashMap.h>
#include <AK/Types.h>
#include <AK/Utf32View.h>
#include <AK/Vector.h>
#include <ctype.h>

#include <stdio.h>

namespace regex {

namespace Detail {

struct Block {
    size_t start;
    size_t end;
};

}

static constexpr size_t const c_max_recursion = 5000;
static constexpr size_t const c_match_preallocation_count = 0;

struct RegexResult final {
    bool success { false };
    size_t count { 0 };
    Vector<Match> matches;
    Vector<Vector<Match>> capture_group_matches;
    size_t n_operations { 0 };
    size_t n_capture_groups { 0 };
    size_t n_named_capture_groups { 0 };
};

template<class Parser>
class Regex;

template<class Parser>
class Matcher final {

public:
    Matcher(Regex<Parser> const* pattern, Optional<typename ParserTraits<Parser>::OptionsType> regex_options = {})
        : m_pattern(pattern)
        , m_regex_options(regex_options.value_or({}))
    {
    }
    ~Matcher() = default;

    RegexResult match(RegexStringView, Optional<typename ParserTraits<Parser>::OptionsType> = {}) const;
    RegexResult match(Vector<RegexStringView> const&, Optional<typename ParserTraits<Parser>::OptionsType> = {}) const;

    typename ParserTraits<Parser>::OptionsType options() const
    {
        return m_regex_options;
    }

    void reset_pattern(Badge<Regex<Parser>>, Regex<Parser> const* pattern)
    {
        m_pattern = pattern;
    }

private:
    bool execute(MatchInput const& input, MatchState& state, size_t& operations) const;

    Regex<Parser> const* m_pattern;
    typename ParserTraits<Parser>::OptionsType const m_regex_options;
};

template<class Parser>
class Regex final {
public:
    ByteString pattern_value;
    regex::Parser::Result parser_result;
    OwnPtr<Matcher<Parser>> matcher { nullptr };
    mutable size_t start_offset { 0 };

    static regex::Parser::Result parse_pattern(StringView pattern, typename ParserTraits<Parser>::OptionsType regex_options = {});

    explicit Regex(ByteString pattern, typename ParserTraits<Parser>::OptionsType regex_options = {});
    Regex(regex::Parser::Result parse_result, ByteString pattern, typename ParserTraits<Parser>::OptionsType regex_options = {});
    ~Regex() = default;
    Regex(Regex&&);
    Regex& operator=(Regex&&);

    typename ParserTraits<Parser>::OptionsType options() const;
    ByteString error_string(Optional<ByteString> message = {}) const;

    RegexResult match(RegexStringView view, Optional<typename ParserTraits<Parser>::OptionsType> regex_options = {}) const
    {
        if (!matcher || parser_result.error != Error::NoError)
            return {};
        return matcher->match(view, regex_options);
    }

    RegexResult match(Vector<RegexStringView> const& views, Optional<typename ParserTraits<Parser>::OptionsType> regex_options = {}) const
    {
        if (!matcher || parser_result.error != Error::NoError)
            return {};
        return matcher->match(views, regex_options);
    }

    ByteString replace(RegexStringView view, StringView replacement_pattern, Optional<typename ParserTraits<Parser>::OptionsType> regex_options = {}) const
    {
        if (!matcher || parser_result.error != Error::NoError)
            return {};

        StringBuilder builder;
        size_t start_offset = 0;
        RegexResult result = matcher->match(view, regex_options);
        if (!result.success)
            return view.to_byte_string();

        for (size_t i = 0; i < result.matches.size(); ++i) {
            auto& match = result.matches[i];
            builder.append(view.substring_view(start_offset, match.global_offset - start_offset).to_byte_string());
            start_offset = match.global_offset + match.view.length();
            GenericLexer lexer(replacement_pattern);
            while (!lexer.is_eof()) {
                if (lexer.consume_specific('\\')) {
                    if (lexer.consume_specific('\\')) {
                        builder.append('\\');
                        continue;
                    }
                    auto number = lexer.consume_while(isdigit);
                    if (auto index = number.to_number<unsigned>(); index.has_value() && result.n_capture_groups >= index.value()) {
                        builder.append(result.capture_group_matches[i][index.value() - 1].view.to_byte_string());
                    } else {
                        builder.appendff("\\{}", number);
                    }
                } else {
                    builder.append(lexer.consume_while([](auto ch) { return ch != '\\'; }));
                }
            }
        }

        builder.append(view.substring_view(start_offset, view.length() - start_offset).to_byte_string());

        return builder.to_byte_string();
    }

    // FIXME: replace(Vector<RegexStringView> const , ...)

    RegexResult search(RegexStringView view, Optional<typename ParserTraits<Parser>::OptionsType> regex_options = {}) const
    {
        if (!matcher || parser_result.error != Error::NoError)
            return {};

        AllOptions options = (AllOptions)regex_options.value_or({});
        if ((options & AllFlags::MatchNotBeginOfLine) && (options & AllFlags::MatchNotEndOfLine)) {
            options.reset_flag(AllFlags::MatchNotEndOfLine);
            options.reset_flag(AllFlags::MatchNotBeginOfLine);
        }
        options.reset_flag(AllFlags::Internal_Stateful);
        options |= AllFlags::Global;

        return matcher->match(view, options);
    }

    RegexResult search(Vector<RegexStringView> const& views, Optional<typename ParserTraits<Parser>::OptionsType> regex_options = {}) const
    {
        if (!matcher || parser_result.error != Error::NoError)
            return {};

        AllOptions options = (AllOptions)regex_options.value_or({});
        if ((options & AllFlags::MatchNotBeginOfLine) && (options & AllFlags::MatchNotEndOfLine)) {
            options.reset_flag(AllFlags::MatchNotEndOfLine);
            options.reset_flag(AllFlags::MatchNotBeginOfLine);
        }
        options.reset_flag(AllFlags::Internal_Stateful);
        options |= AllFlags::Global;

        return matcher->match(views, options);
    }

    bool match(RegexStringView view, RegexResult& m, Optional<typename ParserTraits<Parser>::OptionsType> regex_options = {}) const
    {
        m = match(view, regex_options);
        return m.success;
    }

    bool match(Vector<RegexStringView> const& views, RegexResult& m, Optional<typename ParserTraits<Parser>::OptionsType> regex_options = {}) const
    {
        m = match(views, regex_options);
        return m.success;
    }

    bool search(RegexStringView view, RegexResult& m, Optional<typename ParserTraits<Parser>::OptionsType> regex_options = {}) const
    {
        m = search(view, regex_options);
        return m.success;
    }

    bool search(Vector<RegexStringView> const& views, RegexResult& m, Optional<typename ParserTraits<Parser>::OptionsType> regex_options = {}) const
    {
        m = search(views, regex_options);
        return m.success;
    }

    bool has_match(RegexStringView view, Optional<typename ParserTraits<Parser>::OptionsType> regex_options = {}) const
    {
        if (!matcher || parser_result.error != Error::NoError)
            return false;
        RegexResult result = matcher->match(view, AllOptions { regex_options.value_or({}) } | AllFlags::SkipSubExprResults);
        return result.success;
    }

    bool has_match(Vector<RegexStringView> const& views, Optional<typename ParserTraits<Parser>::OptionsType> regex_options = {}) const
    {
        if (!matcher || parser_result.error != Error::NoError)
            return false;
        RegexResult result = matcher->match(views, AllOptions { regex_options.value_or({}) } | AllFlags::SkipSubExprResults);
        return result.success;
    }

    using BasicBlockList = Vector<Detail::Block>;
    static BasicBlockList split_basic_blocks(ByteCode const&);

private:
    void run_optimization_passes();
    void attempt_rewrite_loops_as_atomic_groups(BasicBlockList const&);
    bool attempt_rewrite_entire_match_as_substring_search(BasicBlockList const&);
};

// free standing functions for match, search and has_match
template<class Parser>
RegexResult match(RegexStringView view, Regex<Parser>& pattern, Optional<typename ParserTraits<Parser>::OptionsType> regex_options = {})
{
    return pattern.match(view, regex_options);
}

template<class Parser>
RegexResult match(Vector<RegexStringView> const& view, Regex<Parser>& pattern, Optional<typename ParserTraits<Parser>::OptionsType> regex_options = {})
{
    return pattern.match(view, regex_options);
}

template<class Parser>
bool match(RegexStringView view, Regex<Parser>& pattern, RegexResult&, Optional<typename ParserTraits<Parser>::OptionsType> regex_options = {})
{
    return pattern.match(view, regex_options);
}

template<class Parser>
bool match(Vector<RegexStringView> const& view, Regex<Parser>& pattern, RegexResult&, Optional<typename ParserTraits<Parser>::OptionsType> regex_options = {})
{
    return pattern.match(view, regex_options);
}

template<class Parser>
RegexResult search(RegexStringView view, Regex<Parser>& pattern, Optional<typename ParserTraits<Parser>::OptionsType> regex_options = {})
{
    return pattern.search(view, regex_options);
}

template<class Parser>
RegexResult search(Vector<RegexStringView> const& views, Regex<Parser>& pattern, Optional<typename ParserTraits<Parser>::OptionsType> regex_options = {})
{
    return pattern.search(views, regex_options);
}

template<class Parser>
bool search(RegexStringView view, Regex<Parser>& pattern, RegexResult&, Optional<typename ParserTraits<Parser>::OptionsType> regex_options = {})
{
    return pattern.search(view, regex_options);
}

template<class Parser>
bool search(Vector<RegexStringView> const& views, Regex<Parser>& pattern, RegexResult&, Optional<typename ParserTraits<Parser>::OptionsType> regex_options = {})
{
    return pattern.search(views, regex_options);
}

template<class Parser>
bool has_match(RegexStringView view, Regex<Parser>& pattern, Optional<typename ParserTraits<Parser>::OptionsType> regex_options = {})
{
    return pattern.has_match(view, regex_options);
}

template<class Parser>
bool has_match(Vector<RegexStringView> const& views, Regex<Parser>& pattern, Optional<typename ParserTraits<Parser>::OptionsType> regex_options = {})
{
    return pattern.has_match(views, regex_options);
}
}

using regex::has_match;
using regex::match;
using regex::Regex;
using regex::RegexResult;
