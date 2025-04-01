/*
 * Copyright (c) 2020, Emanuel Sprung <emanuel.sprung@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "RegexByteCode.h"
#include "RegexError.h"
#include "RegexLexer.h"
#include "RegexOptions.h"

#include <AK/DeprecatedFlyString.h>
#include <AK/Forward.h>
#include <AK/HashMap.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibUnicode/Forward.h>

namespace regex {

class PosixExtendedParser;
class PosixBasicParser;
class ECMA262Parser;

template<typename T>
struct GenericParserTraits {
    using OptionsType = T;
};

template<typename T>
struct ParserTraits : public GenericParserTraits<T> {
};

template<>
struct ParserTraits<PosixExtendedParser> : public GenericParserTraits<PosixOptions> {
};

template<>
struct ParserTraits<PosixBasicParser> : public GenericParserTraits<PosixOptions> {
};

template<>
struct ParserTraits<ECMA262Parser> : public GenericParserTraits<ECMAScriptOptions> {
};

struct NamedCaptureGroup {
    size_t group_index;
    size_t alternative_id;
};

class Parser {
public:
    struct Result {
        ByteCode bytecode;
        size_t capture_groups_count;
        size_t named_capture_groups_count;
        size_t match_length_minimum;
        Error error;
        Token error_token;
        Vector<DeprecatedFlyString> capture_groups;
        AllOptions options;

        struct {
            Optional<ByteString> pure_substring_search;
            bool only_start_of_line = false;
        } optimization_data {};
    };

    explicit Parser(Lexer& lexer)
        : m_parser_state(lexer)
    {
    }

    Parser(Lexer& lexer, AllOptions regex_options)
        : m_parser_state(lexer, regex_options)
    {
    }

    virtual ~Parser() = default;

    Result parse(Optional<AllOptions> regex_options = {});
    bool has_error() const { return m_parser_state.error != Error::NoError; }
    Error error() const { return m_parser_state.error; }
    AllOptions options() const { return m_parser_state.regex_options; }

protected:
    virtual bool parse_internal(ByteCode&, size_t& match_length_minimum) = 0;

    ALWAYS_INLINE bool match(TokenType type) const;
    ALWAYS_INLINE bool match(char ch) const;
    ALWAYS_INLINE bool match_ordinary_characters();
    ALWAYS_INLINE Token consume();
    ALWAYS_INLINE Token consume(TokenType type, Error error);
    ALWAYS_INLINE bool consume(ByteString const&);
    ALWAYS_INLINE Optional<u32> consume_escaped_code_point(bool unicode);
    ALWAYS_INLINE bool try_skip(StringView);
    ALWAYS_INLINE bool lookahead_any(StringView);
    ALWAYS_INLINE unsigned char skip();
    ALWAYS_INLINE void back(size_t = 1);
    ALWAYS_INLINE void reset();
    ALWAYS_INLINE bool done() const;
    ALWAYS_INLINE bool set_error(Error error);

    size_t tell() const { return m_parser_state.current_token.position(); }

    struct ParserState {
        Lexer& lexer;
        Token current_token;
        Error error = Error::NoError;
        Token error_token { TokenType::Eof, 0, {} };
        ByteCode bytecode;
        size_t capture_groups_count { 0 };
        size_t named_capture_groups_count { 0 };
        size_t match_length_minimum { 0 };
        size_t repetition_mark_count { 0 };
        AllOptions regex_options;
        HashMap<size_t, size_t> capture_group_minimum_lengths;
        HashMap<DeprecatedFlyString, Vector<NamedCaptureGroup>> named_capture_groups;

        explicit ParserState(Lexer& lexer)
            : lexer(lexer)
            , current_token(lexer.next())
        {
        }
        explicit ParserState(Lexer& lexer, AllOptions regex_options)
            : lexer(lexer)
            , current_token(lexer.next())
            , regex_options(regex_options)
        {
        }
    };

    ParserState m_parser_state;
};

class AbstractPosixParser : public Parser {
protected:
    explicit AbstractPosixParser(Lexer& lexer)
        : Parser(lexer)
    {
    }

    AbstractPosixParser(Lexer& lexer, Optional<typename ParserTraits<PosixExtendedParser>::OptionsType> regex_options)
        : Parser(lexer, regex_options.value_or({}))
    {
    }

    ALWAYS_INLINE bool parse_bracket_expression(Vector<CompareTypeAndValuePair>&, size_t&);
};

class PosixBasicParser final : public AbstractPosixParser {
public:
    explicit PosixBasicParser(Lexer& lexer)
        : AbstractPosixParser(lexer)
    {
    }

    PosixBasicParser(Lexer& lexer, Optional<typename ParserTraits<PosixBasicParser>::OptionsType> regex_options)
        : AbstractPosixParser(lexer, regex_options.value_or({}))
    {
    }

    ~PosixBasicParser() = default;

private:
    bool parse_internal(ByteCode&, size_t&) override;

    bool parse_root(ByteCode&, size_t&);
    bool parse_re_expression(ByteCode&, size_t&);
    bool parse_simple_re(ByteCode&, size_t&);
    bool parse_nonduplicating_re(ByteCode&, size_t&);
    bool parse_one_char_or_collation_element(ByteCode&, size_t&);

    constexpr static size_t number_of_addressable_capture_groups = 9;
    size_t m_capture_group_minimum_lengths[number_of_addressable_capture_groups] { 0 };
    bool m_capture_group_seen[number_of_addressable_capture_groups] { false };
    size_t m_current_capture_group_depth { 0 };
};

class PosixExtendedParser final : public AbstractPosixParser {
    constexpr static auto default_options = static_cast<PosixFlags>(AllFlags::SingleLine) | static_cast<PosixFlags>(AllFlags::Internal_ConsiderNewline);

public:
    explicit PosixExtendedParser(Lexer& lexer)
        : AbstractPosixParser(lexer, default_options)
    {
    }

    PosixExtendedParser(Lexer& lexer, Optional<typename ParserTraits<PosixExtendedParser>::OptionsType> regex_options)
        : AbstractPosixParser(lexer, regex_options.value_or({}) | default_options.value())
    {
    }

    ~PosixExtendedParser() = default;

private:
    ALWAYS_INLINE bool match_repetition_symbol();

    bool parse_internal(ByteCode&, size_t&) override;

    bool parse_root(ByteCode&, size_t&);
    ALWAYS_INLINE bool parse_sub_expression(ByteCode&, size_t&);
    ALWAYS_INLINE bool parse_bracket_expression(ByteCode&, size_t&);
    ALWAYS_INLINE bool parse_repetition_symbol(ByteCode&, size_t&);
};

class ECMA262Parser final : public Parser {
    constexpr static ECMAScriptOptions default_options = static_cast<ECMAScriptFlags>(AllFlags::Internal_ConsiderNewline);

public:
    explicit ECMA262Parser(Lexer& lexer)
        : Parser(lexer, default_options)
    {
        m_capture_groups_in_scope.empend();
    }

    ECMA262Parser(Lexer& lexer, Optional<typename ParserTraits<ECMA262Parser>::OptionsType> regex_options)
        : Parser(lexer, regex_options.value_or({}) | default_options.value())
    {
        m_should_use_browser_extended_grammar = regex_options.has_value() && regex_options->has_flag_set(ECMAScriptFlags::BrowserExtended);
        m_capture_groups_in_scope.empend();
    }

    ~ECMA262Parser() = default;

private:
    bool parse_internal(ByteCode&, size_t&) override;

    struct ParseFlags {
        bool unicode { false };
        bool named { false };
        bool unicode_sets { false };
    };

    enum class ReadDigitsInitialZeroState {
        Allow,
        Disallow,
    };
    StringView read_digits_as_string(ReadDigitsInitialZeroState initial_zero = ReadDigitsInitialZeroState::Allow, bool hex = false, int max_count = -1, int min_count = -1);
    Optional<unsigned> read_digits(ReadDigitsInitialZeroState initial_zero = ReadDigitsInitialZeroState::Allow, bool hex = false, int max_count = -1, int min_count = -1);
    DeprecatedFlyString read_capture_group_specifier(bool take_starting_angle_bracket = false);

    struct Script {
        Unicode::Script script {};
        bool is_extension { false };
    };
    using PropertyEscape = Variant<Unicode::Property, Unicode::GeneralCategory, Script, Empty>;
    Optional<PropertyEscape> read_unicode_property_escape();

    bool parse_pattern(ByteCode&, size_t&, ParseFlags);
    bool parse_disjunction(ByteCode&, size_t&, ParseFlags);
    bool parse_alternative(ByteCode&, size_t&, ParseFlags);
    bool parse_term(ByteCode&, size_t&, ParseFlags);
    bool parse_assertion(ByteCode&, size_t&, ParseFlags);
    bool parse_atom(ByteCode&, size_t&, ParseFlags);
    bool parse_quantifier(ByteCode&, size_t&, ParseFlags);
    bool parse_interval_quantifier(Optional<u64>& repeat_min, Optional<u64>& repeat_max);
    bool parse_atom_escape(ByteCode&, size_t&, ParseFlags);
    bool parse_character_class(ByteCode&, size_t&, ParseFlags);
    bool parse_capture_group(ByteCode&, size_t&, ParseFlags);
    Optional<CharClass> parse_character_class_escape(bool& out_inverse, bool expect_backslash = false);
    bool parse_nonempty_class_ranges(Vector<CompareTypeAndValuePair>&, ParseFlags);
    bool parse_unicode_property_escape(PropertyEscape& property, bool& negated);

    bool parse_character_escape(Vector<CompareTypeAndValuePair>&, size_t&, ParseFlags);

    bool parse_class_set_expression(Vector<CompareTypeAndValuePair>&);
    bool parse_class_union(Vector<CompareTypeAndValuePair>&);
    bool parse_class_intersection(Vector<CompareTypeAndValuePair>&);
    bool parse_class_subtraction(Vector<CompareTypeAndValuePair>&);
    bool parse_class_set_range(Vector<CompareTypeAndValuePair>&);
    bool parse_class_set_operand(Vector<CompareTypeAndValuePair>&);
    bool parse_nested_class(Vector<CompareTypeAndValuePair>&);
    Optional<u32> parse_class_set_character();

    // Used only by B.1.4, Regular Expression Patterns (Extended for use in browsers)
    bool parse_quantifiable_assertion(ByteCode&, size_t&, ParseFlags);
    bool parse_extended_atom(ByteCode&, size_t&, ParseFlags);
    bool parse_inner_disjunction(ByteCode& bytecode_stack, size_t& length, ParseFlags);
    bool parse_invalid_braced_quantifier(); // Note: This function either parses and *fails*, or doesn't parse anything and returns false.
    Optional<u8> parse_legacy_octal_escape();

    bool has_duplicate_in_current_alternative(DeprecatedFlyString const& name);

    size_t ensure_total_number_of_capturing_parenthesis();

    void enter_capture_group_scope() { m_capture_groups_in_scope.empend(); }

    void exit_capture_group_scope()
    {
        auto last = m_capture_groups_in_scope.take_last();
        m_capture_groups_in_scope.last().extend(move(last));
    }

    void clear_all_capture_groups_in_scope(ByteCode& stack)
    {
        for (auto& index : m_capture_groups_in_scope.last())
            stack.insert_bytecode_clear_capture_group(index);
    }

    // ECMA-262's flavour of regex is a bit weird in that it allows backrefs to reference "future" captures, and such backrefs
    // always match the empty string. So we have to know how many capturing parenthesis there are, but we don't want to always
    // parse it twice, so we'll just do so when it's actually needed.
    // Most patterns should have no need to ever populate this field.
    Optional<size_t> m_total_number_of_capturing_parenthesis;

    // We need to keep track of the current alternative's named capture groups, so we can check for duplicates.
    size_t m_current_alternative_id { 0 };

    // Keep the Annex B. behavior behind a flag, the users can enable it by passing the `ECMAScriptFlags::BrowserExtended` flag.
    bool m_should_use_browser_extended_grammar { false };

    // ECMA-262 basically requires that we clear the inner captures of a capture group before trying to match it,
    // by requiring that (...)+ only contain the matches for the last iteration.
    // To do that, we have to keep track of which capture groups are "in scope", so we can clear them as needed.
    Vector<Vector<size_t>> m_capture_groups_in_scope;
};

using PosixExtended = PosixExtendedParser;
using PosixBasic = PosixBasicParser;
using ECMA262 = ECMA262Parser;

}

using regex::ECMA262;
using regex::PosixBasic;
using regex::PosixExtended;
