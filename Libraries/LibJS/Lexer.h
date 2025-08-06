/*
 * Copyright (c) 2020, Stephan Unverwerth <s.unverwerth@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/StringView.h>
#include <AK/Utf16String.h>
#include <LibJS/Export.h>
#include <LibJS/Token.h>

namespace JS {

class JS_API Lexer {
public:
    explicit Lexer(StringView source, StringView filename = "(unknown)"sv, size_t line_number = 1, size_t line_column = 0);
    explicit Lexer(Utf16String source, StringView filename = "(unknown)"sv, size_t line_number = 1, size_t line_column = 0);

    Token next();

    Utf16String const& source() const { return m_source; }
    String const& filename() const { return m_filename; }

    void disallow_html_comments() { m_allow_html_comments = false; }

    Token force_slash_as_regex();

private:
    void consume();
    bool consume_exponent();
    bool consume_octal_number();
    bool consume_hexadecimal_number();
    bool consume_binary_number();
    bool consume_decimal_number();

    u32 current_code_point() const;

    bool is_eof() const;
    bool is_line_terminator() const;
    bool is_whitespace() const;
    Optional<u32> is_identifier_unicode_escape(size_t& identifier_length) const;
    Optional<u32> is_identifier_start(size_t& identifier_length) const;
    Optional<u32> is_identifier_middle(size_t& identifier_length) const;
    bool is_line_comment_start(bool line_has_token_yet) const;
    bool is_block_comment_start() const;
    bool is_block_comment_end() const;
    bool is_numeric_literal_start() const;
    bool match(char16_t, char16_t) const;
    bool match(char16_t, char16_t, char16_t) const;
    bool match(char16_t, char16_t, char16_t, char16_t) const;
    template<typename Callback>
    bool match_numeric_literal_separator_followed_by(Callback) const;
    bool slash_means_division() const;

    TokenType consume_regex_literal();

    Utf16String m_source;
    size_t m_position { 0 };
    Token m_current_token;
    char16_t m_current_code_unit { 0 };
    bool m_eof { false };

    String m_filename;
    size_t m_line_number { 1 };
    size_t m_line_column { 0 };

    bool m_regex_is_in_character_class { false };

    struct TemplateState {
        bool in_expr;
        u8 open_bracket_count;
    };
    Vector<TemplateState> m_template_states;

    bool m_allow_html_comments { true };

    static HashMap<Utf16FlyString, TokenType> s_keywords;

    struct ParsedIdentifiers : public RefCounted<ParsedIdentifiers> {
        // Resolved identifiers must be kept alive for the duration of the parsing stage, otherwise
        // the only references to these strings are deleted by the Token destructor.
        HashTable<Utf16FlyString> identifiers;
    };

    RefPtr<ParsedIdentifiers> m_parsed_identifiers;
};

bool is_syntax_character(u32 code_point);
bool is_whitespace(u32 code_point);
bool is_line_terminator(u32 code_point);

}
