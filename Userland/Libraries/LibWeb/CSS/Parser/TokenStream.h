/*
 * Copyright (c) 2020-2021, the SerenityOS developers.
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Format.h>
#include <AK/Vector.h>
#include <LibWeb/CSS/Parser/ComponentValue.h>
#include <LibWeb/CSS/Parser/Tokenizer.h>

namespace Web::CSS::Parser {

// https://drafts.csswg.org/css-syntax/#css-token-stream
template<typename T>
class TokenStream {
public:
    class StateTransaction {
    public:
        explicit StateTransaction(TokenStream<T>& token_stream)
            : m_token_stream(token_stream)
            , m_saved_index(token_stream.m_index)
        {
        }

        ~StateTransaction()
        {
            if (!m_commit)
                m_token_stream.m_index = m_saved_index;
        }

        StateTransaction create_child() { return StateTransaction(*this); }

        void commit()
        {
            m_commit = true;
            if (m_parent)
                m_parent->commit();
        }

    private:
        explicit StateTransaction(StateTransaction& parent)
            : m_parent(&parent)
            , m_token_stream(parent.m_token_stream)
            , m_saved_index(parent.m_token_stream.m_index)
        {
        }

        StateTransaction* m_parent { nullptr };
        TokenStream<T>& m_token_stream;
        size_t m_saved_index { 0 };
        bool m_commit { false };
    };

    explicit TokenStream(Span<T const> tokens)
        : m_tokens(tokens)
        , m_eof(make_eof())
    {
    }

    explicit TokenStream(Vector<T> const& tokens)
        : m_tokens(tokens.span())
        , m_eof(make_eof())
    {
    }

    static TokenStream<T> of_single_token(T const& token)
    {
        return TokenStream(Span<T const> { &token, 1 });
    }

    TokenStream(TokenStream<T> const&) = delete;
    TokenStream(TokenStream<T>&&) = default;

    // https://drafts.csswg.org/css-syntax/#token-stream-next-token
    [[nodiscard]] T const& next_token() const
    {
        // The item of tokens at index.
        // If that index would be out-of-bounds past the end of the list, it’s instead an <eof-token>.
        if (m_index < m_tokens.size())
            return m_tokens[m_index];
        return m_eof;
    }

    // https://drafts.csswg.org/css-syntax/#token-stream-empty
    [[nodiscard]] bool is_empty() const
    {
        // A token stream is empty if the next token is an <eof-token>.
        return next_token().is(Token::Type::EndOfFile);
    }

    // https://drafts.csswg.org/css-syntax/#token-stream-consume-a-token
    [[nodiscard]] T const& consume_a_token()
    {
        // Let token be the next token. Increment index, then return token.
        auto& token = next_token();
        ++m_index;
        return token;
    }

    // https://drafts.csswg.org/css-syntax/#token-stream-discard-a-token
    void discard_a_token()
    {
        // If the token stream is not empty, increment index.
        if (!is_empty())
            ++m_index;
    }

    // https://drafts.csswg.org/css-syntax/#token-stream-mark
    void mark()
    {
        // Append index to marked indexes.
        m_marked_indexes.append(m_index);
    }

    // https://drafts.csswg.org/css-syntax/#token-stream-restore-a-mark
    void restore_a_mark()
    {
        // Pop from marked indexes, and set index to the popped value.
        m_index = m_marked_indexes.take_last();
    }

    // https://drafts.csswg.org/css-syntax/#token-stream-discard-a-mark
    void discard_a_mark()
    {
        // Pop from marked indexes, and do nothing with the popped value.
        m_marked_indexes.take_last();
    }

    // https://drafts.csswg.org/css-syntax/#token-stream-discard-whitespace
    void discard_whitespace()
    {
        // While the next token is a <whitespace-token>, discard a token.
        while (next_token().is(Token::Type::Whitespace))
            discard_a_token();
    }

    bool has_next_token()
    {
        return !is_empty();
    }

    // Deprecated, used in older versions of the spec.
    T const& current_token()
    {
        if (m_index < 1 || (m_index - 1) >= m_tokens.size())
            return m_eof;

        return m_tokens.at(m_index - 1);
    }

    // Deprecated
    T const& peek_token(size_t offset = 0)
    {
        if (remaining_token_count() <= offset)
            return m_eof;

        return m_tokens.at(m_index + offset);
    }

    // Deprecated, was used in older versions of the spec.
    void reconsume_current_input_token()
    {
        if (m_index > 0)
            --m_index;
    }

    StateTransaction begin_transaction() { return StateTransaction(*this); }

    size_t remaining_token_count() const
    {
        if (m_tokens.size() > m_index)
            return m_tokens.size() - m_index;
        return 0;
    }

    void dump_all_tokens()
    {
        dbgln("Dumping all tokens:");
        for (size_t i = 0; i < m_tokens.size(); ++i) {
            auto& token = m_tokens[i];
            if (i == m_index)
                dbgln("-> {}", token.to_debug_string());
            else
                dbgln("   {}", token.to_debug_string());
        }
    }

    void copy_state(Badge<Parser>, TokenStream<T> const& other)
    {
        m_index = other.m_index;
    }

private:
    // https://drafts.csswg.org/css-syntax/#token-stream-tokens
    Span<T const> m_tokens;

    // https://drafts.csswg.org/css-syntax/#token-stream-index
    size_t m_index { 0 };

    // https://drafts.csswg.org/css-syntax/#token-stream-marked-indexes
    Vector<size_t> m_marked_indexes;

    T make_eof()
    {
        if constexpr (IsSame<T, Token>) {
            return Tokenizer::create_eof_token();
        }
        if constexpr (IsSame<T, ComponentValue>) {
            return ComponentValue(Tokenizer::create_eof_token());
        }
    }

    T m_eof;
};

}
