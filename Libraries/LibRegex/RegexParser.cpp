/*
 * Copyright (c) 2020, Emanuel Sprung <emanuel.sprung@gmail.com>
 * Copyright (c) 2020-2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "RegexParser.h"
#include "RegexDebug.h"
#include <AK/AnyOf.h>
#include <AK/ByteString.h>
#include <AK/CharacterTypes.h>
#include <AK/Debug.h>
#include <AK/DeprecatedFlyString.h>
#include <AK/GenericLexer.h>
#include <AK/ScopeGuard.h>
#include <AK/StringBuilder.h>
#include <AK/StringUtils.h>
#include <AK/TemporaryChange.h>
#include <LibUnicode/CharacterTypes.h>

namespace regex {

static constexpr size_t s_maximum_repetition_count = 1024 * 1024;
static constexpr u64 s_ecma262_maximum_repetition_count = (1ull << 53) - 1;
static constexpr auto s_alphabetic_characters = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"sv;
static constexpr auto s_decimal_characters = "0123456789"sv;

static constexpr StringView identity_escape_characters(bool unicode, bool browser_extended)
{
    if (unicode)
        return "^$\\.*+?()[]{}|/"sv;
    if (browser_extended)
        return "^$\\.*+?()[|"sv;
    return "^$\\.*+?()[]{}|"sv;
}

ALWAYS_INLINE bool Parser::set_error(Error error)
{
    if (m_parser_state.error == Error::NoError) {
        m_parser_state.error = error;
        m_parser_state.error_token = m_parser_state.current_token;
    }
    return false; // always return false, that eases the API usage (return set_error(...)) :^)
}

ALWAYS_INLINE bool Parser::done() const
{
    return match(TokenType::Eof);
}

ALWAYS_INLINE bool Parser::match(TokenType type) const
{
    return m_parser_state.current_token.type() == type;
}

ALWAYS_INLINE bool Parser::match(char ch) const
{
    return m_parser_state.current_token.type() == TokenType::Char && m_parser_state.current_token.value().length() == 1 && m_parser_state.current_token.value()[0] == ch;
}

ALWAYS_INLINE Token Parser::consume()
{
    auto old_token = m_parser_state.current_token;
    m_parser_state.current_token = m_parser_state.lexer.next();
    return old_token;
}

ALWAYS_INLINE Token Parser::consume(TokenType type, Error error)
{
    if (m_parser_state.current_token.type() != type) {
        set_error(error);
        dbgln_if(REGEX_DEBUG, "[PARSER] Error: Unexpected token {}. Expected: {}", m_parser_state.current_token.name(), Token::name(type));
    }
    return consume();
}

ALWAYS_INLINE bool Parser::consume(ByteString const& str)
{
    size_t potentially_go_back { 1 };
    for (auto ch : str) {
        if (match(TokenType::Char)) {
            if (m_parser_state.current_token.value()[0] != ch) {
                m_parser_state.lexer.back(potentially_go_back);
                m_parser_state.current_token = m_parser_state.lexer.next();
                return false;
            }
        } else {
            m_parser_state.lexer.back(potentially_go_back);
            m_parser_state.current_token = m_parser_state.lexer.next();
            return false;
        }
        consume(TokenType::Char, Error::NoError);
        ++potentially_go_back;
    }
    return true;
}

ALWAYS_INLINE Optional<u32> Parser::consume_escaped_code_point(bool unicode)
{
    if (match(TokenType::LeftCurly) && !unicode) {
        // In non-Unicode mode, this should be parsed as a repetition symbol (repeating the 'u').
        return static_cast<u32>('u');
    }

    m_parser_state.lexer.retreat(2 + !done()); // Go back to just before '\u' (+1 char, because we will have consumed an extra character)

    if (auto code_point_or_error = m_parser_state.lexer.consume_escaped_code_point(unicode); !code_point_or_error.is_error()) {
        m_parser_state.current_token = m_parser_state.lexer.next();
        return code_point_or_error.value();
    }

    if (!unicode) {
        // '\u' is allowed in non-unicode mode, just matches 'u'.
        return static_cast<u32>('u');
    }

    set_error(Error::InvalidPattern);
    return {};
}

ALWAYS_INLINE bool Parser::try_skip(StringView str)
{
    if (str.starts_with(m_parser_state.current_token.value()))
        str = str.substring_view(m_parser_state.current_token.value().length(), str.length() - m_parser_state.current_token.value().length());
    else
        return false;

    size_t potentially_go_back { 0 };
    for (auto ch : str) {
        if (!m_parser_state.lexer.consume_specific(ch)) {
            m_parser_state.lexer.back(potentially_go_back);
            return false;
        }
        ++potentially_go_back;
    }

    m_parser_state.current_token = m_parser_state.lexer.next();
    return true;
}

ALWAYS_INLINE bool Parser::lookahead_any(StringView str)
{
    return AK::any_of(str, [this](auto ch) { return match(ch); });
}

ALWAYS_INLINE unsigned char Parser::skip()
{
    unsigned char ch;
    if (m_parser_state.current_token.value().length() == 1) {
        ch = m_parser_state.current_token.value()[0];
    } else {
        m_parser_state.lexer.back(m_parser_state.current_token.value().length());
        ch = m_parser_state.lexer.consume();
    }

    m_parser_state.current_token = m_parser_state.lexer.next();
    return ch;
}

ALWAYS_INLINE void Parser::back(size_t count)
{
    m_parser_state.lexer.back(count);
    m_parser_state.current_token = m_parser_state.lexer.next();
}

ALWAYS_INLINE void Parser::reset()
{
    m_parser_state.bytecode.clear();
    m_parser_state.lexer.reset();
    m_parser_state.current_token = m_parser_state.lexer.next();
    m_parser_state.error = Error::NoError;
    m_parser_state.error_token = { TokenType::Eof, 0, {} };
    m_parser_state.capture_group_minimum_lengths.clear();
    m_parser_state.capture_groups_count = 0;
    m_parser_state.named_capture_groups_count = 0;
    m_parser_state.named_capture_groups.clear();
}

Parser::Result Parser::parse(Optional<AllOptions> regex_options)
{
    ByteCode::reset_checkpoint_serial_id();

    reset();
    if (regex_options.has_value())
        m_parser_state.regex_options = regex_options.value();
    if (parse_internal(m_parser_state.bytecode, m_parser_state.match_length_minimum))
        consume(TokenType::Eof, Error::InvalidPattern);
    else
        set_error(Error::InvalidPattern);

    dbgln_if(REGEX_DEBUG, "[PARSER] Produced bytecode with {} entries (opcodes + arguments)", m_parser_state.bytecode.size());
    return {
        move(m_parser_state.bytecode),
        move(m_parser_state.capture_groups_count),
        move(m_parser_state.named_capture_groups_count),
        move(m_parser_state.match_length_minimum),
        move(m_parser_state.error),
        move(m_parser_state.error_token),
        m_parser_state.named_capture_groups.keys(),
        m_parser_state.regex_options,
    };
}

ALWAYS_INLINE bool Parser::match_ordinary_characters()
{
    // NOTE: This method must not be called during bracket and repetition parsing!
    // FIXME: Add assertion for that?
    auto type = m_parser_state.current_token.type();
    return ((type == TokenType::Char && m_parser_state.current_token.value() != "\\"sv) // NOTE: Backslash will only be matched as 'char' if it does not form a valid escape.
        || type == TokenType::Comma
        || type == TokenType::Slash
        || type == TokenType::EqualSign
        || type == TokenType::HyphenMinus
        || type == TokenType::Colon);
}

// =============================
// Abstract Posix Parser
// =============================

ALWAYS_INLINE bool AbstractPosixParser::parse_bracket_expression(Vector<CompareTypeAndValuePair>& values, size_t& match_length_minimum)
{
    for (; !done();) {
        if (match(TokenType::HyphenMinus)) {
            consume();

            if (values.is_empty() || (values.size() == 1 && values.last().type == CharacterCompareType::Inverse)) {
                // first in the bracket expression
                values.append({ CharacterCompareType::Char, (ByteCodeValueType)'-' });
            } else if (match(TokenType::RightBracket)) {
                // Last in the bracket expression
                values.append({ CharacterCompareType::Char, (ByteCodeValueType)'-' });
            } else if (values.last().type == CharacterCompareType::Char) {
                values.append({ CharacterCompareType::RangeExpressionDummy, 0 });

                if (done())
                    return set_error(Error::MismatchingBracket);

                if (match(TokenType::HyphenMinus)) {
                    consume();
                    // Valid range, add ordinary character
                    values.append({ CharacterCompareType::Char, (ByteCodeValueType)'-' });
                }
            } else {
                return set_error(Error::InvalidRange);
            }

        } else if (match(TokenType::Circumflex)) {
            auto t = consume();

            if (values.is_empty())
                values.append({ CharacterCompareType::Inverse, 0 });
            else
                values.append({ CharacterCompareType::Char, (ByteCodeValueType)*t.value().characters_without_null_termination() });

        } else if (match(TokenType::LeftBracket)) {
            consume();

            if (match(TokenType::Period)) {
                consume();

                // FIXME: Parse collating element, this is needed when we have locale support
                //        This could have impact on length parameter, I guess.
                set_error(Error::InvalidCollationElement);

                consume(TokenType::Period, Error::InvalidCollationElement);
                consume(TokenType::RightBracket, Error::MismatchingBracket);

            } else if (match(TokenType::EqualSign)) {
                consume();
                // FIXME: Parse collating element, this is needed when we have locale support
                //        This could have impact on length parameter, I guess.
                set_error(Error::InvalidCollationElement);

                consume(TokenType::EqualSign, Error::InvalidCollationElement);
                consume(TokenType::RightBracket, Error::MismatchingBracket);

            } else if (match(TokenType::Colon)) {
                consume();

                CharClass ch_class;
                // parse character class
                if (match(TokenType::Char)) {
                    if (consume("alnum"))
                        ch_class = CharClass::Alnum;
                    else if (consume("alpha"))
                        ch_class = CharClass::Alpha;
                    else if (consume("blank"))
                        ch_class = CharClass::Blank;
                    else if (consume("cntrl"))
                        ch_class = CharClass::Cntrl;
                    else if (consume("digit"))
                        ch_class = CharClass::Digit;
                    else if (consume("graph"))
                        ch_class = CharClass::Graph;
                    else if (consume("lower"))
                        ch_class = CharClass::Lower;
                    else if (consume("print"))
                        ch_class = CharClass::Print;
                    else if (consume("punct"))
                        ch_class = CharClass::Punct;
                    else if (consume("space"))
                        ch_class = CharClass::Space;
                    else if (consume("upper"))
                        ch_class = CharClass::Upper;
                    else if (consume("xdigit"))
                        ch_class = CharClass::Xdigit;
                    else
                        return set_error(Error::InvalidCharacterClass);

                    values.append({ CharacterCompareType::CharClass, (ByteCodeValueType)ch_class });

                } else
                    return set_error(Error::InvalidCharacterClass);

                // FIXME: we do not support locale specific character classes until locales are implemented

                consume(TokenType::Colon, Error::InvalidCharacterClass);
                consume(TokenType::RightBracket, Error::MismatchingBracket);
            } else {
                return set_error(Error::MismatchingBracket);
            }

        } else if (match(TokenType::RightBracket)) {

            if (values.is_empty() || (values.size() == 1 && values.last().type == CharacterCompareType::Inverse)) {
                // handle bracket as ordinary character
                values.append({ CharacterCompareType::Char, (ByteCodeValueType)*consume().value().characters_without_null_termination() });
            } else {
                // closing bracket expression
                break;
            }
        } else {
            values.append({ CharacterCompareType::Char, (ByteCodeValueType)skip() });
        }

        // check if range expression has to be completed...
        if (values.size() >= 3 && values.at(values.size() - 2).type == CharacterCompareType::RangeExpressionDummy) {
            if (values.last().type != CharacterCompareType::Char)
                return set_error(Error::InvalidRange);

            auto value2 = values.take_last();
            values.take_last(); // RangeExpressionDummy
            auto value1 = values.take_last();

            values.append({ CharacterCompareType::CharRange, static_cast<ByteCodeValueType>(CharRange { (u32)value1.value, (u32)value2.value }) });
        }
    }

    if (!values.is_empty()) {
        match_length_minimum = 1;
        if (values.first().type == CharacterCompareType::Inverse)
            match_length_minimum = 0;
    }

    return true;
}

// =============================
// PosixBasic Parser
// =============================

bool PosixBasicParser::parse_internal(ByteCode& stack, size_t& match_length_minimum)
{
    return parse_root(stack, match_length_minimum);
}

bool PosixBasicParser::parse_root(ByteCode& bytecode, size_t& match_length_minimum)
{
    // basic_reg_exp : L_ANCHOR? RE_expression R_ANCHOR?
    if (match(TokenType::Circumflex)) {
        consume();
        bytecode.empend((ByteCodeValueType)OpCodeId::CheckBegin);
    }

    if (!parse_re_expression(bytecode, match_length_minimum))
        return false;

    if (match(TokenType::Dollar)) {
        consume();
        bytecode.empend((ByteCodeValueType)OpCodeId::CheckEnd);
    }

    return !has_error();
}

bool PosixBasicParser::parse_re_expression(ByteCode& bytecode, size_t& match_length_minimum)
{
    // RE_expression : RE_expression? simple_RE
    while (!done()) {
        if (!parse_simple_re(bytecode, match_length_minimum))
            break;
    }

    return !has_error();
}

bool PosixBasicParser::parse_simple_re(ByteCode& bytecode, size_t& match_length_minimum)
{
    // simple_RE : nondupl_RE RE_dupl_symbol?
    ByteCode simple_re_bytecode;
    size_t re_match_length_minimum = 0;
    if (!parse_nonduplicating_re(simple_re_bytecode, re_match_length_minimum))
        return false;

    // RE_dupl_symbol : '*' | Back_open_brace DUP_COUNT (',' DUP_COUNT?)? Back_close_brace
    if (match(TokenType::Asterisk)) {
        consume();
        ByteCode::transform_bytecode_repetition_any(simple_re_bytecode, true);
    } else if (try_skip("\\{"sv)) {
        auto read_number = [&]() -> Optional<size_t> {
            if (!match(TokenType::Char))
                return {};
            size_t value = 0;
            while (match(TokenType::Char)) {
                auto c = m_parser_state.current_token.value().substring_view(0, 1);
                auto c_value = c.to_number<unsigned>();
                if (!c_value.has_value())
                    break;
                value *= 10;
                value += *c_value;
                consume();
            }
            return value;
        };

        size_t min_limit;
        Optional<size_t> max_limit;

        if (auto limit = read_number(); !limit.has_value())
            return set_error(Error::InvalidRepetitionMarker);
        else
            min_limit = *limit;

        if (match(TokenType::Comma)) {
            consume();
            max_limit = read_number();
        }

        if (!try_skip("\\}"sv))
            return set_error(Error::MismatchingBrace);

        if (max_limit.value_or(min_limit) < min_limit)
            return set_error(Error::InvalidBraceContent);

        if (min_limit > s_maximum_repetition_count || (max_limit.has_value() && *max_limit > s_maximum_repetition_count))
            return set_error(Error::InvalidBraceContent);

        auto min_repetition_mark_id = m_parser_state.repetition_mark_count++;
        auto max_repetition_mark_id = m_parser_state.repetition_mark_count++;
        ByteCode::transform_bytecode_repetition_min_max(simple_re_bytecode, min_limit, max_limit, min_repetition_mark_id, max_repetition_mark_id, true);
        match_length_minimum += re_match_length_minimum * min_limit;
    } else {
        match_length_minimum += re_match_length_minimum;
    }

    bytecode.extend(move(simple_re_bytecode));
    return true;
}

bool PosixBasicParser::parse_nonduplicating_re(ByteCode& bytecode, size_t& match_length_minimum)
{
    // nondupl_RE : one_char_or_coll_elem_RE | Back_open_paren RE_expression Back_close_paren | BACKREF
    if (try_skip("\\("sv)) {
        TemporaryChange change { m_current_capture_group_depth, m_current_capture_group_depth + 1 };
        // Max number of addressable capture groups is 10, let's just be lenient
        // and accept 20; anything past that is probably a silly pattern anyway.
        if (m_current_capture_group_depth > 20)
            return set_error(Error::InvalidPattern);
        ByteCode capture_bytecode;
        size_t capture_length_minimum = 0;
        auto capture_group_index = ++m_parser_state.capture_groups_count;

        if (!parse_re_expression(capture_bytecode, capture_length_minimum))
            return false;

        if (!try_skip("\\)"sv))
            return set_error(Error::MismatchingParen);

        match_length_minimum += capture_length_minimum;
        if (capture_group_index <= number_of_addressable_capture_groups) {
            m_capture_group_minimum_lengths[capture_group_index - 1] = capture_length_minimum;
            m_capture_group_seen[capture_group_index - 1] = true;
            bytecode.insert_bytecode_group_capture_left(capture_group_index);
        }

        bytecode.extend(capture_bytecode);

        if (capture_group_index <= number_of_addressable_capture_groups)
            bytecode.insert_bytecode_group_capture_right(capture_group_index);
        return true;
    }

    for (size_t i = 1; i < 10; ++i) {
        char backref_name[2] { '\\', '0' };
        backref_name[1] += i;
        if (try_skip({ backref_name, 2 })) {
            if (!m_capture_group_seen[i - 1])
                return set_error(Error::InvalidNumber);
            match_length_minimum += m_capture_group_minimum_lengths[i - 1];
            bytecode.insert_bytecode_compare_values({ { CharacterCompareType::Reference, (ByteCodeValueType)i } });
            return true;
        }
    }

    return parse_one_char_or_collation_element(bytecode, match_length_minimum);
}

bool PosixBasicParser::parse_one_char_or_collation_element(ByteCode& bytecode, size_t& match_length_minimum)
{
    // one_char_or_coll_elem_RE : ORD_CHAR | QUOTED_CHAR | '.' | bracket_expression
    if (match(TokenType::Period)) {
        consume();
        bytecode.insert_bytecode_compare_values({ { CharacterCompareType::AnyChar, 0 } });
        match_length_minimum += 1;
        return true;
    }

    // Dollars are special if at the end of a pattern.
    if (match(TokenType::Dollar)) {
        consume();

        // If we are at the end of a pattern, emit an end check instruction.
        if (match(TokenType::Eof)) {
            bytecode.empend((ByteCodeValueType)OpCodeId::CheckEnd);
            return true;
        }

        // We are not at the end of the string, so we should roll back and continue as normal.
        back(2);
    }

    if (match(TokenType::Char)) {
        auto ch = consume().value()[0];
        if (ch == '\\') {
            if (m_parser_state.regex_options.has_flag_set(AllFlags::Extra))
                return set_error(Error::InvalidPattern);

            // This was \<ORD_CHAR>, the spec does not define any behaviour for this but glibc regex ignores it - and so do we.
            return true;
        }

        bytecode.insert_bytecode_compare_values({ { CharacterCompareType::Char, (ByteCodeValueType)ch } });
        match_length_minimum += 1;
        return true;
    }

    // None of these are special in BRE.
    if (match(TokenType::Questionmark) || match(TokenType::RightParen) || match(TokenType::HyphenMinus)
        || match(TokenType::Circumflex) || match(TokenType::RightCurly) || match(TokenType::Comma) || match(TokenType::Colon)
        || match(TokenType::Dollar) || match(TokenType::EqualSign) || match(TokenType::LeftCurly) || match(TokenType::LeftParen)
        || match(TokenType::Pipe) || match(TokenType::Slash) || match(TokenType::RightBracket) || match(TokenType::RightParen)) {

        auto ch = consume().value()[0];
        bytecode.insert_bytecode_compare_values({ { CharacterCompareType::Char, (ByteCodeValueType)ch } });
        match_length_minimum += 1;
        return true;
    }

    if (match(TokenType::EscapeSequence)) {
        if (m_parser_state.current_token.value().is_one_of("\\)"sv, "\\}"sv, "\\("sv, "\\{"sv))
            return false;
        auto ch = consume().value()[1];
        bytecode.insert_bytecode_compare_values({ { CharacterCompareType::Char, (ByteCodeValueType)ch } });
        match_length_minimum += 1;
        return true;
    }

    Vector<CompareTypeAndValuePair> values;
    size_t bracket_minimum_length = 0;

    if (match(TokenType::LeftBracket)) {
        consume();
        if (!AbstractPosixParser::parse_bracket_expression(values, bracket_minimum_length))
            return false;

        consume(TokenType::RightBracket, Error::MismatchingBracket);

        if (!has_error())
            bytecode.insert_bytecode_compare_values(move(values));
        match_length_minimum += bracket_minimum_length;
        return !has_error();
    }

    return set_error(Error::InvalidPattern);
}

// =============================
// PosixExtended Parser
// =============================

bool PosixExtendedParser::parse_internal(ByteCode& stack, size_t& match_length_minimum)
{
    return parse_root(stack, match_length_minimum);
}

ALWAYS_INLINE bool PosixExtendedParser::match_repetition_symbol()
{
    auto type = m_parser_state.current_token.type();
    return (type == TokenType::Asterisk
        || type == TokenType::Plus
        || type == TokenType::Questionmark
        || type == TokenType::LeftCurly);
}

ALWAYS_INLINE bool PosixExtendedParser::parse_repetition_symbol(ByteCode& bytecode_to_repeat, size_t& match_length_minimum)
{
    if (match(TokenType::LeftCurly)) {
        consume();

        StringBuilder number_builder;

        while (match(TokenType::Char)) {
            number_builder.append(consume().value());
        }

        auto maybe_minimum = number_builder.to_byte_string().to_number<unsigned>();
        if (!maybe_minimum.has_value())
            return set_error(Error::InvalidBraceContent);

        auto minimum = maybe_minimum.value();
        match_length_minimum *= minimum;

        if (minimum > s_maximum_repetition_count)
            return set_error(Error::InvalidBraceContent);

        if (match(TokenType::Comma)) {
            consume();
        } else {
            auto repetition_mark_id = m_parser_state.repetition_mark_count++;

            ByteCode bytecode;
            bytecode.insert_bytecode_repetition_n(bytecode_to_repeat, minimum, repetition_mark_id);
            bytecode_to_repeat = move(bytecode);

            consume(TokenType::RightCurly, Error::MismatchingBrace);
            return !has_error();
        }

        Optional<u32> maybe_maximum {};
        number_builder.clear();
        while (match(TokenType::Char)) {
            number_builder.append(consume().value());
        }
        if (!number_builder.is_empty()) {
            auto value = number_builder.to_byte_string().to_number<unsigned>();
            if (!value.has_value() || minimum > value.value() || *value > s_maximum_repetition_count)
                return set_error(Error::InvalidBraceContent);

            maybe_maximum = value.value();
        }

        auto min_repetition_mark_id = m_parser_state.repetition_mark_count++;
        auto max_repetition_mark_id = m_parser_state.repetition_mark_count++;
        ByteCode::transform_bytecode_repetition_min_max(bytecode_to_repeat, minimum, maybe_maximum, min_repetition_mark_id, max_repetition_mark_id);

        consume(TokenType::RightCurly, Error::MismatchingBrace);
        return !has_error();
    }
    if (match(TokenType::Plus)) {
        consume();

        bool nongreedy = match(TokenType::Questionmark);
        if (nongreedy)
            consume();

        // Note: don't touch match_length_minimum, it's already correct
        ByteCode::transform_bytecode_repetition_min_one(bytecode_to_repeat, !nongreedy);
        return !has_error();
    }
    if (match(TokenType::Asterisk)) {
        consume();
        match_length_minimum = 0;

        bool nongreedy = match(TokenType::Questionmark);
        if (nongreedy)
            consume();

        ByteCode::transform_bytecode_repetition_any(bytecode_to_repeat, !nongreedy);

        return !has_error();
    }
    if (match(TokenType::Questionmark)) {
        consume();
        match_length_minimum = 0;

        bool nongreedy = match(TokenType::Questionmark);
        if (nongreedy)
            consume();

        ByteCode::transform_bytecode_repetition_zero_or_one(bytecode_to_repeat, !nongreedy);
        return !has_error();
    }

    return false;
}

ALWAYS_INLINE bool PosixExtendedParser::parse_bracket_expression(ByteCode& stack, size_t& match_length_minimum)
{
    Vector<CompareTypeAndValuePair> values;
    if (!AbstractPosixParser::parse_bracket_expression(values, match_length_minimum))
        return false;

    if (!has_error())
        stack.insert_bytecode_compare_values(move(values));

    return !has_error();
}

ALWAYS_INLINE bool PosixExtendedParser::parse_sub_expression(ByteCode& stack, size_t& match_length_minimum)
{
    ByteCode bytecode;
    size_t length = 0;
    bool should_parse_repetition_symbol { false };

    for (;;) {
        if (match_ordinary_characters()) {
            Token start_token = m_parser_state.current_token;
            Token last_token = m_parser_state.current_token;
            for (;;) {
                if (!match_ordinary_characters())
                    break;
                ++length;
                last_token = consume();
            }

            if (length > 1) {
                // last character is inserted into 'bytecode' for duplication symbol handling
                auto new_length = length - (match_repetition_symbol() ? 1 : 0);
                stack.insert_bytecode_compare_string({ start_token.value().characters_without_null_termination(), new_length });
            }

            if ((match_repetition_symbol() && length > 1) || length == 1) // Create own compare opcode for last character before duplication symbol
                bytecode.insert_bytecode_compare_values({ { CharacterCompareType::Char, (ByteCodeValueType)last_token.value().characters_without_null_termination()[0] } });

            should_parse_repetition_symbol = true;
            break;
        }

        if (m_parser_state.current_token.value() == "\\"sv) {
            if (m_parser_state.regex_options.has_flag_set(AllFlags::Extra))
                return set_error(Error::InvalidPattern);

            consume();
            continue;
        }

        if (match_repetition_symbol())
            return set_error(Error::InvalidRepetitionMarker);

        if (match(TokenType::Period)) {
            length = 1;
            consume();
            bytecode.insert_bytecode_compare_values({ { CharacterCompareType::AnyChar, 0 } });
            should_parse_repetition_symbol = true;
            break;
        }

        if (match(TokenType::EscapeSequence)) {
            length = 1;
            Token t = consume();
            dbgln_if(REGEX_DEBUG, "[PARSER] EscapeSequence with substring {}", t.value());

            bytecode.insert_bytecode_compare_values({ { CharacterCompareType::Char, (u32)t.value().characters_without_null_termination()[1] } });
            should_parse_repetition_symbol = true;
            break;
        }

        if (match(TokenType::LeftBracket)) {
            consume();

            ByteCode sub_ops;
            if (!parse_bracket_expression(sub_ops, length) || !sub_ops.size())
                return set_error(Error::InvalidBracketContent);

            bytecode.extend(move(sub_ops));

            consume(TokenType::RightBracket, Error::MismatchingBracket);
            should_parse_repetition_symbol = true;
            break;
        }

        if (match(TokenType::RightBracket)) {
            return set_error(Error::MismatchingBracket);
        }

        if (match(TokenType::RightCurly)) {
            return set_error(Error::MismatchingBrace);
        }

        if (match(TokenType::Circumflex)) {
            consume();
            bytecode.empend((ByteCodeValueType)OpCodeId::CheckBegin);
            break;
        }

        if (match(TokenType::Dollar)) {
            consume();
            bytecode.empend((ByteCodeValueType)OpCodeId::CheckEnd);
            break;
        }

        if (match(TokenType::RightParen))
            return false;

        if (match(TokenType::LeftParen)) {
            enum GroupMode {
                Normal,
                Lookahead,
                NegativeLookahead,
                Lookbehind,
                NegativeLookbehind,
            } group_mode { Normal };
            consume();
            Optional<StringView> capture_group_name;
            bool prevent_capture_group = false;
            if (match(TokenType::Questionmark)) {
                consume();

                if (match(TokenType::Colon)) {
                    consume();
                    prevent_capture_group = true;
                } else if (consume("<")) { // named capturing group

                    Token start_token = m_parser_state.current_token;
                    Token last_token = m_parser_state.current_token;
                    size_t capture_group_name_length = 0;
                    for (;;) {
                        if (!match_ordinary_characters())
                            return set_error(Error::InvalidNameForCaptureGroup);
                        if (match(TokenType::Char) && m_parser_state.current_token.value()[0] == '>') {
                            consume();
                            break;
                        }
                        ++capture_group_name_length;
                        last_token = consume();
                    }
                    capture_group_name = StringView(start_token.value().characters_without_null_termination(), capture_group_name_length);
                    ++m_parser_state.named_capture_groups_count;

                } else if (match(TokenType::EqualSign)) { // positive lookahead
                    consume();
                    group_mode = Lookahead;
                } else if (consume("!")) { // negative lookahead
                    group_mode = NegativeLookahead;
                } else if (consume("<")) {
                    if (match(TokenType::EqualSign)) { // positive lookbehind
                        consume();
                        group_mode = Lookbehind;
                    }
                    if (consume("!")) // negative lookbehind
                        group_mode = NegativeLookbehind;
                } else {
                    return set_error(Error::InvalidRepetitionMarker);
                }
            }

            auto current_capture_group = m_parser_state.capture_groups_count;
            if (!(m_parser_state.regex_options & AllFlags::SkipSubExprResults || prevent_capture_group)) {
                bytecode.insert_bytecode_group_capture_left(current_capture_group);
                m_parser_state.capture_groups_count++;
            }

            ByteCode capture_group_bytecode;

            if (!parse_root(capture_group_bytecode, length))
                return set_error(Error::InvalidPattern);

            switch (group_mode) {
            case Normal:
                bytecode.extend(move(capture_group_bytecode));
                break;
            case Lookahead:
                bytecode.insert_bytecode_lookaround(move(capture_group_bytecode), ByteCode::LookAroundType::LookAhead, length);
                break;
            case NegativeLookahead:
                bytecode.insert_bytecode_lookaround(move(capture_group_bytecode), ByteCode::LookAroundType::NegatedLookAhead, length);
                break;
            case Lookbehind:
                bytecode.insert_bytecode_lookaround(move(capture_group_bytecode), ByteCode::LookAroundType::LookBehind, length);
                break;
            case NegativeLookbehind:
                bytecode.insert_bytecode_lookaround(move(capture_group_bytecode), ByteCode::LookAroundType::NegatedLookBehind, length);
                break;
            }

            consume(TokenType::RightParen, Error::MismatchingParen);

            if (!(m_parser_state.regex_options & AllFlags::SkipSubExprResults || prevent_capture_group)) {
                if (capture_group_name.has_value())
                    bytecode.insert_bytecode_group_capture_right(current_capture_group, capture_group_name.value());
                else
                    bytecode.insert_bytecode_group_capture_right(current_capture_group);
            }
            should_parse_repetition_symbol = true;
            break;
        }

        return false;
    }

    if (match_repetition_symbol()) {
        if (should_parse_repetition_symbol)
            parse_repetition_symbol(bytecode, length);
        else
            return set_error(Error::InvalidRepetitionMarker);
    }

    stack.extend(move(bytecode));
    match_length_minimum += length;

    return true;
}

bool PosixExtendedParser::parse_root(ByteCode& stack, size_t& match_length_minimum)
{
    ByteCode bytecode_left;
    size_t match_length_minimum_left { 0 };

    if (match_repetition_symbol())
        return set_error(Error::InvalidRepetitionMarker);

    for (;;) {
        if (!parse_sub_expression(bytecode_left, match_length_minimum_left))
            break;

        if (match(TokenType::Pipe)) {
            consume();

            ByteCode bytecode_right;
            size_t match_length_minimum_right { 0 };

            if (!parse_root(bytecode_right, match_length_minimum_right) || bytecode_right.is_empty())
                return set_error(Error::InvalidPattern);

            ByteCode new_bytecode;
            new_bytecode.insert_bytecode_alternation(move(bytecode_left), move(bytecode_right));
            bytecode_left = move(new_bytecode);
            match_length_minimum_left = min(match_length_minimum_right, match_length_minimum_left);
        }
    }

    if (bytecode_left.is_empty())
        set_error(Error::EmptySubExpression);

    stack.extend(move(bytecode_left));
    match_length_minimum = match_length_minimum_left;
    return !has_error();
}

// =============================
// ECMA262 Parser
// =============================

bool ECMA262Parser::parse_internal(ByteCode& stack, size_t& match_length_minimum)
{
    auto unicode = m_parser_state.regex_options.has_flag_set(AllFlags::Unicode);
    auto unicode_sets = m_parser_state.regex_options.has_flag_set(AllFlags::UnicodeSets);
    if (unicode || unicode_sets) {
        return parse_pattern(stack, match_length_minimum, { .unicode = true, .named = true, .unicode_sets = unicode_sets });
    }

    ByteCode new_stack;
    size_t new_match_length = 0;
    auto res = parse_pattern(new_stack, new_match_length, { .unicode = false, .named = false, .unicode_sets = false });
    if (m_parser_state.named_capture_groups_count > 0) {
        reset();
        return parse_pattern(stack, match_length_minimum, { .unicode = false, .named = true, .unicode_sets = false });
    }

    if (!res)
        return false;

    stack.extend(new_stack);
    match_length_minimum = new_match_length;
    return res;
}

bool ECMA262Parser::parse_pattern(ByteCode& stack, size_t& match_length_minimum, ParseFlags flags)
{
    return parse_disjunction(stack, match_length_minimum, flags);
}

bool ECMA262Parser::has_duplicate_in_current_alternative(DeprecatedFlyString const& name)
{
    auto it = m_parser_state.named_capture_groups.find(name);
    if (it == m_parser_state.named_capture_groups.end())
        return false;

    return any_of(it->value.begin(), it->value.end(), [&](auto& group) {
        return group.alternative_id == m_current_alternative_id;
    });
}

bool ECMA262Parser::parse_disjunction(ByteCode& stack, size_t& match_length_minimum, ParseFlags flags)
{
    size_t total_match_length_minimum = NumericLimits<size_t>::max();
    Vector<ByteCode> alternatives;

    TemporaryChange<size_t> alternative_id_change { m_current_alternative_id, 1 };

    while (true) {
        ByteCode alternative_stack;
        size_t alternative_minimum_length = 0;
        auto alt_ok = parse_alternative(alternative_stack, alternative_minimum_length, flags);
        if (!alt_ok)
            return false;

        alternatives.append(move(alternative_stack));
        total_match_length_minimum = min(alternative_minimum_length, total_match_length_minimum);

        if (!match(TokenType::Pipe))
            break;
        consume();

        m_current_alternative_id += 1;
    }

    Optimizer::append_alternation(stack, alternatives.span());
    match_length_minimum = total_match_length_minimum;

    return true;
}

bool ECMA262Parser::parse_alternative(ByteCode& stack, size_t& match_length_minimum, ParseFlags flags)
{
    for (;;) {
        if (match(TokenType::Eof))
            return true;

        if (parse_term(stack, match_length_minimum, flags))
            continue;

        return !has_error();
    }
}

bool ECMA262Parser::parse_term(ByteCode& stack, size_t& match_length_minimum, ParseFlags flags)
{
    if (parse_assertion(stack, match_length_minimum, flags))
        return true;

    ByteCode atom_stack;
    size_t minimum_atom_length = 0;
    auto parse_with_quantifier = [&] {
        bool did_parse_one = false;
        if (m_should_use_browser_extended_grammar)
            did_parse_one = parse_extended_atom(atom_stack, minimum_atom_length, flags);

        if (!did_parse_one)
            did_parse_one = parse_atom(atom_stack, minimum_atom_length, flags);

        if (!did_parse_one)
            return false;

        VERIFY(did_parse_one);
        return parse_quantifier(atom_stack, minimum_atom_length, flags);
    };

    if (!parse_with_quantifier())
        return false;

    stack.extend(move(atom_stack));
    match_length_minimum += minimum_atom_length;
    return true;
}

bool ECMA262Parser::parse_assertion(ByteCode& stack, [[maybe_unused]] size_t& match_length_minimum, ParseFlags flags)
{
    if (match(TokenType::Circumflex)) {
        consume();
        stack.empend((ByteCodeValueType)OpCodeId::CheckBegin);
        return true;
    }

    if (match(TokenType::Dollar)) {
        consume();
        stack.empend((ByteCodeValueType)OpCodeId::CheckEnd);
        return true;
    }

    if (try_skip("\\b"sv)) {
        stack.insert_bytecode_check_boundary(BoundaryCheckType::Word);
        return true;
    }

    if (try_skip("\\B"sv)) {
        stack.insert_bytecode_check_boundary(BoundaryCheckType::NonWord);
        return true;
    }

    if (match(TokenType::LeftParen)) {
        if (!try_skip("(?"sv))
            return false;

        if (done()) {
            set_error(Error::InvalidCaptureGroup);
            return false;
        }

        ByteCode assertion_stack;
        size_t length_dummy = 0;

        bool should_parse_forward_assertion = !m_should_use_browser_extended_grammar || flags.unicode;
        if (should_parse_forward_assertion && try_skip("="sv)) {
            if (!parse_inner_disjunction(assertion_stack, length_dummy, flags))
                return false;
            stack.insert_bytecode_lookaround(move(assertion_stack), ByteCode::LookAroundType::LookAhead);
            return true;
        }
        if (should_parse_forward_assertion && try_skip("!"sv)) {
            enter_capture_group_scope();
            ScopeGuard quit_scope {
                [this] {
                    exit_capture_group_scope();
                }
            };
            if (!parse_inner_disjunction(assertion_stack, length_dummy, flags))
                return false;
            stack.insert_bytecode_lookaround(move(assertion_stack), ByteCode::LookAroundType::NegatedLookAhead);
            clear_all_capture_groups_in_scope(stack);

            return true;
        }
        if (m_should_use_browser_extended_grammar) {
            if (!flags.unicode) {
                if (parse_quantifiable_assertion(assertion_stack, match_length_minimum, flags)) {
                    if (!parse_quantifier(assertion_stack, match_length_minimum, flags))
                        return false;

                    stack.extend(move(assertion_stack));
                    return true;
                }
            }
        }
        if (try_skip("<="sv)) {
            if (!parse_inner_disjunction(assertion_stack, length_dummy, flags))
                return false;
            // FIXME: Somehow ensure that this assertion regexp has a fixed length.
            stack.insert_bytecode_lookaround(move(assertion_stack), ByteCode::LookAroundType::LookBehind, length_dummy);
            return true;
        }
        if (try_skip("<!"sv)) {
            enter_capture_group_scope();
            ScopeGuard quit_scope {
                [this] {
                    exit_capture_group_scope();
                }
            };
            if (!parse_inner_disjunction(assertion_stack, length_dummy, flags))
                return false;
            stack.insert_bytecode_lookaround(move(assertion_stack), ByteCode::LookAroundType::NegatedLookBehind, length_dummy);
            clear_all_capture_groups_in_scope(stack);
            return true;
        }

        // If none of these matched, put the '(?' back.
        m_parser_state.lexer.back(3);
        m_parser_state.current_token = m_parser_state.lexer.next();
        return false;
    }

    return false;
}

bool ECMA262Parser::parse_inner_disjunction(ByteCode& bytecode_stack, size_t& length, ParseFlags flags)
{
    auto disjunction_ok = parse_disjunction(bytecode_stack, length, flags);
    if (!disjunction_ok)
        return false;
    consume(TokenType::RightParen, Error::MismatchingParen);
    return true;
}

bool ECMA262Parser::parse_quantifiable_assertion(ByteCode& stack, size_t&, ParseFlags flags)
{
    VERIFY(m_should_use_browser_extended_grammar);
    ByteCode assertion_stack;
    size_t match_length_minimum = 0;

    if (try_skip("="sv)) {
        if (!parse_inner_disjunction(assertion_stack, match_length_minimum, { .unicode = false, .named = flags.named, .unicode_sets = false }))
            return false;

        stack.insert_bytecode_lookaround(move(assertion_stack), ByteCode::LookAroundType::LookAhead);
        return true;
    }
    if (try_skip("!"sv)) {
        enter_capture_group_scope();
        ScopeGuard quit_scope {
            [this] {
                exit_capture_group_scope();
            }
        };
        if (!parse_inner_disjunction(assertion_stack, match_length_minimum, { .unicode = false, .named = flags.named, .unicode_sets = false }))
            return false;

        stack.insert_bytecode_lookaround(move(assertion_stack), ByteCode::LookAroundType::NegatedLookAhead);
        clear_all_capture_groups_in_scope(stack);
        return true;
    }

    return false;
}

StringView ECMA262Parser::read_digits_as_string(ReadDigitsInitialZeroState initial_zero, bool hex, int max_count, int min_count)
{
    if (!match(TokenType::Char))
        return {};

    if (initial_zero == ReadDigitsInitialZeroState::Disallow && m_parser_state.current_token.value() == "0")
        return {};

    int count = 0;
    size_t offset = 0;
    auto start_token = m_parser_state.current_token;
    while (match(TokenType::Char)) {
        auto const c = m_parser_state.current_token.value();

        if (max_count > 0 && count >= max_count)
            break;

        if (hex && !AK::StringUtils::convert_to_uint_from_hex(c).has_value())
            break;
        if (!hex && !c.to_number<unsigned>().has_value())
            break;

        offset += consume().value().length();
        ++count;
    }

    if (count < min_count)
        return {};

    return StringView { start_token.value().characters_without_null_termination(), offset };
}

Optional<unsigned> ECMA262Parser::read_digits(ECMA262Parser::ReadDigitsInitialZeroState initial_zero, bool hex, int max_count, int min_count)
{
    auto str = read_digits_as_string(initial_zero, hex, max_count, min_count);
    if (str.is_empty())
        return {};
    if (hex)
        return AK::StringUtils::convert_to_uint_from_hex(str);
    return str.to_number<unsigned>();
}

bool ECMA262Parser::parse_quantifier(ByteCode& stack, size_t& match_length_minimum, ParseFlags flags)
{
    enum class Repetition {
        OneOrMore,
        ZeroOrMore,
        Optional,
        Explicit,
        None,
    } repetition_mark { Repetition::None };

    bool ungreedy = false;
    Optional<u64> repeat_min, repeat_max;

    if (match(TokenType::Asterisk)) {
        consume();
        repetition_mark = Repetition::ZeroOrMore;
    } else if (match(TokenType::Plus)) {
        consume();
        repetition_mark = Repetition::OneOrMore;
    } else if (match(TokenType::Questionmark)) {
        consume();
        repetition_mark = Repetition::Optional;
    } else if (match(TokenType::LeftCurly)) {
        repetition_mark = Repetition::Explicit;
        if (!parse_interval_quantifier(repeat_min, repeat_max)) {
            if (flags.unicode) {
                // Invalid interval quantifiers are disallowed in Unicode mod - they must be escaped with '\{'.
                set_error(Error::InvalidPattern);
            }
            return !has_error();
        }
    } else {
        return true;
    }

    if (match(TokenType::Questionmark)) {
        consume();
        ungreedy = true;
    }

    switch (repetition_mark) {
    case Repetition::OneOrMore:
        ByteCode::transform_bytecode_repetition_min_one(stack, !ungreedy);
        break;
    case Repetition::ZeroOrMore:
        ByteCode::transform_bytecode_repetition_any(stack, !ungreedy);
        match_length_minimum = 0;
        break;
    case Repetition::Optional:
        ByteCode::transform_bytecode_repetition_zero_or_one(stack, !ungreedy);
        match_length_minimum = 0;
        break;
    case Repetition::Explicit: {
        auto min_repetition_mark_id = m_parser_state.repetition_mark_count++;
        auto max_repetition_mark_id = m_parser_state.repetition_mark_count++;
        ByteCode::transform_bytecode_repetition_min_max(stack, repeat_min.value(), repeat_max, min_repetition_mark_id, max_repetition_mark_id, !ungreedy);
        match_length_minimum *= repeat_min.value();
        break;
    }
    case Repetition::None:
        VERIFY_NOT_REACHED();
    }

    return true;
}

bool ECMA262Parser::parse_interval_quantifier(Optional<u64>& repeat_min, Optional<u64>& repeat_max)
{
    VERIFY(match(TokenType::LeftCurly));
    consume();
    auto chars_consumed = 1;

    auto low_bound_string = read_digits_as_string();
    chars_consumed += low_bound_string.length();

    auto low_bound = low_bound_string.to_number<u64>();

    if (!low_bound.has_value()) {
        if (!m_should_use_browser_extended_grammar && done())
            return set_error(Error::MismatchingBrace);

        back(chars_consumed + !done());
        return false;
    }

    repeat_min = low_bound.value();

    if (match(TokenType::Comma)) {
        consume();
        ++chars_consumed;
        auto high_bound_string = read_digits_as_string();
        auto high_bound = high_bound_string.to_number<u64>();
        if (high_bound.has_value()) {
            repeat_max = high_bound.value();
            chars_consumed += high_bound_string.length();
        }
    } else {
        repeat_max = repeat_min;
    }

    if (!match(TokenType::RightCurly)) {
        if (!m_should_use_browser_extended_grammar && done())
            return set_error(Error::MismatchingBrace);

        back(chars_consumed + !done());
        return false;
    }

    consume();
    ++chars_consumed;

    if (repeat_max.has_value()) {
        if (repeat_min.value() > repeat_max.value())
            set_error(Error::InvalidBraceContent);
    }

    if ((*repeat_min > s_ecma262_maximum_repetition_count) || (repeat_max.has_value() && (*repeat_max > s_ecma262_maximum_repetition_count)))
        return set_error(Error::InvalidBraceContent);

    return true;
}

bool ECMA262Parser::parse_atom(ByteCode& stack, size_t& match_length_minimum, ParseFlags flags)
{
    if (match(TokenType::EscapeSequence)) {
        // Also part of AtomEscape.
        auto token = consume();
        match_length_minimum += 1;
        stack.insert_bytecode_compare_values({ { CharacterCompareType::Char, (u8)token.value()[1] } });
        return true;
    }
    if (try_skip("\\"sv)) {
        // AtomEscape.
        return parse_atom_escape(stack, match_length_minimum, flags);
    }

    if (match(TokenType::LeftBracket)) {
        // Character class.
        return parse_character_class(stack, match_length_minimum, flags);
    }

    if (match(TokenType::LeftParen)) {
        // Non-capturing group, or a capture group.
        return parse_capture_group(stack, match_length_minimum, flags);
    }

    if (match(TokenType::Period)) {
        consume();
        match_length_minimum += 1;
        stack.insert_bytecode_compare_values({ { CharacterCompareType::AnyChar, 0 } });
        return true;
    }

    if (match(TokenType::Circumflex) || match(TokenType::Dollar) || match(TokenType::RightParen)
        || match(TokenType::Pipe) || match(TokenType::Plus) || match(TokenType::Asterisk)
        || match(TokenType::Questionmark)) {

        return false;
    }

    if (match(TokenType::RightBracket) || match(TokenType::RightCurly) || match(TokenType::LeftCurly)) {
        if (flags.unicode)
            return set_error(Error::InvalidPattern);

        if (m_should_use_browser_extended_grammar) {
            auto token = consume();
            match_length_minimum += 1;
            stack.insert_bytecode_compare_values({ { CharacterCompareType::Char, (u8)token.value()[0] } });
            return true;
        }
        return false;
    }

    if (match_ordinary_characters()) {
        auto token = consume().value();
        match_length_minimum += 1;
        stack.insert_bytecode_compare_values({ { CharacterCompareType::Char, (u8)token[0] } });
        return true;
    }

    set_error(Error::InvalidPattern);
    return false;
}

bool ECMA262Parser::parse_extended_atom(ByteCode&, size_t&, ParseFlags)
{
    // Note: This includes only rules *not* present in parse_atom()
    VERIFY(m_should_use_browser_extended_grammar);

    return parse_invalid_braced_quantifier(); // FAIL FAIL FAIL
}

bool ECMA262Parser::parse_invalid_braced_quantifier()
{
    if (!match(TokenType::LeftCurly))
        return false;
    consume();
    size_t chars_consumed = 1;
    auto low_bound = read_digits_as_string();
    StringView high_bound;

    if (low_bound.is_empty()) {
        back(chars_consumed + !done());
        return false;
    }
    chars_consumed += low_bound.length();
    if (match(TokenType::Comma)) {
        consume();
        ++chars_consumed;

        high_bound = read_digits_as_string();
        chars_consumed += high_bound.length();
    }

    if (!match(TokenType::RightCurly)) {
        back(chars_consumed + !done());
        return false;
    }

    consume();
    set_error(Error::InvalidPattern);
    return true;
}

bool ECMA262Parser::parse_character_escape(Vector<CompareTypeAndValuePair>& compares, size_t& match_length_minimum, ParseFlags flags)
{
    // CharacterEscape > ControlEscape
    if (try_skip("f"sv)) {
        match_length_minimum += 1;
        compares.append({ CharacterCompareType::Char, (ByteCodeValueType)'\f' });
        return true;
    }

    if (try_skip("n"sv)) {
        match_length_minimum += 1;
        compares.append({ CharacterCompareType::Char, (ByteCodeValueType)'\n' });
        return true;
    }

    if (try_skip("r"sv)) {
        match_length_minimum += 1;
        compares.append({ CharacterCompareType::Char, (ByteCodeValueType)'\r' });
        return true;
    }

    if (try_skip("t"sv)) {
        match_length_minimum += 1;
        compares.append({ CharacterCompareType::Char, (ByteCodeValueType)'\t' });
        return true;
    }

    if (try_skip("v"sv)) {
        match_length_minimum += 1;
        compares.append({ CharacterCompareType::Char, (ByteCodeValueType)'\v' });
        return true;
    }

    // CharacterEscape > ControlLetter
    if (try_skip("c"sv)) {
        for (auto c : s_alphabetic_characters) {
            if (try_skip({ &c, 1 })) {
                match_length_minimum += 1;
                compares.append({ CharacterCompareType::Char, (ByteCodeValueType)(c % 32) });
                return true;
            }
        }

        if (flags.unicode) {
            set_error(Error::InvalidPattern);
            return false;
        }

        if (m_should_use_browser_extended_grammar) {
            back(1 + (done() ? 0 : 1));
            compares.append({ CharacterCompareType::Char, (ByteCodeValueType)'\\' });
            match_length_minimum += 1;
            return true;
        }

        // Allow '\c' in non-unicode mode, just matches 'c'.
        match_length_minimum += 1;
        compares.append({ CharacterCompareType::Char, (ByteCodeValueType)'c' });
        return true;
    }

    // '\0'
    if (try_skip("0"sv)) {
        if (!lookahead_any(s_decimal_characters)) {
            match_length_minimum += 1;
            compares.append({ CharacterCompareType::Char, (ByteCodeValueType)0 });
            return true;
        }

        back();
    }

    // LegacyOctalEscapeSequence
    if (m_should_use_browser_extended_grammar) {
        if (!flags.unicode) {
            if (auto escape = parse_legacy_octal_escape(); escape.has_value()) {
                compares.append({ CharacterCompareType::Char, (ByteCodeValueType)escape.value() });
                match_length_minimum += 1;
                return true;
            }
        }
    }

    // HexEscape
    if (try_skip("x"sv)) {
        if (auto hex_escape = read_digits(ReadDigitsInitialZeroState::Allow, true, 2, 2); hex_escape.has_value()) {
            match_length_minimum += 1;
            compares.append({ CharacterCompareType::Char, (ByteCodeValueType)hex_escape.value() });
            return true;
        }
        if (!flags.unicode) {
            // '\x' is allowed in non-unicode mode, just matches 'x'.
            match_length_minimum += 1;
            compares.append({ CharacterCompareType::Char, (ByteCodeValueType)'x' });
            return true;
        }

        set_error(Error::InvalidPattern);
        return false;
    }

    if (try_skip("u"sv)) {
        if (auto code_point = consume_escaped_code_point(flags.unicode); code_point.has_value()) {
            match_length_minimum += 1;
            compares.append({ CharacterCompareType::Char, (ByteCodeValueType)code_point.value() });
            return true;
        }

        return false;
    }

    // IdentityEscape
    for (auto ch : identity_escape_characters(flags.unicode, m_should_use_browser_extended_grammar)) {
        if (try_skip({ &ch, 1 })) {
            match_length_minimum += 1;
            compares.append({ CharacterCompareType::Char, (ByteCodeValueType)ch });
            return true;
        }
    }

    if (flags.unicode) {
        if (try_skip("/"sv)) {
            match_length_minimum += 1;
            compares.append({ CharacterCompareType::Char, (ByteCodeValueType)'/' });
            return true;
        }
    }

    return false;
}

bool ECMA262Parser::parse_atom_escape(ByteCode& stack, size_t& match_length_minimum, ParseFlags flags)
{
    if (auto escape_str = read_digits_as_string(ReadDigitsInitialZeroState::Disallow); !escape_str.is_empty()) {
        if (auto escape = escape_str.to_number<unsigned>(); escape.has_value()) {
            // See if this is a "back"-reference (we've already parsed the group it refers to)
            auto maybe_length = m_parser_state.capture_group_minimum_lengths.get(escape.value());
            if (maybe_length.has_value()) {
                match_length_minimum += maybe_length.value();
                stack.insert_bytecode_compare_values({ { CharacterCompareType::Reference, (ByteCodeValueType)escape.value() } });
                return true;
            }
            // It's not a pattern seen before, so we have to see if it's a valid reference to a future group.
            if (escape.value() <= ensure_total_number_of_capturing_parenthesis()) {
                // This refers to a future group, and it will _always_ be matching an empty string
                // So just match nothing and move on.
                return true;
            }
            if (!m_should_use_browser_extended_grammar) {
                set_error(Error::InvalidNumber);
                return false;
            }
        }

        // If not, put the characters back.
        back(escape_str.length() + (done() ? 0 : 1));
    }

    Vector<CompareTypeAndValuePair> escape_compares;
    if (parse_character_escape(escape_compares, match_length_minimum, flags)) {
        stack.insert_bytecode_compare_values(move(escape_compares));
        return true;
    }

    if (flags.named && try_skip("k"sv)) {
        auto name = read_capture_group_specifier(true);
        if (name.is_empty()) {
            set_error(Error::InvalidNameForCaptureGroup);
            return false;
        }

        auto it = m_parser_state.named_capture_groups.find(name);
        if (it == m_parser_state.named_capture_groups.end()) {
            set_error(Error::InvalidNameForCaptureGroup);
            return false;
        }

        // Use the first occurrence of the named group for the backreference
        // This follows ECMAScript behavior where \k<name> refers to the first
        // group with that name in left-to-right order, regardless of alternative
        auto group_index = it->value.first().group_index;

        auto maybe_length = m_parser_state.capture_group_minimum_lengths.get(group_index);
        if (!maybe_length.has_value()) {
            set_error(Error::InvalidNameForCaptureGroup);
            return false;
        }

        match_length_minimum += maybe_length.value();
        stack.insert_bytecode_compare_values({ { CharacterCompareType::Reference, (ByteCodeValueType)group_index } });
        return true;
    }

    if (flags.unicode) {
        PropertyEscape property {};
        bool negated = false;

        if (parse_unicode_property_escape(property, negated)) {
            Vector<CompareTypeAndValuePair> compares;
            if (negated)
                compares.empend(CompareTypeAndValuePair { CharacterCompareType::Inverse, 0 });
            property.visit(
                [&](Unicode::Property property) {
                    compares.empend(CompareTypeAndValuePair { CharacterCompareType::Property, (ByteCodeValueType)property.value() });
                },
                [&](Unicode::GeneralCategory general_category) {
                    compares.empend(CompareTypeAndValuePair { CharacterCompareType::GeneralCategory, (ByteCodeValueType)general_category.value() });
                },
                [&](Script script) {
                    if (script.is_extension)
                        compares.empend(CompareTypeAndValuePair { CharacterCompareType::ScriptExtension, (ByteCodeValueType)script.script.value() });
                    else
                        compares.empend(CompareTypeAndValuePair { CharacterCompareType::Script, (ByteCodeValueType)script.script.value() });
                },
                [](Empty&) { VERIFY_NOT_REACHED(); });
            stack.insert_bytecode_compare_values(move(compares));
            match_length_minimum += 1;
            return true;
        }
    }

    if (done())
        return set_error(Error::InvalidTrailingEscape);

    bool negate = false;
    auto ch = parse_character_class_escape(negate);
    if (!ch.has_value()) {
        if (!flags.unicode) {
            // Allow all SourceCharacter's as escapes here.
            auto token = consume();
            match_length_minimum += 1;
            stack.insert_bytecode_compare_values({ { CharacterCompareType::Char, (u8)token.value()[0] } });
            return true;
        }

        set_error(Error::InvalidCharacterClass);
        return false;
    }

    Vector<CompareTypeAndValuePair> compares;
    if (negate)
        compares.empend(CompareTypeAndValuePair { CharacterCompareType::Inverse, 0 });
    compares.empend(CompareTypeAndValuePair { CharacterCompareType::CharClass, (ByteCodeValueType)ch.value() });
    match_length_minimum += 1;
    stack.insert_bytecode_compare_values(move(compares));
    return true;
}
Optional<u8> ECMA262Parser::parse_legacy_octal_escape()
{
    constexpr auto all_octal_digits = "01234567"sv;
    auto read_octal_digit = [&](auto start, auto end, bool should_ensure_no_following_octal_digit) -> Optional<u8> {
        for (char c = '0' + start; c <= '0' + end; ++c) {
            if (try_skip({ &c, 1 })) {
                if (!should_ensure_no_following_octal_digit || !lookahead_any(all_octal_digits))
                    return c - '0';
                back(2);
                return {};
            }
        }
        return {};
    };

    // OctalDigit(1)
    if (auto digit = read_octal_digit(0, 7, true); digit.has_value()) {
        return digit.value();
    }

    // OctalDigit(2)
    if (auto left_digit = read_octal_digit(0, 3, false); left_digit.has_value()) {
        if (auto right_digit = read_octal_digit(0, 7, true); right_digit.has_value()) {
            return left_digit.value() * 8 + right_digit.value();
        }

        back(2);
    }

    // OctalDigit(2)
    if (auto left_digit = read_octal_digit(4, 7, false); left_digit.has_value()) {
        if (auto right_digit = read_octal_digit(0, 7, false); right_digit.has_value()) {
            return left_digit.value() * 8 + right_digit.value();
        }

        back(2);
    }

    // OctalDigit(3)
    if (auto left_digit = read_octal_digit(0, 3, false); left_digit.has_value()) {
        size_t chars_consumed = 1;
        if (auto mid_digit = read_octal_digit(0, 7, false); mid_digit.has_value()) {
            ++chars_consumed;
            if (auto right_digit = read_octal_digit(0, 7, false); right_digit.has_value()) {
                return left_digit.value() * 64 + mid_digit.value() * 8 + right_digit.value();
            }
        }

        back(chars_consumed);
    }

    return {};
}

Optional<CharClass> ECMA262Parser::parse_character_class_escape(bool& negate, bool expect_backslash)
{
    if (expect_backslash && !try_skip("\\"sv))
        return {};

    // CharacterClassEscape
    CharClass ch_class;
    if (try_skip("d"sv)) {
        ch_class = CharClass::Digit;
    } else if (try_skip("D"sv)) {
        ch_class = CharClass::Digit;
        negate = true;
    } else if (try_skip("s"sv)) {
        ch_class = CharClass::Space;
    } else if (try_skip("S"sv)) {
        ch_class = CharClass::Space;
        negate = true;
    } else if (try_skip("w"sv)) {
        ch_class = CharClass::Word;
    } else if (try_skip("W"sv)) {
        ch_class = CharClass::Word;
        negate = true;
    } else {
        return {};
    }

    return ch_class;
}

bool ECMA262Parser::parse_character_class(ByteCode& stack, size_t& match_length_minimum, ParseFlags flags)
{
    consume(TokenType::LeftBracket, Error::InvalidPattern);

    Vector<CompareTypeAndValuePair> compares;

    auto uses_explicit_or_semantics = false;
    if (match(TokenType::Circumflex)) {
        // Negated charclass
        consume();
        compares.empend(CompareTypeAndValuePair { CharacterCompareType::Inverse, 0 });
        uses_explicit_or_semantics = true;
    }

    // ClassContents :: [empty]
    if (match(TokenType::RightBracket)) {
        consume();
        // Should only have at most an 'Inverse'
        VERIFY(compares.size() <= 1);
        stack.insert_bytecode_compare_values(move(compares));
        return true;
    }

    // ClassContents :: [~UnicodeSetsMode] NonemptyClassRanges[?UnicodeMode]
    if (!flags.unicode_sets && !parse_nonempty_class_ranges(compares, flags))
        return false;

    // ClassContents :: [+UnicodeSetsMode] ClassSetExpression
    if (flags.unicode_sets && !parse_class_set_expression(compares))
        return false;

    if (uses_explicit_or_semantics && compares.size() > 2) {
        compares.insert(1, CompareTypeAndValuePair { CharacterCompareType::Or, 0 });
        compares.empend(CompareTypeAndValuePair { CharacterCompareType::EndAndOr, 0 });
    }

    match_length_minimum += 1;
    stack.insert_bytecode_compare_values(move(compares));
    return true;
}

struct CharClassRangeElement {
    union {
        CharClass character_class;
        u32 code_point { 0 };
        Unicode::Property property;
        Unicode::GeneralCategory general_category;
        Unicode::Script script;
    };

    bool is_negated { false };
    bool is_character_class { false };
    bool is_property { false };
    bool is_general_category { false };
    bool is_script { false };
    bool is_script_extension { false };
};

bool ECMA262Parser::parse_nonempty_class_ranges(Vector<CompareTypeAndValuePair>& ranges, ParseFlags flags)
{
    auto read_class_atom_no_dash = [&]() -> Optional<CharClassRangeElement> {
        if (match(TokenType::EscapeSequence)) {
            auto token = consume().value();
            return { CharClassRangeElement { .code_point = (u32)token[1], .is_character_class = false } };
        }

        if (try_skip("\\"sv)) {
            if (done()) {
                set_error(Error::InvalidTrailingEscape);
                return {};
            }

            if (try_skip("f"sv))
                return { CharClassRangeElement { .code_point = '\f', .is_character_class = false } };
            if (try_skip("n"sv))
                return { CharClassRangeElement { .code_point = '\n', .is_character_class = false } };
            if (try_skip("r"sv))
                return { CharClassRangeElement { .code_point = '\r', .is_character_class = false } };
            if (try_skip("t"sv))
                return { CharClassRangeElement { .code_point = '\t', .is_character_class = false } };
            if (try_skip("v"sv))
                return { CharClassRangeElement { .code_point = '\v', .is_character_class = false } };
            if (try_skip("b"sv))
                return { CharClassRangeElement { .code_point = '\b', .is_character_class = false } };
            if (try_skip("/"sv))
                return { CharClassRangeElement { .code_point = '/', .is_character_class = false } };

            // CharacterEscape > ControlLetter
            if (try_skip("c"sv)) {
                for (auto c : s_alphabetic_characters) {
                    if (try_skip({ &c, 1 })) {
                        return { CharClassRangeElement { .code_point = (u32)(c % 32), .is_character_class = false } };
                    }
                }

                if (flags.unicode) {
                    set_error(Error::InvalidPattern);
                    return {};
                }

                if (m_should_use_browser_extended_grammar) {
                    for (auto c = '0'; c <= '9'; ++c) {
                        if (try_skip({ &c, 1 }))
                            return { CharClassRangeElement { .code_point = (u32)(c % 32), .is_character_class = false } };
                    }
                    if (try_skip("_"sv))
                        return { CharClassRangeElement { .code_point = (u32)('_' % 32), .is_character_class = false } };

                    back(1 + !done());
                    return { CharClassRangeElement { .code_point = '\\', .is_character_class = false } };
                }
            }

            // '\0'
            if (try_skip("0"sv)) {
                if (!lookahead_any(s_decimal_characters))
                    return { CharClassRangeElement { .code_point = 0, .is_character_class = false } };
                back();
            }

            // LegacyOctalEscapeSequence
            if (m_should_use_browser_extended_grammar && !flags.unicode) {
                if (auto escape = parse_legacy_octal_escape(); escape.has_value())
                    return { CharClassRangeElement { .code_point = escape.value(), .is_character_class = false } };
            }

            // HexEscape
            if (try_skip("x"sv)) {
                if (auto hex_escape = read_digits(ReadDigitsInitialZeroState::Allow, true, 2, 2); hex_escape.has_value()) {
                    return { CharClassRangeElement { .code_point = hex_escape.value(), .is_character_class = false } };
                } else if (!flags.unicode) {
                    // '\x' is allowed in non-unicode mode, just matches 'x'.
                    return { CharClassRangeElement { .code_point = 'x', .is_character_class = false } };
                } else {
                    set_error(Error::InvalidPattern);
                    return {};
                }
            }

            if (try_skip("u"sv)) {
                if (auto code_point = consume_escaped_code_point(flags.unicode); code_point.has_value()) {
                    // FIXME: While code point ranges are supported, code point matches as "Char" are not!
                    return { CharClassRangeElement { .code_point = code_point.value(), .is_character_class = false } };
                }
                return {};
            }

            // IdentityEscape
            for (auto ch : identity_escape_characters(flags.unicode, m_should_use_browser_extended_grammar)) {
                if (try_skip({ &ch, 1 }))
                    return { CharClassRangeElement { .code_point = (u32)ch, .is_character_class = false } };
            }

            if (flags.unicode) {
                if (try_skip("-"sv))
                    return { CharClassRangeElement { .code_point = '-', .is_character_class = false } };

                PropertyEscape property {};
                bool negated = false;
                if (parse_unicode_property_escape(property, negated)) {
                    return property.visit(
                        [&](Unicode::Property property) {
                            return CharClassRangeElement { .property = property, .is_negated = negated, .is_character_class = true, .is_property = true };
                        },
                        [&](Unicode::GeneralCategory general_category) {
                            return CharClassRangeElement { .general_category = general_category, .is_negated = negated, .is_character_class = true, .is_general_category = true };
                        },
                        [&](Script script) {
                            if (script.is_extension)
                                return CharClassRangeElement { .script = script.script, .is_negated = negated, .is_character_class = true, .is_script_extension = true };

                            return CharClassRangeElement { .script = script.script, .is_negated = negated, .is_character_class = true, .is_script = true };
                        },
                        [](Empty&) -> CharClassRangeElement { VERIFY_NOT_REACHED(); });
                }
            }

            if (try_skip("d"sv))
                return { CharClassRangeElement { .character_class = CharClass::Digit, .is_character_class = true } };
            if (try_skip("s"sv))
                return { CharClassRangeElement { .character_class = CharClass::Space, .is_character_class = true } };
            if (try_skip("w"sv))
                return { CharClassRangeElement { .character_class = CharClass::Word, .is_character_class = true } };
            if (try_skip("D"sv))
                return { CharClassRangeElement { .character_class = CharClass::Digit, .is_negated = true, .is_character_class = true } };
            if (try_skip("S"sv))
                return { CharClassRangeElement { .character_class = CharClass::Space, .is_negated = true, .is_character_class = true } };
            if (try_skip("W"sv))
                return { CharClassRangeElement { .character_class = CharClass::Word, .is_negated = true, .is_character_class = true } };

            if (!flags.unicode) {
                // Any unrecognised escape is allowed in non-unicode mode.
                return { CharClassRangeElement { .code_point = (u32)skip(), .is_character_class = false } };
            }

            set_error(Error::InvalidPattern);
            return {};
        }

        if (match(TokenType::Eof)) {
            set_error(Error::MismatchingBracket);
            return {};
        }

        if (match(TokenType::RightBracket) || match(TokenType::HyphenMinus))
            return {};

        // Allow any (other) SourceCharacter.
        return { CharClassRangeElement { .code_point = (u32)skip(), .is_character_class = false } };
    };
    auto read_class_atom = [&]() -> Optional<CharClassRangeElement> {
        if (match(TokenType::HyphenMinus)) {
            consume();
            return { CharClassRangeElement { .code_point = '-', .is_character_class = false } };
        }

        return read_class_atom_no_dash();
    };

    auto empend_atom = [&](auto& atom) {
        if (atom.is_character_class) {
            if (atom.is_negated)
                ranges.empend(CompareTypeAndValuePair { CharacterCompareType::TemporaryInverse, 0 });

            if (atom.is_property)
                ranges.empend(CompareTypeAndValuePair { CharacterCompareType::Property, (ByteCodeValueType)(atom.property.value()) });
            else if (atom.is_general_category)
                ranges.empend(CompareTypeAndValuePair { CharacterCompareType::GeneralCategory, (ByteCodeValueType)(atom.general_category.value()) });
            else if (atom.is_script)
                ranges.empend(CompareTypeAndValuePair { CharacterCompareType::Script, (ByteCodeValueType)(atom.script.value()) });
            else if (atom.is_script_extension)
                ranges.empend(CompareTypeAndValuePair { CharacterCompareType::ScriptExtension, (ByteCodeValueType)(atom.script.value()) });
            else
                ranges.empend(CompareTypeAndValuePair { CharacterCompareType::CharClass, (ByteCodeValueType)atom.character_class });
        } else {
            VERIFY(!atom.is_negated);
            ranges.empend(CompareTypeAndValuePair { CharacterCompareType::Char, atom.code_point });
        }
    };

    while (!match(TokenType::RightBracket)) {
        if (match(TokenType::Eof)) {
            set_error(Error::MismatchingBracket);
            return false;
        }

        auto first_atom = read_class_atom();
        if (!first_atom.has_value())
            return false;

        if (match(TokenType::HyphenMinus)) {
            consume();
            if (match(TokenType::RightBracket)) {
                //  Allow '-' as the last element in a charclass, even after an atom.
                m_parser_state.lexer.back(2); // -]
                m_parser_state.current_token = m_parser_state.lexer.next();
                goto read_as_single_atom;
            }
            auto second_atom = read_class_atom();
            if (!second_atom.has_value())
                return false;

            if (first_atom.value().is_character_class || second_atom.value().is_character_class) {
                if (m_should_use_browser_extended_grammar) {
                    if (flags.unicode) {
                        set_error(Error::InvalidRange);
                        return false;
                    }

                    // CharacterRangeOrUnion > !Unicode > CharClass
                    empend_atom(*first_atom);
                    ranges.empend(CompareTypeAndValuePair { CharacterCompareType::Char, (ByteCodeValueType)'-' });
                    empend_atom(*second_atom);
                    continue;
                }

                set_error(Error::InvalidRange);
                return false;
            }

            if (first_atom.value().code_point > second_atom.value().code_point) {
                set_error(Error::InvalidRange);
                return false;
            }

            VERIFY(!first_atom.value().is_negated);
            VERIFY(!second_atom.value().is_negated);

            ranges.empend(CompareTypeAndValuePair { CharacterCompareType::CharRange, CharRange { first_atom.value().code_point, second_atom.value().code_point } });
            continue;
        }

    read_as_single_atom:;

        auto atom = first_atom.value();
        empend_atom(atom);
    }

    consume(TokenType::RightBracket, Error::MismatchingBracket);

    return true;
}

bool ECMA262Parser::parse_class_set_expression(Vector<CompareTypeAndValuePair>& compares)
{
    auto start_position = tell();

    // ClassSetExpression :: ClassUnion | ClassIntersection | ClassSubtraction
    if (parse_class_subtraction(compares)) {
        consume(TokenType::RightBracket, Error::MismatchingBracket);
        return true;
    }
    if (has_error())
        return false;

    back(tell() - start_position + 1);
    if (parse_class_intersection(compares)) {
        consume(TokenType::RightBracket, Error::MismatchingBracket);
        return true;
    }
    if (has_error())
        return false;

    back(tell() - start_position + 1);
    if (parse_class_union(compares)) {
        consume(TokenType::RightBracket, Error::MismatchingBracket);
        return true;
    }

    return false;
}

bool ECMA262Parser::parse_class_union(Vector<regex::CompareTypeAndValuePair>& compares)
{
    auto start_position = tell();
    ArmedScopeGuard restore_position { [&] { back(tell() - start_position + 1); } };

    auto first = true;

    // ClassUnion :: ClassSetRange ClassUnion[opt] | ClassSetOperand ClassUnion[opt]
    for (;;) {
        if (!parse_class_set_range(compares)) {
            if (has_error() || match(TokenType::RightBracket))
                break;

            if (!parse_class_set_operand(compares)) {
                if (first || has_error())
                    return false;
                break;
            }
        }
        first = false;
    }

    restore_position.disarm();
    return !has_error();
}

bool ECMA262Parser::parse_class_intersection(Vector<CompareTypeAndValuePair>& compares)
{
    // ClassIntersection :: ClassSetOperand "&&" [lookahead != "&"] ClassSetOperand
    //                    | ClassIntersection "&&" [lookahead != "&"] ClassSetOperand
    Vector<CompareTypeAndValuePair> lhs;
    Vector<CompareTypeAndValuePair> rhs;

    auto start_position = tell();
    ArmedScopeGuard restore_position { [&] { back(tell() - start_position + 1); } };

    if (!parse_class_set_operand(lhs))
        return false;

    if (!try_skip("&&"sv))
        return false;

    compares.append({ CharacterCompareType::And, 0 });
    compares.extend(move(lhs));

    do {
        rhs.clear_with_capacity();
        if (!parse_class_set_operand(rhs))
            return false;

        compares.extend(rhs);

        if (try_skip("&&&"sv))
            return false;
    } while (!has_error() && try_skip("&&"sv));

    compares.append({ CharacterCompareType::EndAndOr, 0 });

    restore_position.disarm();
    return true;
}

bool ECMA262Parser::parse_class_subtraction(Vector<CompareTypeAndValuePair>& compares)
{
    // ClassSubtraction :: ClassSetOperand "--" ClassSetOperand | ClassSubtraction "--" ClassSetOperand
    Vector<CompareTypeAndValuePair> lhs;
    Vector<CompareTypeAndValuePair> rhs;

    auto start_position = tell();
    ArmedScopeGuard restore_position { [&] { back(tell() - start_position + 1); } };

    if (!parse_class_set_operand(lhs))
        return false;

    if (!try_skip("--"sv))
        return false;

    compares.append({ CharacterCompareType::And, 0 });
    compares.extend(move(lhs));

    do {
        rhs.clear_with_capacity();
        if (!parse_class_set_operand(rhs))
            return false;

        compares.append({ CharacterCompareType::TemporaryInverse, 0 });
        compares.extend(rhs);
    } while (!has_error() && try_skip("--"sv));

    compares.append({ CharacterCompareType::EndAndOr, 0 });

    restore_position.disarm();
    return true;
}

bool ECMA262Parser::parse_class_set_range(Vector<CompareTypeAndValuePair>& compares)
{
    // ClassSetRange :: ClassSetCharacter "-" ClassSetCharacter
    auto start_position = tell();
    ArmedScopeGuard restore_position { [&] { back(tell() - start_position + 1); } };

    auto lhs = parse_class_set_character();
    if (!lhs.has_value())
        return false;

    if (!match(TokenType::HyphenMinus))
        return false;
    consume();

    auto rhs = parse_class_set_character();
    if (!rhs.has_value())
        return false;

    compares.append({
        CharacterCompareType::CharRange,
        CharRange { lhs.value(), rhs.value() },
    });
    restore_position.disarm();
    return true;
}

Optional<u32> ECMA262Parser::parse_class_set_character()
{
    // ClassSetCharacter :: [lookahead ∉ ClassSetReservedDoublePunctuator] SourceCharacter but not ClassSetSyntaxCharacter
    //                    | "\" CharacterEscape[+UnicodeMode]
    //                    | "\" ClassSetReservedPunctuator
    //                    | "\" b
    // ClassSetReservedDoublePunctuator :: one of "&&" "!!" "##" "$$" "%%" "**" "++" ",," ".." "::" ";;" "<<" "==" ">>" "??" "@@" "^^" "``" "~~"
    // ClassSetSyntaxCharacter :: one of "(" ")" "{" "}" "[" "]" "/" "-" "\" "|"
    // ClassSetReservedPunctuator :: one of "&" "-" "!" "#" "%" "," ":" ";" "<" "=" ">" "@" "`" "~"

    constexpr auto class_set_reserved_double_punctuator = Array {
        "&&"sv, "!!"sv, "##"sv, "$$"sv, "%%"sv, "**"sv, "++"sv, ",,"sv, ".."sv, "::"sv, ";;"sv, "<<"sv, "=="sv, ">>"sv, "??"sv, "@@"sv, "^^"sv, "``"sv, "~~"sv
    };

    if (done()) {
        set_error(Error::InvalidPattern);
        return {};
    }

    auto start_position = tell();
    ArmedScopeGuard restore { [&] { back(tell() - start_position + 1); } };

    if (try_skip("\\"sv)) {
        if (done()) {
            set_error(Error::InvalidTrailingEscape);
            return {};
        }

        // "\" ClassSetReservedPunctuator
        for (auto const& reserved : class_set_reserved_double_punctuator) {
            if (try_skip(reserved)) {
                // "\" ClassSetReservedPunctuator (ClassSetReservedPunctuator)
                back();

                restore.disarm();
                return reserved[0];
            }
        }
        // "\" b
        if (try_skip("b"sv)) {
            restore.disarm();
            return '\b';
        }

        // "\" CharacterEscape[+UnicodeMode]
        Vector<CompareTypeAndValuePair> compares;
        size_t minimum_length = 0;
        if (parse_character_escape(compares, minimum_length, { .unicode = true })) {
            VERIFY(compares.size() == 1);
            auto& compare = compares.first();
            VERIFY(compare.type == CharacterCompareType::Char);
            restore.disarm();
            return compare.value;
        }

        return {};
    }

    // [lookahead ∉ ClassSetReservedDoublePunctuator] SourceCharacter but not ClassSetSyntaxCharacter
    auto lookahead_matches = any_of(class_set_reserved_double_punctuator, [this](auto& reserved) {
        return try_skip(reserved);
    });

    if (lookahead_matches)
        return {};

    for (auto character : { "("sv, ")"sv, "{"sv, "}"sv, "["sv, "]"sv, "/"sv, "-"sv, "\\"sv, "|"sv }) {
        if (try_skip(character))
            return {};
    }

    restore.disarm();
    return skip();
}

bool ECMA262Parser::parse_class_set_operand(Vector<regex::CompareTypeAndValuePair>& compares)
{
    auto start_position = tell();

    // ClassSetOperand :: ClassSetCharacter | ClassStringDisjunction | NestedClass
    if (auto character = parse_class_set_character(); character.has_value()) {
        compares.append({ CharacterCompareType::Char, character.value() });
        return true;
    }

    // NestedClass :: "[" [lookahead != "^"] ClassContents[+UnicodeMode +UnicodeSetsMode] "]"
    //              | "[" "^" ClassContents[+UnicodeMode +UnicodeSetsMode] "]"
    //              | "\" CharacterClassEscape[+UnicodeMode]
    if (parse_nested_class(compares))
        return true;

    if (has_error())
        return false;

    auto negated = false;
    if (auto ch = parse_character_class_escape(negated, true); ch.has_value()) {
        if (negated)
            compares.append({ CharacterCompareType::TemporaryInverse, 1 });
        compares.append({ CharacterCompareType::CharClass, (ByteCodeValueType)ch.value() });
        return true;
    }

    PropertyEscape property {};
    if (parse_unicode_property_escape(property, negated)) {
        if (negated)
            compares.empend(CompareTypeAndValuePair { CharacterCompareType::Inverse, 0 });
        property.visit(
            [&](Unicode::Property property) {
                compares.empend(CompareTypeAndValuePair { CharacterCompareType::Property, (ByteCodeValueType)property.value() });
            },
            [&](Unicode::GeneralCategory general_category) {
                compares.empend(CompareTypeAndValuePair { CharacterCompareType::GeneralCategory, (ByteCodeValueType)general_category.value() });
            },
            [&](Script script) {
                if (script.is_extension)
                    compares.empend(CompareTypeAndValuePair { CharacterCompareType::ScriptExtension, (ByteCodeValueType)script.script.value() });
                else
                    compares.empend(CompareTypeAndValuePair { CharacterCompareType::Script, (ByteCodeValueType)script.script.value() });
            },
            [](Empty&) { VERIFY_NOT_REACHED(); });
        return true;
    }

    if (has_error())
        return false;

    // ClassStringDisjunction :: "\q{" ClassStringDisjunctionContents "}"
    // ClassStringDisjunctionContents :: ClassString | ClassString "|" ClassStringDisjunctionContents
    // ClassString :: [empty] | NonEmptyClassString
    // NonEmptyClassString :: ClassCharacter NonEmptyClassString[opt]
    if (try_skip("\\q{"sv)) {
        // FIXME: Implement this :P
        return set_error(Error::InvalidCharacterClass);
    }

    back(tell() - start_position + 1);
    return false;
}

bool ECMA262Parser::parse_nested_class(Vector<regex::CompareTypeAndValuePair>& compares)
{
    auto start_position = tell();

    // NestedClass :: "[" [lookahead ≠ ^ ] ClassContents [+UnicodeMode, +UnicodeSetsMode] "]"
    //              | "[" "^" ClassContents[+UnicodeMode, +UnicodeSetsMode] "]"
    //              | "\" CharacterClassEscape[+UnicodeMode]

    if (match(TokenType::LeftBracket)) {
        consume();

        compares.append(CompareTypeAndValuePair { CharacterCompareType::Or, 0 });

        if (match(TokenType::Circumflex)) {
            // Negated charclass
            consume();
            compares.empend(CompareTypeAndValuePair { CharacterCompareType::Inverse, 0 });
        }

        // ClassContents :: [empty]
        if (match(TokenType::RightBracket)) {
            consume();
            // Should only have at most an 'Inverse' (after an 'Or')
            if (m_parser_state.regex_options.has_flag_set(regex::AllFlags::UnicodeSets)) {
                // In unicode sets mode, we can have an additional 'And'/'Or' before the 'Inverse'.
                VERIFY(compares.size() <= 3);
            } else {
                VERIFY(compares.size() <= 2);
            }
            compares.append(CompareTypeAndValuePair { CharacterCompareType::EndAndOr, 0 });
            return true;
        }

        // ClassContents :: [+UnicodeSetsMode] ClassSetExpression
        if (!parse_class_set_expression(compares))
            return false;

        compares.append(CompareTypeAndValuePair { CharacterCompareType::EndAndOr, 0 });
        return true;
    }

    if (try_skip("\\"sv)) {
        auto negated = false;
        if (auto char_class = parse_character_class_escape(negated); char_class.has_value()) {
            if (negated)
                compares.append({ CharacterCompareType::TemporaryInverse, 1 });
            compares.append({ CharacterCompareType::CharClass, (ByteCodeValueType)char_class.value() });
            return true;
        }

        PropertyEscape property {};
        if (parse_unicode_property_escape(property, negated)) {
            if (negated)
                compares.empend(CompareTypeAndValuePair { CharacterCompareType::Inverse, 0 });
            property.visit(
                [&](Unicode::Property property) {
                    compares.empend(CompareTypeAndValuePair { CharacterCompareType::Property, (ByteCodeValueType)property.value() });
                },
                [&](Unicode::GeneralCategory general_category) {
                    compares.empend(CompareTypeAndValuePair { CharacterCompareType::GeneralCategory, (ByteCodeValueType)general_category.value() });
                },
                [&](Script script) {
                    if (script.is_extension)
                        compares.empend(CompareTypeAndValuePair { CharacterCompareType::ScriptExtension, (ByteCodeValueType)script.script.value() });
                    else
                        compares.empend(CompareTypeAndValuePair { CharacterCompareType::Script, (ByteCodeValueType)script.script.value() });
                },
                [](Empty&) { VERIFY_NOT_REACHED(); });
            return true;
        }

        if (has_error())
            return false;
    }

    back(tell() - start_position + 1);
    return false;
}

bool ECMA262Parser::parse_unicode_property_escape(PropertyEscape& property, bool& negated)
{
    negated = false;

    if (try_skip("p"sv))
        negated = false;
    else if (try_skip("P"sv))
        negated = true;
    else
        return false;

    auto parsed_property = read_unicode_property_escape();
    if (!parsed_property.has_value()) {
        set_error(Error::InvalidNameForProperty);
        return false;
    }

    property = move(*parsed_property);

    return property.visit(
        [this](Unicode::Property property) {
            if (!Unicode::is_ecma262_property(property)) {
                set_error(Error::InvalidNameForProperty);
                return false;
            }
            return true;
        },
        [](Unicode::GeneralCategory) { return true; },
        [](Script) { return true; },
        [](Empty&) -> bool { VERIFY_NOT_REACHED(); });
}

DeprecatedFlyString ECMA262Parser::read_capture_group_specifier(bool take_starting_angle_bracket)
{
    static constexpr u32 const REPLACEMENT_CHARACTER = 0xFFFD;
    constexpr u32 const ZERO_WIDTH_NON_JOINER { 0x200C };
    constexpr u32 const ZERO_WIDTH_JOINER { 0x200D };

    if (take_starting_angle_bracket && !consume("<"))
        return {};

    StringBuilder builder;

    auto consume_code_point = [&] {
        Utf8View utf_8_view { m_parser_state.lexer.source().substring_view(m_parser_state.lexer.tell() - 1) };
        if (utf_8_view.is_empty())
            return REPLACEMENT_CHARACTER;
        u32 code_point = *utf_8_view.begin();
        auto characters = utf_8_view.byte_offset_of(1);

        while (characters-- > 0)
            consume();

        return code_point;
    };

    {
        // The first character is limited to: https://tc39.es/ecma262/#prod-RegExpIdentifierStart
        //  RegExpIdentifierStart[UnicodeMode] ::
        //      IdentifierStartChar
        //      \ RegExpUnicodeEscapeSequence[+UnicodeMode]
        //      [~UnicodeMode] UnicodeLeadSurrogate UnicodeTrailSurrogate

        auto code_point = consume_code_point();

        if (code_point == '\\' && match('u')) {
            consume();

            if (auto maybe_code_point = consume_escaped_code_point(true); maybe_code_point.has_value()) {
                code_point = *maybe_code_point;
            } else {
                set_error(Error::InvalidNameForCaptureGroup);
                return {};
            }
        }

        if (is_ascii(code_point)) {
            // The only valid ID_Start unicode characters in ascii are the letters.
            if (!is_ascii_alpha(code_point) && code_point != '$' && code_point != '_') {
                set_error(Error::InvalidNameForCaptureGroup);
                return {};
            }
        } else if (!Unicode::code_point_has_identifier_start_property(code_point)) {
            set_error(Error::InvalidNameForCaptureGroup);
            return {};
        }
        builder.append_code_point(code_point);
    }

    bool hit_end = false;

    // Any following characters are limited to:
    //  RegExpIdentifierPart[UnicodeMode] ::
    //      IdentifierPartChar
    //      \ RegExpUnicodeEscapeSequence[+UnicodeMode]
    //      [~UnicodeMode] UnicodeLeadSurrogate UnicodeTrailSurrogate

    while (match(TokenType::Char) || match(TokenType::Dollar) || match(TokenType::LeftCurly) || match(TokenType::RightCurly)) {
        auto code_point = consume_code_point();

        if (code_point == '>') {
            hit_end = true;
            break;
        }

        if (code_point == '\\') {
            if (!try_skip("u"sv)) {
                set_error(Error::InvalidNameForCaptureGroup);
                return {};
            }
            if (auto maybe_code_point = consume_escaped_code_point(true); maybe_code_point.has_value()) {
                code_point = *maybe_code_point;
            } else {
                set_error(Error::InvalidNameForCaptureGroup);
                return {};
            }
        }

        if (is_ascii(code_point)) {
            // The only valid ID_Continue unicode characters in ascii are the letters and numbers.
            if (!is_ascii_alphanumeric(code_point) && code_point != '$' && code_point != '_') {
                set_error(Error::InvalidNameForCaptureGroup);
                return {};
            }
        } else if (code_point != ZERO_WIDTH_JOINER && code_point != ZERO_WIDTH_NON_JOINER) {
            if (!Unicode::code_point_has_identifier_continue_property(code_point)) {
                set_error(Error::InvalidNameForCaptureGroup);
                return {};
            }
        }
        builder.append_code_point(code_point);
    }

    DeprecatedFlyString name = builder.to_byte_string();
    if (!hit_end || name.is_empty())
        set_error(Error::InvalidNameForCaptureGroup);

    return name;
}

Optional<ECMA262Parser::PropertyEscape> ECMA262Parser::read_unicode_property_escape()
{
    consume(TokenType::LeftCurly, Error::InvalidPattern);

    auto read_until = [&]<typename... Ts>(Ts&&... terminators) {
        auto start_token = m_parser_state.current_token;
        size_t offset = 0;

        while (match(TokenType::Char)) {
            if (m_parser_state.current_token.value().is_one_of(forward<Ts>(terminators)...))
                break;
            offset += consume().value().length();
        }

        return StringView { start_token.value().characters_without_null_termination(), offset };
    };

    StringView property_type;
    StringView property_name = read_until("="sv, "}"sv);

    if (try_skip("="sv)) {
        if (property_name.is_empty())
            return {};
        property_type = property_name;
        property_name = read_until("}"sv);
    }

    consume(TokenType::RightCurly, Error::InvalidPattern);

    if (property_type.is_empty()) {
        if (auto property = Unicode::property_from_string(property_name); property.has_value())
            return { *property };
        if (auto general_category = Unicode::general_category_from_string(property_name); general_category.has_value())
            return { *general_category };
    } else if ((property_type == "General_Category"sv) || (property_type == "gc"sv)) {
        if (auto general_category = Unicode::general_category_from_string(property_name); general_category.has_value())
            return { *general_category };
    } else if ((property_type == "Script"sv) || (property_type == "sc"sv)) {
        if (auto script = Unicode::script_from_string(property_name); script.has_value())
            return Script { *script, false };
    } else if ((property_type == "Script_Extensions"sv) || (property_type == "scx"sv)) {
        if (auto script = Unicode::script_from_string(property_name); script.has_value())
            return Script { *script, true };
    }

    return {};
}

bool ECMA262Parser::parse_capture_group(ByteCode& stack, size_t& match_length_minimum, ParseFlags flags)
{
    consume(TokenType::LeftParen, Error::InvalidPattern);

    auto register_capture_group_in_current_scope = [&](auto identifier) {
        m_capture_groups_in_scope.last().empend(identifier);
    };

    if (match(TokenType::Questionmark)) {
        // Non-capturing group or group with specifier.
        consume();

        if (match(TokenType::Colon)) {
            consume();
            ByteCode noncapture_group_bytecode;
            size_t length = 0;

            enter_capture_group_scope();
            if (!parse_disjunction(noncapture_group_bytecode, length, flags))
                return set_error(Error::InvalidPattern);
            clear_all_capture_groups_in_scope(stack);
            exit_capture_group_scope();

            consume(TokenType::RightParen, Error::MismatchingParen);

            stack.extend(move(noncapture_group_bytecode));
            match_length_minimum += length;
            return true;
        }

        if (consume("<")) {
            ++m_parser_state.named_capture_groups_count;
            auto group_index = ++m_parser_state.capture_groups_count; // Named capture groups count as normal capture groups too.
            auto name = read_capture_group_specifier();

            if (name.is_empty()) {
                set_error(Error::InvalidNameForCaptureGroup);
                return false;
            }

            if (has_duplicate_in_current_alternative(name)) {
                set_error(Error::DuplicateNamedCapture);
                return false;
            }

            m_parser_state.named_capture_groups.ensure(name).append({ group_index, m_current_alternative_id });

            ByteCode capture_group_bytecode;
            size_t length = 0;
            enter_capture_group_scope();
            if (!parse_disjunction(capture_group_bytecode, length, flags))
                return set_error(Error::InvalidPattern);
            clear_all_capture_groups_in_scope(stack);
            exit_capture_group_scope();

            register_capture_group_in_current_scope(group_index);

            consume(TokenType::RightParen, Error::MismatchingParen);

            stack.insert_bytecode_group_capture_left(group_index);
            stack.extend(move(capture_group_bytecode));
            stack.insert_bytecode_group_capture_right(group_index, name.view());

            match_length_minimum += length;

            m_parser_state.capture_group_minimum_lengths.set(group_index, length);
            return true;
        }

        set_error(Error::InvalidCaptureGroup);
        return false;
    }

    auto group_index = ++m_parser_state.capture_groups_count;
    enter_capture_group_scope();

    ByteCode capture_group_bytecode;
    size_t length = 0;

    if (!parse_disjunction(capture_group_bytecode, length, flags))
        return set_error(Error::InvalidPattern);

    clear_all_capture_groups_in_scope(stack);
    exit_capture_group_scope();

    register_capture_group_in_current_scope(group_index);

    stack.insert_bytecode_group_capture_left(group_index);
    stack.extend(move(capture_group_bytecode));

    m_parser_state.capture_group_minimum_lengths.set(group_index, length);

    consume(TokenType::RightParen, Error::MismatchingParen);

    stack.insert_bytecode_group_capture_right(group_index);

    match_length_minimum += length;

    return true;
}

size_t ECMA262Parser::ensure_total_number_of_capturing_parenthesis()
{
    if (m_total_number_of_capturing_parenthesis.has_value())
        return m_total_number_of_capturing_parenthesis.value();

    GenericLexer lexer { m_parser_state.lexer.source() };
    size_t count = 0;
    while (!lexer.is_eof()) {
        switch (lexer.peek()) {
        case '\\':
            lexer.consume(min(lexer.tell_remaining(), 2));
            continue;
        case '[':
            while (!lexer.is_eof()) {
                if (lexer.consume_specific('\\')) {
                    if (lexer.is_eof())
                        break;
                    lexer.consume();
                    continue;
                }
                if (lexer.consume_specific(']')) {
                    break;
                }

                if (lexer.is_eof())
                    break;
                lexer.consume();
            }
            break;
        case '(':
            lexer.consume();
            if (lexer.consume_specific('?')) {
                // non-capturing group '(?:', lookaround '(?<='/'(?<!', or named capture '(?<'
                if (!lexer.consume_specific('<'))
                    break;

                if (lexer.next_is(is_any_of("=!"sv)))
                    break;

                ++count;
            } else {
                ++count;
            }
            break;
        default:
            lexer.consume();
            break;
        }
    }

    m_total_number_of_capturing_parenthesis = count;
    return count;
}
}
