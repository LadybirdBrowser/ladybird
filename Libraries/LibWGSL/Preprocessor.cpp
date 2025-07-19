/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWGSL/Preprocessor.h>

namespace WGSL {

Preprocessor::Preprocessor(StringView input)
    : m_lexer(input)
{
}

ErrorOr<String> Preprocessor::process()
{
    return TRY(compute_remove_comments());
}

// https://www.w3.org/TR/WGSL/#parsing
ErrorOr<String> Preprocessor::compute_remove_comments()
{
    StringBuilder result;

    // 1. Remove comments
    // Replace the first comment with a space code point (U+0020). Repeat until no comments remain.
    while (!m_lexer.is_eof()) {
        char const c = m_lexer.peek();

        // https://www.w3.org/TR/WGSL/#comments
        if (c == '/' && m_lexer.peek(1) == '/') {
            m_lexer.consume();
            m_lexer.consume();
            m_lexer.ignore_while([](char ch) { return ch != '\n' && ch != '\r' && ch != '\0'; });
            result.append(' ');
            continue;
        }

        if (c == '/' && m_lexer.peek(1) == '*') {
            m_lexer.consume();
            m_lexer.consume();
            int nesting_level = 1;

            while (nesting_level > 0 && !m_lexer.is_eof()) {
                char const c1 = m_lexer.consume();
                if (m_lexer.is_eof()) {
                    return Error::from_string_literal("Unterminated block comment");
                }

                if (char const c2 = m_lexer.peek(); c1 == '/' && c2 == '*') {
                    m_lexer.consume();
                    nesting_level++;
                } else if (c1 == '*' && c2 == '/') {
                    m_lexer.consume();
                    nesting_level--;
                }
            }
            if (nesting_level > 0) {
                return Error::from_string_literal("Unterminated block comment");
            }
            result.append(' ');
            continue;
        }

        result.append(m_lexer.consume());
    }

    return TRY(result.to_string());
}

}
