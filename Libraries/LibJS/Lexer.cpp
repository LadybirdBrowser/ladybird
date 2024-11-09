/*
 * Copyright (c) 2020, Stephan Unverwerth <s.unverwerth@serenityos.org>
 * Copyright (c) 2020-2021, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Lexer.h"
#include <AK/CharacterTypes.h>
#include <AK/Debug.h>
#include <AK/GenericLexer.h>
#include <AK/HashMap.h>
#include <AK/Utf8View.h>
#include <LibUnicode/CharacterTypes.h>
#include <stdio.h>

namespace JS {

HashMap<DeprecatedFlyString, TokenType> Lexer::s_keywords;

static constexpr TokenType parse_two_char_token(StringView view)
{
    if (view.length() != 2)
        return TokenType::Invalid;

    auto const* bytes = view.bytes().data();
    switch (bytes[0]) {
    case '=':
        switch (bytes[1]) {
        case '>':
            return TokenType::Arrow;
        case '=':
            return TokenType::EqualsEquals;
        default:
            return TokenType::Invalid;
        }
    case '+':
        switch (bytes[1]) {
        case '=':
            return TokenType::PlusEquals;
        case '+':
            return TokenType::PlusPlus;
        default:
            return TokenType::Invalid;
        }
    case '-':
        switch (bytes[1]) {
        case '=':
            return TokenType::MinusEquals;
        case '-':
            return TokenType::MinusMinus;
        default:
            return TokenType::Invalid;
        }
    case '*':
        switch (bytes[1]) {
        case '=':
            return TokenType::AsteriskEquals;
        case '*':
            return TokenType::DoubleAsterisk;
        default:
            return TokenType::Invalid;
        }
    case '/':
        switch (bytes[1]) {
        case '=':
            return TokenType::SlashEquals;
        default:
            return TokenType::Invalid;
        }
    case '%':
        switch (bytes[1]) {
        case '=':
            return TokenType::PercentEquals;
        default:
            return TokenType::Invalid;
        }
    case '&':
        switch (bytes[1]) {
        case '=':
            return TokenType::AmpersandEquals;
        case '&':
            return TokenType::DoubleAmpersand;
        default:
            return TokenType::Invalid;
        }
    case '|':
        switch (bytes[1]) {
        case '=':
            return TokenType::PipeEquals;
        case '|':
            return TokenType::DoublePipe;
        default:
            return TokenType::Invalid;
        }
    case '^':
        switch (bytes[1]) {
        case '=':
            return TokenType::CaretEquals;
        default:
            return TokenType::Invalid;
        }
    case '<':
        switch (bytes[1]) {
        case '=':
            return TokenType::LessThanEquals;
        case '<':
            return TokenType::ShiftLeft;
        default:
            return TokenType::Invalid;
        }
    case '>':
        switch (bytes[1]) {
        case '=':
            return TokenType::GreaterThanEquals;
        case '>':
            return TokenType::ShiftRight;
        default:
            return TokenType::Invalid;
        }
    case '?':
        switch (bytes[1]) {
        case '?':
            return TokenType::DoubleQuestionMark;
        case '.':
            return TokenType::QuestionMarkPeriod;
        default:
            return TokenType::Invalid;
        }
    case '!':
        switch (bytes[1]) {
        case '=':
            return TokenType::ExclamationMarkEquals;
        default:
            return TokenType::Invalid;
        }
    default:
        return TokenType::Invalid;
    }
}

static constexpr TokenType parse_three_char_token(StringView view)
{
    if (view.length() != 3)
        return TokenType::Invalid;

    auto const* bytes = view.bytes().data();
    switch (bytes[0]) {
    case '<':
        if (bytes[1] == '<' && bytes[2] == '=')
            return TokenType::ShiftLeftEquals;
        return TokenType::Invalid;
    case '>':
        if (bytes[1] == '>' && bytes[2] == '=')
            return TokenType::ShiftRightEquals;
        if (bytes[1] == '>' && bytes[2] == '>')
            return TokenType::UnsignedShiftRight;
        return TokenType::Invalid;
    case '=':
        if (bytes[1] == '=' && bytes[2] == '=')
            return TokenType::EqualsEqualsEquals;
        return TokenType::Invalid;
    case '!':
        if (bytes[1] == '=' && bytes[2] == '=')
            return TokenType::ExclamationMarkEqualsEquals;
        return TokenType::Invalid;
    case '.':
        if (bytes[1] == '.' && bytes[2] == '.')
            return TokenType::TripleDot;
        return TokenType::Invalid;
    case '*':
        if (bytes[1] == '*' && bytes[2] == '=')
            return TokenType::DoubleAsteriskEquals;
        return TokenType::Invalid;
    case '&':
        if (bytes[1] == '&' && bytes[2] == '=')
            return TokenType::DoubleAmpersandEquals;
        return TokenType::Invalid;
    case '|':
        if (bytes[1] == '|' && bytes[2] == '=')
            return TokenType::DoublePipeEquals;
        return TokenType::Invalid;
    case '?':
        if (bytes[1] == '?' && bytes[2] == '=')
            return TokenType::DoubleQuestionMarkEquals;
        return TokenType::Invalid;
    default:
        return TokenType::Invalid;
    }
}

static consteval Array<TokenType, 256> make_single_char_tokens_array()
{
    Array<TokenType, 256> array;
    array.fill(TokenType::Invalid);
    array['&'] = TokenType::Ampersand;
    array['*'] = TokenType::Asterisk;
    array['['] = TokenType::BracketOpen;
    array[']'] = TokenType::BracketClose;
    array['^'] = TokenType::Caret;
    array[':'] = TokenType::Colon;
    array[','] = TokenType::Comma;
    array['{'] = TokenType::CurlyOpen;
    array['}'] = TokenType::CurlyClose;
    array['='] = TokenType::Equals;
    array['!'] = TokenType::ExclamationMark;
    array['-'] = TokenType::Minus;
    array['('] = TokenType::ParenOpen;
    array[')'] = TokenType::ParenClose;
    array['%'] = TokenType::Percent;
    array['.'] = TokenType::Period;
    array['|'] = TokenType::Pipe;
    array['+'] = TokenType::Plus;
    array['?'] = TokenType::QuestionMark;
    array[';'] = TokenType::Semicolon;
    array['/'] = TokenType::Slash;
    array['~'] = TokenType::Tilde;
    array['<'] = TokenType::LessThan;
    array['>'] = TokenType::GreaterThan;
    return array;
}

static constexpr auto s_single_char_tokens = make_single_char_tokens_array();

Lexer::Lexer(StringView source, StringView filename, size_t line_number, size_t line_column)
    : m_source(source)
    , m_current_token(TokenType::Eof, {}, {}, {}, 0, 0, 0)
    , m_filename(String::from_utf8(filename).release_value_but_fixme_should_propagate_errors())
    , m_line_number(line_number)
    , m_line_column(line_column)
    , m_parsed_identifiers(adopt_ref(*new ParsedIdentifiers))
{
    if (s_keywords.is_empty()) {
        s_keywords.set("async", TokenType::Async);
        s_keywords.set("await", TokenType::Await);
        s_keywords.set("break", TokenType::Break);
        s_keywords.set("case", TokenType::Case);
        s_keywords.set("catch", TokenType::Catch);
        s_keywords.set("class", TokenType::Class);
        s_keywords.set("const", TokenType::Const);
        s_keywords.set("continue", TokenType::Continue);
        s_keywords.set("debugger", TokenType::Debugger);
        s_keywords.set("default", TokenType::Default);
        s_keywords.set("delete", TokenType::Delete);
        s_keywords.set("do", TokenType::Do);
        s_keywords.set("else", TokenType::Else);
        s_keywords.set("enum", TokenType::Enum);
        s_keywords.set("export", TokenType::Export);
        s_keywords.set("extends", TokenType::Extends);
        s_keywords.set("false", TokenType::BoolLiteral);
        s_keywords.set("finally", TokenType::Finally);
        s_keywords.set("for", TokenType::For);
        s_keywords.set("function", TokenType::Function);
        s_keywords.set("if", TokenType::If);
        s_keywords.set("import", TokenType::Import);
        s_keywords.set("in", TokenType::In);
        s_keywords.set("instanceof", TokenType::Instanceof);
        s_keywords.set("let", TokenType::Let);
        s_keywords.set("new", TokenType::New);
        s_keywords.set("null", TokenType::NullLiteral);
        s_keywords.set("return", TokenType::Return);
        s_keywords.set("super", TokenType::Super);
        s_keywords.set("switch", TokenType::Switch);
        s_keywords.set("this", TokenType::This);
        s_keywords.set("throw", TokenType::Throw);
        s_keywords.set("true", TokenType::BoolLiteral);
        s_keywords.set("try", TokenType::Try);
        s_keywords.set("typeof", TokenType::Typeof);
        s_keywords.set("var", TokenType::Var);
        s_keywords.set("void", TokenType::Void);
        s_keywords.set("while", TokenType::While);
        s_keywords.set("with", TokenType::With);
        s_keywords.set("yield", TokenType::Yield);
    }

    consume();
}

void Lexer::consume()
{
    auto did_reach_eof = [this] {
        if (m_position < m_source.length())
            return false;
        m_eof = true;
        m_current_char = '\0';
        m_position = m_source.length() + 1;
        m_line_column++;
        return true;
    };

    if (m_position > m_source.length())
        return;

    if (did_reach_eof())
        return;

    if (is_line_terminator()) {
        if constexpr (LEXER_DEBUG) {
            ByteString type;
            if (m_current_char == '\n')
                type = "LINE FEED";
            else if (m_current_char == '\r')
                type = "CARRIAGE RETURN";
            else if (m_source[m_position + 1] == (char)0xa8)
                type = "LINE SEPARATOR";
            else
                type = "PARAGRAPH SEPARATOR";
            dbgln("Found a line terminator: {}", type);
        }
        // This is a three-char line terminator, we need to increase m_position some more.
        // We might reach EOF and need to check again.
        if (m_current_char != '\n' && m_current_char != '\r') {
            m_position += 2;
            if (did_reach_eof())
                return;
        }

        // If the previous character is \r and the current one \n we already updated line number
        // and column - don't do it again. From https://tc39.es/ecma262/#sec-line-terminators:
        //   The sequence <CR><LF> is commonly used as a line terminator.
        //   It should be considered a single SourceCharacter for the purpose of reporting line numbers.
        auto second_char_of_crlf = m_position > 1 && m_source[m_position - 2] == '\r' && m_current_char == '\n';

        if (!second_char_of_crlf) {
            m_line_number++;
            m_line_column = 1;
            dbgln_if(LEXER_DEBUG, "Incremented line number, now at: line {}, column 1", m_line_number);
        } else {
            dbgln_if(LEXER_DEBUG, "Previous was CR, this is LF - not incrementing line number again.");
        }
    } else if (is_unicode_character()) {
        size_t char_size = 1;
        if ((m_current_char & 64) == 0) {
            m_hit_invalid_unicode = m_position;
        } else if ((m_current_char & 32) == 0) {
            char_size = 2;
        } else if ((m_current_char & 16) == 0) {
            char_size = 3;
        } else if ((m_current_char & 8) == 0) {
            char_size = 4;
        }

        VERIFY(char_size >= 1);
        --char_size;

        for (size_t i = m_position; i < m_position + char_size; i++) {
            if (i >= m_source.length() || (m_source[i] & 0b11000000) != 0b10000000) {
                m_hit_invalid_unicode = m_position;
                break;
            }
        }

        if (m_hit_invalid_unicode.has_value())
            m_position = m_source.length();
        else
            m_position += char_size;

        if (did_reach_eof())
            return;

        m_line_column++;
    } else {
        m_line_column++;
    }

    m_current_char = m_source[m_position++];
}

bool Lexer::consume_decimal_number()
{
    if (!is_ascii_digit(m_current_char))
        return false;

    while (is_ascii_digit(m_current_char) || match_numeric_literal_separator_followed_by(is_ascii_digit)) {
        consume();
    }
    return true;
}

bool Lexer::consume_exponent()
{
    consume();
    if (m_current_char == '-' || m_current_char == '+')
        consume();

    if (!is_ascii_digit(m_current_char))
        return false;

    return consume_decimal_number();
}

static constexpr bool is_octal_digit(char ch)
{
    return ch >= '0' && ch <= '7';
}

bool Lexer::consume_octal_number()
{
    consume();
    if (!is_octal_digit(m_current_char))
        return false;

    while (is_octal_digit(m_current_char) || match_numeric_literal_separator_followed_by(is_octal_digit))
        consume();

    return true;
}

bool Lexer::consume_hexadecimal_number()
{
    consume();
    if (!is_ascii_hex_digit(m_current_char))
        return false;

    while (is_ascii_hex_digit(m_current_char) || match_numeric_literal_separator_followed_by(is_ascii_hex_digit))
        consume();

    return true;
}

static constexpr bool is_binary_digit(char ch)
{
    return ch == '0' || ch == '1';
}

bool Lexer::consume_binary_number()
{
    consume();
    if (!is_binary_digit(m_current_char))
        return false;

    while (is_binary_digit(m_current_char) || match_numeric_literal_separator_followed_by(is_binary_digit))
        consume();

    return true;
}

template<typename Callback>
bool Lexer::match_numeric_literal_separator_followed_by(Callback callback) const
{
    if (m_position >= m_source.length())
        return false;
    return m_current_char == '_'
        && callback(m_source[m_position]);
}

bool Lexer::match(char a, char b) const
{
    if (m_position >= m_source.length())
        return false;

    return m_current_char == a
        && m_source[m_position] == b;
}

bool Lexer::match(char a, char b, char c) const
{
    if (m_position + 1 >= m_source.length())
        return false;

    return m_current_char == a
        && m_source[m_position] == b
        && m_source[m_position + 1] == c;
}

bool Lexer::match(char a, char b, char c, char d) const
{
    if (m_position + 2 >= m_source.length())
        return false;

    return m_current_char == a
        && m_source[m_position] == b
        && m_source[m_position + 1] == c
        && m_source[m_position + 2] == d;
}

bool Lexer::is_eof() const
{
    return m_eof;
}

ALWAYS_INLINE bool Lexer::is_line_terminator() const
{
    if (m_current_char == '\n' || m_current_char == '\r')
        return true;
    if (!is_unicode_character())
        return false;

    auto code_point = current_code_point();
    return code_point == LINE_SEPARATOR || code_point == PARAGRAPH_SEPARATOR;
}

ALWAYS_INLINE bool Lexer::is_unicode_character() const
{
    return (m_current_char & 128) != 0;
}

ALWAYS_INLINE u32 Lexer::current_code_point() const
{
    static constexpr u32 const REPLACEMENT_CHARACTER = 0xFFFD;
    if (m_position == 0)
        return REPLACEMENT_CHARACTER;
    auto substring = m_source.substring_view(m_position - 1);
    if (substring.is_empty())
        return REPLACEMENT_CHARACTER;
    if (is_ascii(substring[0]))
        return substring[0];
    Utf8View utf_8_view { substring };
    return *utf_8_view.begin();
}

bool Lexer::is_whitespace() const
{
    if (is_ascii_space(m_current_char))
        return true;
    if (!is_unicode_character())
        return false;
    auto code_point = current_code_point();
    if (code_point == NO_BREAK_SPACE || code_point == ZERO_WIDTH_NO_BREAK_SPACE)
        return true;
    return Unicode::code_point_has_space_separator_general_category(code_point);
}

// UnicodeEscapeSequence :: https://tc39.es/ecma262/#prod-UnicodeEscapeSequence
//          u Hex4Digits
//          u{ CodePoint }
Optional<u32> Lexer::is_identifier_unicode_escape(size_t& identifier_length) const
{
    GenericLexer lexer(source().substring_view(m_position - 1));

    if (auto code_point_or_error = lexer.consume_escaped_code_point(false); !code_point_or_error.is_error()) {
        identifier_length = lexer.tell();
        return code_point_or_error.value();
    }

    return {};
}

// IdentifierStart :: https://tc39.es/ecma262/#prod-IdentifierStart
//          UnicodeIDStart
//          $
//          _
//          \ UnicodeEscapeSequence
Optional<u32> Lexer::is_identifier_start(size_t& identifier_length) const
{
    u32 code_point = current_code_point();
    identifier_length = 1;

    if (code_point == '\\') {
        if (auto maybe_code_point = is_identifier_unicode_escape(identifier_length); maybe_code_point.has_value())
            code_point = *maybe_code_point;
        else
            return {};
    }

    if (is_ascii_alpha(code_point) || code_point == '_' || code_point == '$')
        return code_point;

    // Optimization: the first codepoint with the ID_Start property after A-Za-z is outside the
    // ASCII range (0x00AA), so we can skip code_point_has_property() for any ASCII characters.
    if (is_ascii(code_point))
        return {};

    if (Unicode::code_point_has_identifier_start_property(code_point))
        return code_point;

    return {};
}

// IdentifierPart :: https://tc39.es/ecma262/#prod-IdentifierPart
//          UnicodeIDContinue
//          $
//          \ UnicodeEscapeSequence
//          <ZWNJ>
//          <ZWJ>
Optional<u32> Lexer::is_identifier_middle(size_t& identifier_length) const
{
    u32 code_point = current_code_point();
    identifier_length = 1;

    if (code_point == '\\') {
        if (auto maybe_code_point = is_identifier_unicode_escape(identifier_length); maybe_code_point.has_value())
            code_point = *maybe_code_point;
        else
            return {};
    }

    if (is_ascii_alphanumeric(code_point) || (code_point == '$') || (code_point == ZERO_WIDTH_NON_JOINER) || (code_point == ZERO_WIDTH_JOINER))
        return code_point;

    // Optimization: the first codepoint with the ID_Continue property after A-Za-z0-9_ is outside the
    // ASCII range (0x00AA), so we can skip code_point_has_property() for any ASCII characters.
    if (code_point == '_')
        return code_point;
    if (is_ascii(code_point))
        return {};

    if (Unicode::code_point_has_identifier_continue_property(code_point))
        return code_point;

    return {};
}

bool Lexer::is_line_comment_start(bool line_has_token_yet) const
{
    return match('/', '/')
        || (m_allow_html_comments && match('<', '!', '-', '-'))
        // "-->" is considered a line comment start if the current line is only whitespace and/or
        // other block comment(s); or in other words: the current line does not have a token or
        // ongoing line comment yet
        || (m_allow_html_comments && !line_has_token_yet && match('-', '-', '>'))
        // https://tc39.es/ecma262/#sec-hashbang
        || (match('#', '!') && m_position == 1);
}

bool Lexer::is_block_comment_start() const
{
    return match('/', '*');
}

bool Lexer::is_block_comment_end() const
{
    return match('*', '/');
}

bool Lexer::is_numeric_literal_start() const
{
    return is_ascii_digit(m_current_char) || (m_current_char == '.' && m_position < m_source.length() && is_ascii_digit(m_source[m_position]));
}

bool Lexer::slash_means_division() const
{
    auto type = m_current_token.type();
    return m_current_token.is_identifier_name()
        || type == TokenType::BigIntLiteral
        || type == TokenType::BracketClose
        || type == TokenType::CurlyClose
        || type == TokenType::MinusMinus
        || type == TokenType::NumericLiteral
        || type == TokenType::ParenClose
        || type == TokenType::PlusPlus
        || type == TokenType::PrivateIdentifier
        || type == TokenType::RegexLiteral
        || type == TokenType::StringLiteral
        || type == TokenType::TemplateLiteralEnd;
}

Token Lexer::next()
{
    size_t trivia_start = m_position;
    auto in_template = !m_template_states.is_empty();
    bool line_has_token_yet = m_line_column > 1;
    bool unterminated_comment = false;

    if (!in_template || m_template_states.last().in_expr) {
        // consume whitespace and comments
        while (true) {
            if (is_line_terminator()) {
                line_has_token_yet = false;
                do {
                    consume();
                } while (is_line_terminator());
            } else if (is_whitespace()) {
                do {
                    consume();
                } while (is_whitespace());
            } else if (is_line_comment_start(line_has_token_yet)) {
                consume();
                do {
                    consume();
                } while (!is_eof() && !is_line_terminator());
            } else if (is_block_comment_start()) {
                size_t start_line_number = m_line_number;
                consume();
                do {
                    consume();
                } while (!is_eof() && !is_block_comment_end());
                if (is_eof())
                    unterminated_comment = true;
                consume(); // consume *
                if (is_eof())
                    unterminated_comment = true;
                consume(); // consume /

                if (start_line_number != m_line_number)
                    line_has_token_yet = false;
            } else {
                break;
            }
        }
    }

    size_t value_start = m_position;
    size_t value_start_line_number = m_line_number;
    size_t value_start_column_number = m_line_column;
    auto token_type = TokenType::Invalid;
    auto did_consume_whitespace_or_comments = trivia_start != value_start;
    // This is being used to communicate info about invalid tokens to the parser, which then
    // can turn that into more specific error messages - instead of us having to make up a
    // bunch of Invalid* tokens (bad numeric literals, unterminated comments etc.)
    StringView token_message;

    Optional<DeprecatedFlyString> identifier;
    size_t identifier_length = 0;

    if (m_current_token.type() == TokenType::RegexLiteral && !is_eof() && is_ascii_alpha(m_current_char) && !did_consume_whitespace_or_comments) {
        token_type = TokenType::RegexFlags;
        while (!is_eof() && is_ascii_alpha(m_current_char))
            consume();
    } else if (m_current_char == '`') {
        consume();

        if (!in_template) {
            token_type = TokenType::TemplateLiteralStart;
            m_template_states.append({ false, 0 });
        } else {
            if (m_template_states.last().in_expr) {
                m_template_states.append({ false, 0 });
                token_type = TokenType::TemplateLiteralStart;
            } else {
                m_template_states.take_last();
                token_type = TokenType::TemplateLiteralEnd;
            }
        }
    } else if (in_template && m_template_states.last().in_expr && m_template_states.last().open_bracket_count == 0 && m_current_char == '}') {
        consume();
        token_type = TokenType::TemplateLiteralExprEnd;
        m_template_states.last().in_expr = false;
    } else if (in_template && !m_template_states.last().in_expr) {
        if (is_eof()) {
            token_type = TokenType::UnterminatedTemplateLiteral;
            m_template_states.take_last();
        } else if (match('$', '{')) {
            token_type = TokenType::TemplateLiteralExprStart;
            consume();
            consume();
            m_template_states.last().in_expr = true;
        } else {
            // TemplateCharacter ::
            //     $ [lookahead ≠ {]
            //     \ TemplateEscapeSequence
            //     \ NotEscapeSequence
            //     LineContinuation
            //     LineTerminatorSequence
            //     SourceCharacter but not one of ` or \ or $ or LineTerminator
            while (!match('$', '{') && m_current_char != '`' && !is_eof()) {
                if (match('\\', '$') || match('\\', '`') || match('\\', '\\'))
                    consume();
                consume();
            }
            if (is_eof() && !m_template_states.is_empty())
                token_type = TokenType::UnterminatedTemplateLiteral;
            else
                token_type = TokenType::TemplateLiteralString;
        }
    } else if (m_current_char == '#') {
        // Note: This has some duplicated code with the identifier lexing below
        consume();
        auto code_point = is_identifier_start(identifier_length);
        if (code_point.has_value()) {
            StringBuilder builder;
            builder.append_code_point('#');
            do {
                builder.append_code_point(*code_point);
                for (size_t i = 0; i < identifier_length; ++i)
                    consume();

                code_point = is_identifier_middle(identifier_length);
            } while (code_point.has_value());

            identifier = builder.string_view();
            token_type = TokenType::PrivateIdentifier;

            m_parsed_identifiers->identifiers.set(*identifier);
        } else {
            token_type = TokenType::Invalid;
            token_message = "Start of private name '#' but not followed by valid identifier"sv;
        }
    } else if (auto code_point = is_identifier_start(identifier_length); code_point.has_value()) {
        bool has_escaped_character = false;
        // identifier or keyword
        StringBuilder builder;
        do {
            builder.append_code_point(*code_point);
            for (size_t i = 0; i < identifier_length; ++i)
                consume();

            has_escaped_character |= identifier_length > 1;

            code_point = is_identifier_middle(identifier_length);
        } while (code_point.has_value());

        identifier = builder.string_view();
        m_parsed_identifiers->identifiers.set(*identifier);

        auto it = s_keywords.find(identifier->hash(), [&](auto& entry) { return entry.key == identifier; });
        if (it == s_keywords.end())
            token_type = TokenType::Identifier;
        else
            token_type = has_escaped_character ? TokenType::EscapedKeyword : it->value;
    } else if (is_numeric_literal_start()) {
        token_type = TokenType::NumericLiteral;
        bool is_invalid_numeric_literal = false;
        if (m_current_char == '0') {
            consume();
            if (m_current_char == '.') {
                // decimal
                consume();
                while (is_ascii_digit(m_current_char))
                    consume();
                if (m_current_char == 'e' || m_current_char == 'E')
                    is_invalid_numeric_literal = !consume_exponent();
            } else if (m_current_char == 'e' || m_current_char == 'E') {
                is_invalid_numeric_literal = !consume_exponent();
            } else if (m_current_char == 'o' || m_current_char == 'O') {
                // octal
                is_invalid_numeric_literal = !consume_octal_number();
                if (m_current_char == 'n') {
                    consume();
                    token_type = TokenType::BigIntLiteral;
                }
            } else if (m_current_char == 'b' || m_current_char == 'B') {
                // binary
                is_invalid_numeric_literal = !consume_binary_number();
                if (m_current_char == 'n') {
                    consume();
                    token_type = TokenType::BigIntLiteral;
                }
            } else if (m_current_char == 'x' || m_current_char == 'X') {
                // hexadecimal
                is_invalid_numeric_literal = !consume_hexadecimal_number();
                if (m_current_char == 'n') {
                    consume();
                    token_type = TokenType::BigIntLiteral;
                }
            } else if (m_current_char == 'n') {
                consume();
                token_type = TokenType::BigIntLiteral;
            } else if (is_ascii_digit(m_current_char)) {
                // octal without '0o' prefix. Forbidden in 'strict mode'
                do {
                    consume();
                } while (is_ascii_digit(m_current_char));
            }
        } else {
            // 1...9 or period
            while (is_ascii_digit(m_current_char) || match_numeric_literal_separator_followed_by(is_ascii_digit))
                consume();
            if (m_current_char == 'n') {
                consume();
                token_type = TokenType::BigIntLiteral;
            } else {
                if (m_current_char == '.') {
                    consume();
                    if (m_current_char == '_')
                        is_invalid_numeric_literal = true;

                    while (is_ascii_digit(m_current_char) || match_numeric_literal_separator_followed_by(is_ascii_digit)) {
                        consume();
                    }
                }
                if (m_current_char == 'e' || m_current_char == 'E')
                    is_invalid_numeric_literal = is_invalid_numeric_literal || !consume_exponent();
            }
        }
        if (is_invalid_numeric_literal) {
            token_type = TokenType::Invalid;
            token_message = "Invalid numeric literal"sv;
        }
    } else if (m_current_char == '"' || m_current_char == '\'') {
        char stop_char = m_current_char;
        consume();
        // Note: LS/PS line terminators are allowed in string literals.
        while (m_current_char != stop_char && m_current_char != '\r' && m_current_char != '\n' && !is_eof()) {
            if (m_current_char == '\\') {
                consume();
                if (m_current_char == '\r' && m_position < m_source.length() && m_source[m_position] == '\n') {
                    consume();
                }
            }
            consume();
        }
        if (m_current_char != stop_char) {
            token_type = TokenType::UnterminatedStringLiteral;
        } else {
            consume();
            token_type = TokenType::StringLiteral;
        }
    } else if (m_current_char == '/' && !slash_means_division()) {
        consume();
        token_type = consume_regex_literal();
    } else if (m_eof) {
        if (unterminated_comment) {
            token_type = TokenType::Invalid;
            token_message = "Unterminated multi-line comment"sv;
        } else {
            token_type = TokenType::Eof;
        }
    } else {
        // There is only one four-char operator: >>>=
        bool found_four_char_token = false;
        if (match('>', '>', '>', '=')) {
            found_four_char_token = true;
            consume();
            consume();
            consume();
            consume();
            token_type = TokenType::UnsignedShiftRightEquals;
        }

        bool found_three_char_token = false;
        if (!found_four_char_token && m_position + 1 < m_source.length()) {
            auto three_chars_view = m_source.substring_view(m_position - 1, 3);
            if (auto type = parse_three_char_token(three_chars_view); type != TokenType::Invalid) {
                found_three_char_token = true;
                consume();
                consume();
                consume();
                token_type = type;
            }
        }

        bool found_two_char_token = false;
        if (!found_four_char_token && !found_three_char_token && m_position < m_source.length()) {
            auto two_chars_view = m_source.substring_view(m_position - 1, 2);
            if (auto type = parse_two_char_token(two_chars_view); type != TokenType::Invalid) {
                // OptionalChainingPunctuator :: ?. [lookahead ∉ DecimalDigit]
                if (!(type == TokenType::QuestionMarkPeriod && m_position + 1 < m_source.length() && is_ascii_digit(m_source[m_position + 1]))) {
                    found_two_char_token = true;
                    consume();
                    consume();
                    token_type = type;
                }
            }
        }

        bool found_one_char_token = false;
        if (!found_four_char_token && !found_three_char_token && !found_two_char_token) {
            if (auto type = s_single_char_tokens[static_cast<u8>(m_current_char)]; type != TokenType::Invalid) {
                found_one_char_token = true;
                consume();
                token_type = type;
            }
        }

        if (!found_four_char_token && !found_three_char_token && !found_two_char_token && !found_one_char_token) {
            consume();
            token_type = TokenType::Invalid;
        }
    }

    if (!m_template_states.is_empty() && m_template_states.last().in_expr) {
        if (token_type == TokenType::CurlyOpen) {
            m_template_states.last().open_bracket_count++;
        } else if (token_type == TokenType::CurlyClose) {
            m_template_states.last().open_bracket_count--;
        }
    }

    if (m_hit_invalid_unicode.has_value()) {
        value_start = m_hit_invalid_unicode.value() - 1;
        m_current_token = Token(TokenType::Invalid, "Invalid unicode codepoint in source"_string,
            ""sv, // Since the invalid unicode can occur anywhere in the current token the trivia is not correct
            m_source.substring_view(value_start + 1, min(4u, m_source.length() - value_start - 2)),
            m_line_number,
            m_line_column - 1,
            value_start + 1);
        m_hit_invalid_unicode.clear();
        // Do not produce any further tokens.
        VERIFY(is_eof());
    } else {
        m_current_token = Token(
            token_type,
            token_message,
            m_source.substring_view(trivia_start - 1, value_start - trivia_start),
            m_source.substring_view(value_start - 1, m_position - value_start),
            value_start_line_number,
            value_start_column_number,
            value_start - 1);
    }

    if (identifier.has_value())
        m_current_token.set_identifier_value(identifier.release_value());

    if constexpr (LEXER_DEBUG) {
        dbgln("------------------------------");
        dbgln("Token: {}", m_current_token.name());
        dbgln("Trivia: _{}_", m_current_token.trivia());
        dbgln("Value: _{}_", m_current_token.value());
        dbgln("Line: {}, Column: {}", m_current_token.line_number(), m_current_token.line_column());
        dbgln("------------------------------");
    }

    return m_current_token;
}

Token Lexer::force_slash_as_regex()
{
    VERIFY(m_current_token.type() == TokenType::Slash || m_current_token.type() == TokenType::SlashEquals);

    bool has_equals = m_current_token.type() == TokenType::SlashEquals;

    VERIFY(m_position > 0);
    size_t value_start = m_position - 1;

    if (has_equals) {
        VERIFY(m_source[value_start - 1] == '=');
        --value_start;
        --m_position;
        m_current_char = '=';
    }

    TokenType token_type = consume_regex_literal();

    m_current_token = Token(
        token_type,
        String {},
        m_current_token.trivia(),
        m_source.substring_view(value_start - 1, m_position - value_start),
        m_current_token.line_number(),
        m_current_token.line_column(),
        value_start - 1);

    if constexpr (LEXER_DEBUG) {
        dbgln("------------------------------");
        dbgln("Token: {}", m_current_token.name());
        dbgln("Trivia: _{}_", m_current_token.trivia());
        dbgln("Value: _{}_", m_current_token.value());
        dbgln("Line: {}, Column: {}", m_current_token.line_number(), m_current_token.line_column());
        dbgln("------------------------------");
    }

    return m_current_token;
}

TokenType Lexer::consume_regex_literal()
{
    while (!is_eof()) {
        if (is_line_terminator() || (!m_regex_is_in_character_class && m_current_char == '/')) {
            break;
        } else if (m_current_char == '[') {
            m_regex_is_in_character_class = true;
        } else if (m_current_char == ']') {
            m_regex_is_in_character_class = false;
        } else if (!m_regex_is_in_character_class && m_current_char == '/') {
            break;
        }

        if (match('\\', '/') || match('\\', '[') || match('\\', '\\') || (m_regex_is_in_character_class && match('\\', ']')))
            consume();
        consume();
    }

    if (m_current_char == '/') {
        consume();
        return TokenType::RegexLiteral;
    }

    return TokenType::UnterminatedRegexLiteral;
}

}
