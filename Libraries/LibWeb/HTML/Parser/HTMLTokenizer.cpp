/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/Debug.h>
#include <AK/GenericShorthands.h>
#include <AK/SourceLocation.h>
#include <LibTextCodec/Decoder.h>
#include <LibWeb/HTML/Parser/Entities.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/Parser/HTMLToken.h>
#include <LibWeb/HTML/Parser/HTMLTokenizer.h>
#include <LibWeb/Namespace.h>
#include <string.h>

namespace Web::HTML {

#pragma GCC diagnostic ignored "-Wunused-label"

#define CONSUME_NEXT_INPUT_CHARACTER \
    current_input_character = next_code_point(stop_at_insertion_point);

#define SWITCH_TO(new_state)                       \
    do {                                           \
        VERIFY(m_current_builder.is_empty());      \
        SWITCH_TO_WITH_UNCLEAN_BUILDER(new_state); \
    } while (0)

#define SWITCH_TO_WITH_UNCLEAN_BUILDER(new_state)                                                 \
    do {                                                                                          \
        will_switch_to(State::new_state);                                                         \
        m_state = State::new_state;                                                               \
        if (stop_at_insertion_point == StopAtInsertionPoint::Yes && is_insertion_point_reached()) \
            return {};                                                                            \
        CONSUME_NEXT_INPUT_CHARACTER;                                                             \
        goto new_state;                                                                           \
    } while (0)

#define RECONSUME_IN(new_state)              \
    do {                                     \
        will_reconsume_in(State::new_state); \
        m_state = State::new_state;          \
        goto new_state;                      \
    } while (0)

#define SWITCH_TO_RETURN_STATE          \
    do {                                \
        will_switch_to(m_return_state); \
        m_state = m_return_state;       \
        goto _StartOfFunction;          \
    } while (0)

#define RECONSUME_IN_RETURN_STATE                \
    do {                                         \
        will_reconsume_in(m_return_state);       \
        m_state = m_return_state;                \
        if (current_input_character.has_value()) \
            restore_to(m_prev_utf8_iterator);    \
        goto _StartOfFunction;                   \
    } while (0)

#define SWITCH_TO_AND_EMIT_CURRENT_TOKEN(new_state)     \
    do {                                                \
        VERIFY(m_current_builder.is_empty());           \
        will_switch_to(State::new_state);               \
        m_state = State::new_state;                     \
        will_emit(m_current_token);                     \
        m_queued_tokens.enqueue(move(m_current_token)); \
        return m_queued_tokens.dequeue();               \
    } while (0)

#define EMIT_CHARACTER_AND_RECONSUME_IN(code_point, new_state)          \
    do {                                                                \
        m_queued_tokens.enqueue(HTMLToken::make_character(code_point)); \
        will_reconsume_in(State::new_state);                            \
        m_state = State::new_state;                                     \
        goto new_state;                                                 \
    } while (0)

#define FLUSH_CODEPOINTS_CONSUMED_AS_A_CHARACTER_REFERENCE       \
    do {                                                         \
        for (auto code_point : m_temporary_buffer) {             \
            if (consumed_as_part_of_an_attribute()) {            \
                m_current_builder.append_code_point(code_point); \
            } else {                                             \
                create_new_token(HTMLToken::Type::Character);    \
                m_current_token.set_code_point(code_point);      \
                m_queued_tokens.enqueue(move(m_current_token));  \
            }                                                    \
        }                                                        \
    } while (0)

#define DONT_CONSUME_NEXT_INPUT_CHARACTER        \
    do {                                         \
        if (current_input_character.has_value()) \
            restore_to(m_prev_utf8_iterator);    \
    } while (0)

#define ON(code_point) \
    if (current_input_character.has_value() && current_input_character.value() == code_point)

#define ON_EOF \
    if (!current_input_character.has_value())

#define ON_ASCII_ALPHA \
    if (current_input_character.has_value() && is_ascii_alpha(current_input_character.value()))

#define ON_ASCII_ALPHANUMERIC \
    if (current_input_character.has_value() && is_ascii_alphanumeric(current_input_character.value()))

#define ON_ASCII_UPPER_ALPHA \
    if (current_input_character.has_value() && is_ascii_upper_alpha(current_input_character.value()))

#define ON_ASCII_LOWER_ALPHA \
    if (current_input_character.has_value() && is_ascii_lower_alpha(current_input_character.value()))

#define ON_ASCII_DIGIT \
    if (current_input_character.has_value() && is_ascii_digit(current_input_character.value()))

#define ON_ASCII_HEX_DIGIT \
    if (current_input_character.has_value() && is_ascii_hex_digit(current_input_character.value()))

#define ON_WHITESPACE \
    if (current_input_character.has_value() && is_ascii(*current_input_character) && first_is_one_of(static_cast<char>(*current_input_character), '\t', '\n', '\f', ' '))

#define ANYTHING_ELSE if (1)

#define EMIT_EOF                                        \
    do {                                                \
        if (m_has_emitted_eof)                          \
            return {};                                  \
        m_has_emitted_eof = true;                       \
        create_new_token(HTMLToken::Type::EndOfFile);   \
        will_emit(m_current_token);                     \
        m_queued_tokens.enqueue(move(m_current_token)); \
        return m_queued_tokens.dequeue();               \
    } while (0)

#define EMIT_CURRENT_TOKEN_FOLLOWED_BY_EOF              \
    do {                                                \
        VERIFY(m_current_builder.is_empty());           \
        will_emit(m_current_token);                     \
        m_queued_tokens.enqueue(move(m_current_token)); \
                                                        \
        m_has_emitted_eof = true;                       \
        create_new_token(HTMLToken::Type::EndOfFile);   \
        will_emit(m_current_token);                     \
        m_queued_tokens.enqueue(move(m_current_token)); \
                                                        \
        return m_queued_tokens.dequeue();               \
    } while (0)

#define EMIT_CHARACTER(code_point)                      \
    do {                                                \
        create_new_token(HTMLToken::Type::Character);   \
        m_current_token.set_code_point(code_point);     \
        m_queued_tokens.enqueue(move(m_current_token)); \
        return m_queued_tokens.dequeue();               \
    } while (0)

#define EMIT_CURRENT_CHARACTER \
    EMIT_CHARACTER(current_input_character.value());

#define SWITCH_TO_AND_EMIT_CHARACTER(code_point, new_state) \
    do {                                                    \
        will_switch_to(State::new_state);                   \
        m_state = State::new_state;                         \
        EMIT_CHARACTER(code_point);                         \
    } while (0)

#define SWITCH_TO_AND_EMIT_CURRENT_CHARACTER(new_state) \
    SWITCH_TO_AND_EMIT_CHARACTER(current_input_character.value(), new_state)

#define BEGIN_STATE(state) \
    state:                 \
    case State::state: {   \
        {                  \
            {

#define END_STATE         \
    VERIFY_NOT_REACHED(); \
    break;                \
    }                     \
    }                     \
    }

static inline void log_parse_error(SourceLocation const& location = SourceLocation::current())
{
    dbgln_if(TOKENIZER_TRACE_DEBUG, "Parse error (tokenization) {}", location);
}

Optional<u32> HTMLTokenizer::next_code_point(StopAtInsertionPoint stop_at_insertion_point)
{
    if (m_utf8_iterator == m_utf8_view.end())
        return {};

    u32 code_point;
    // https://html.spec.whatwg.org/multipage/parsing.html#preprocessing-the-input-stream:tokenization
    // https://infra.spec.whatwg.org/#normalize-newlines
    if (peek_code_point(0, stop_at_insertion_point).value_or(0) == '\r' && peek_code_point(1, stop_at_insertion_point).value_or(0) == '\n') {
        // replace every U+000D CR U+000A LF code point pair with a single U+000A LF code point,
        skip(2);
        code_point = '\n';
    } else if (peek_code_point(0, stop_at_insertion_point).value_or(0) == '\r') {
        // replace every remaining U+000D CR code point with a U+000A LF code point.
        skip(1);
        code_point = '\n';
    } else {
        skip(1);
        code_point = *m_prev_utf8_iterator;
    }

    dbgln_if(TOKENIZER_TRACE_DEBUG, "(Tokenizer) Next code_point: {}", code_point);
    return code_point;
}

void HTMLTokenizer::skip(size_t count)
{
    if (!m_source_positions.is_empty())
        m_source_positions.append(m_source_positions.last());
    for (size_t i = 0; i < count; ++i) {
        m_prev_utf8_iterator = m_utf8_iterator;
        auto code_point = *m_utf8_iterator;
        if (!m_source_positions.is_empty()) {
            if (code_point == '\n') {
                m_source_positions.last().column = 0;
                m_source_positions.last().line++;
            } else {
                m_source_positions.last().column++;
            }
            m_source_positions.last().byte_offset += m_utf8_iterator.underlying_code_point_length_in_bytes();
        }
        ++m_utf8_iterator;
    }
}

Optional<u32> HTMLTokenizer::peek_code_point(size_t offset, StopAtInsertionPoint stop_at_insertion_point) const
{
    auto it = m_utf8_iterator;
    for (size_t i = 0; i < offset && it != m_utf8_view.end(); ++i)
        ++it;
    if (stop_at_insertion_point == StopAtInsertionPoint::Yes
        && m_insertion_point.defined
        && m_utf8_view.byte_offset_of(it) >= m_insertion_point.position) {
        return {};
    }
    if (it == m_utf8_view.end())
        return {};
    return *it;
}

HTMLToken::Position HTMLTokenizer::nth_last_position(size_t n)
{
    if (n + 1 > m_source_positions.size()) {
        dbgln_if(TOKENIZER_TRACE_DEBUG, "(Tokenizer::nth_last_position) Invalid position requested: {}th-last of {}. Returning (0-0).", n, m_source_positions.size());
        return HTMLToken::Position { 0, 0 };
    };
    return m_source_positions.at(m_source_positions.size() - 1 - n);
}

Optional<HTMLToken> HTMLTokenizer::next_token(StopAtInsertionPoint stop_at_insertion_point)
{
    if (!m_source_positions.is_empty()) {
        auto last_position = m_source_positions.last();
        m_source_positions.clear_with_capacity();
        m_source_positions.append(move(last_position));
    }
_StartOfFunction:
    if (!m_queued_tokens.is_empty())
        return m_queued_tokens.dequeue();

    if (m_aborted)
        return {};

    for (;;) {
        if (stop_at_insertion_point == StopAtInsertionPoint::Yes && is_insertion_point_reached())
            return {};

        auto current_input_character = next_code_point(stop_at_insertion_point);
        switch (m_state) {
            // 13.2.5.1 Data state, https://html.spec.whatwg.org/multipage/parsing.html#data-state
            BEGIN_STATE(Data)
            {
                ON('&')
                {
                    m_return_state = State::Data;
                    SWITCH_TO(CharacterReference);
                }
                ON('<')
                {
                    SWITCH_TO(TagOpen);
                }
                ON(0)
                {
                    log_parse_error();
                    EMIT_CURRENT_CHARACTER;
                }
                ON_EOF
                {
                    EMIT_EOF;
                }
                ANYTHING_ELSE
                {
                    EMIT_CURRENT_CHARACTER;
                }
            }
            END_STATE

            // 13.2.5.6 Tag open state, https://html.spec.whatwg.org/multipage/parsing.html#tag-open-state
            BEGIN_STATE(TagOpen)
            {
                ON('!')
                {
                    SWITCH_TO(MarkupDeclarationOpen);
                }
                ON('/')
                {
                    SWITCH_TO(EndTagOpen);
                }
                ON_ASCII_ALPHA
                {
                    create_new_token(HTMLToken::Type::StartTag);
                    RECONSUME_IN(TagName);
                }
                ON('?')
                {
                    log_parse_error();
                    create_new_token(HTMLToken::Type::Comment);
                    m_current_token.set_start_position({}, nth_last_position(2));
                    RECONSUME_IN(BogusComment);
                }
                ON_EOF
                {
                    log_parse_error();
                    m_queued_tokens.enqueue(HTMLToken::make_character('<'));
                    EMIT_EOF;
                }
                ANYTHING_ELSE
                {
                    log_parse_error();
                    EMIT_CHARACTER_AND_RECONSUME_IN('<', Data);
                }
            }
            END_STATE

            // 13.2.5.8 Tag name state, https://html.spec.whatwg.org/multipage/parsing.html#tag-name-state
            BEGIN_STATE(TagName)
            {
                ON_WHITESPACE
                {
                    m_current_token.set_tag_name(consume_current_builder());
                    m_current_token.set_end_position({}, nth_last_position(1));
                    SWITCH_TO(BeforeAttributeName);
                }
                ON('/')
                {
                    m_current_token.set_tag_name(consume_current_builder());
                    m_current_token.set_end_position({}, nth_last_position(0));
                    SWITCH_TO(SelfClosingStartTag);
                }
                ON('>')
                {
                    m_current_token.set_tag_name(consume_current_builder());
                    SWITCH_TO_AND_EMIT_CURRENT_TOKEN(Data);
                }
                ON_ASCII_UPPER_ALPHA
                {
                    m_current_builder.append_code_point(to_ascii_lowercase(current_input_character.value()));
                    m_current_token.set_end_position({}, nth_last_position(0));
                    continue;
                }
                ON(0)
                {
                    log_parse_error();
                    m_current_builder.append_code_point(0xFFFD);
                    m_current_token.set_end_position({}, nth_last_position(0));
                    continue;
                }
                ON_EOF
                {
                    log_parse_error();
                    EMIT_EOF;
                }
                ANYTHING_ELSE
                {
                    m_current_builder.append_code_point(current_input_character.value());
                    m_current_token.set_end_position({}, nth_last_position(0));
                    continue;
                }
            }
            END_STATE

            // 13.2.5.7 End tag open state, https://html.spec.whatwg.org/multipage/parsing.html#end-tag-open-state
            BEGIN_STATE(EndTagOpen)
            {
                ON_ASCII_ALPHA
                {
                    create_new_token(HTMLToken::Type::EndTag);
                    RECONSUME_IN(TagName);
                }
                ON('>')
                {
                    log_parse_error();
                    SWITCH_TO(Data);
                }
                ON_EOF
                {
                    log_parse_error();
                    m_queued_tokens.enqueue(HTMLToken::make_character('<'));
                    m_queued_tokens.enqueue(HTMLToken::make_character('/'));
                    EMIT_EOF;
                }
                ANYTHING_ELSE
                {
                    log_parse_error();
                    create_new_token(HTMLToken::Type::Comment);
                    RECONSUME_IN(BogusComment);
                }
            }
            END_STATE

            // 13.2.5.42 Markup declaration open state, https://html.spec.whatwg.org/multipage/parsing.html#markup-declaration-open-state
            BEGIN_STATE(MarkupDeclarationOpen)
            {
                DONT_CONSUME_NEXT_INPUT_CHARACTER;

                switch (consume_next_if_match("--"sv, stop_at_insertion_point)) {
                case ConsumeNextResult::Consumed:
                    create_new_token(HTMLToken::Type::Comment);
                    m_current_token.set_start_position({}, nth_last_position(3));
                    SWITCH_TO(CommentStart);
                    break;
                case ConsumeNextResult::NotConsumed:
                    break;
                case ConsumeNextResult::RanOutOfCharacters:
                    return {};
                }

                switch (consume_next_if_match("DOCTYPE"sv, stop_at_insertion_point, CaseSensitivity::CaseInsensitive)) {
                case ConsumeNextResult::Consumed:
                    SWITCH_TO(DOCTYPE);
                    break;
                case ConsumeNextResult::NotConsumed:
                    break;
                case ConsumeNextResult::RanOutOfCharacters:
                    return {};
                }

                switch (consume_next_if_match("[CDATA["sv, stop_at_insertion_point)) {
                case ConsumeNextResult::Consumed:
                    // We keep the parser optional so that syntax highlighting can be lexer-only.
                    // The parser registers itself with the lexer it creates.
                    if (m_parser != nullptr
                        && m_parser->adjusted_current_node()
                        && m_parser->adjusted_current_node()->namespace_uri() != Namespace::HTML) {
                        SWITCH_TO(CDATASection);
                    } else {
                        create_new_token(HTMLToken::Type::Comment);
                        m_current_builder.append("[CDATA["sv);
                        SWITCH_TO_WITH_UNCLEAN_BUILDER(BogusComment);
                    }
                    break;
                case ConsumeNextResult::NotConsumed:
                    break;
                case ConsumeNextResult::RanOutOfCharacters:
                    return {};
                }
                ANYTHING_ELSE
                {
                    log_parse_error();
                    create_new_token(HTMLToken::Type::Comment);
                    SWITCH_TO(BogusComment);
                }
            }
            END_STATE

            // 13.2.5.41 Bogus comment state, https://html.spec.whatwg.org/multipage/parsing.html#bogus-comment-state
            BEGIN_STATE(BogusComment)
            {
                ON('>')
                {
                    m_current_token.set_comment(consume_current_builder());
                    SWITCH_TO_AND_EMIT_CURRENT_TOKEN(Data);
                }
                ON_EOF
                {
                    m_current_token.set_comment(consume_current_builder());
                    EMIT_CURRENT_TOKEN_FOLLOWED_BY_EOF;
                }
                ON(0)
                {
                    log_parse_error();
                    m_current_builder.append_code_point(0xFFFD);
                    continue;
                }
                ANYTHING_ELSE
                {
                    m_current_builder.append_code_point(current_input_character.value());
                    continue;
                }
            }
            END_STATE

            // 13.2.5.53 DOCTYPE state, https://html.spec.whatwg.org/multipage/parsing.html#doctype-state
            BEGIN_STATE(DOCTYPE)
            {
                ON_WHITESPACE
                {
                    SWITCH_TO(BeforeDOCTYPEName);
                }
                ON('>')
                {
                    RECONSUME_IN(BeforeDOCTYPEName);
                }
                ON_EOF
                {
                    log_parse_error();
                    create_new_token(HTMLToken::Type::DOCTYPE);
                    m_current_token.ensure_doctype_data().force_quirks = true;
                    m_queued_tokens.enqueue(move(m_current_token));
                    EMIT_EOF;
                }
                ANYTHING_ELSE
                {
                    log_parse_error();
                    RECONSUME_IN(BeforeDOCTYPEName);
                }
            }
            END_STATE

            // 13.2.5.54 Before DOCTYPE name state, https://html.spec.whatwg.org/multipage/parsing.html#before-doctype-name-state
            BEGIN_STATE(BeforeDOCTYPEName)
            {
                ON_WHITESPACE
                {
                    continue;
                }
                ON_ASCII_UPPER_ALPHA
                {
                    create_new_token(HTMLToken::Type::DOCTYPE);
                    m_current_builder.append_code_point(to_ascii_lowercase(current_input_character.value()));
                    m_current_token.ensure_doctype_data().missing_name = false;
                    SWITCH_TO_WITH_UNCLEAN_BUILDER(DOCTYPEName);
                }
                ON(0)
                {
                    log_parse_error();
                    create_new_token(HTMLToken::Type::DOCTYPE);
                    m_current_builder.append_code_point(0xFFFD);
                    m_current_token.ensure_doctype_data().missing_name = false;
                    SWITCH_TO_WITH_UNCLEAN_BUILDER(DOCTYPEName);
                }
                ON('>')
                {
                    log_parse_error();
                    create_new_token(HTMLToken::Type::DOCTYPE);
                    m_current_token.ensure_doctype_data().force_quirks = true;
                    SWITCH_TO_AND_EMIT_CURRENT_TOKEN(Data);
                }
                ON_EOF
                {
                    log_parse_error();
                    create_new_token(HTMLToken::Type::DOCTYPE);
                    m_current_token.ensure_doctype_data().force_quirks = true;
                    m_queued_tokens.enqueue(move(m_current_token));
                    EMIT_EOF;
                }
                ANYTHING_ELSE
                {
                    create_new_token(HTMLToken::Type::DOCTYPE);
                    m_current_builder.append_code_point(current_input_character.value());
                    m_current_token.ensure_doctype_data().missing_name = false;
                    SWITCH_TO_WITH_UNCLEAN_BUILDER(DOCTYPEName);
                }
            }
            END_STATE

            // 13.2.5.55 DOCTYPE name state, https://html.spec.whatwg.org/multipage/parsing.html#doctype-name-state
            BEGIN_STATE(DOCTYPEName)
            {
                ON_WHITESPACE
                {
                    m_current_token.ensure_doctype_data().name = consume_current_builder();
                    SWITCH_TO(AfterDOCTYPEName);
                }
                ON('>')
                {
                    m_current_token.ensure_doctype_data().name = consume_current_builder();
                    SWITCH_TO_AND_EMIT_CURRENT_TOKEN(Data);
                }
                ON_ASCII_UPPER_ALPHA
                {
                    m_current_builder.append_code_point(to_ascii_lowercase(current_input_character.value()));
                    continue;
                }
                ON(0)
                {
                    log_parse_error();
                    m_current_builder.append_code_point(0xFFFD);
                    continue;
                }
                ON_EOF
                {
                    log_parse_error();
                    m_current_token.ensure_doctype_data().force_quirks = true;
                    m_queued_tokens.enqueue(move(m_current_token));
                    EMIT_EOF;
                }
                ANYTHING_ELSE
                {
                    m_current_builder.append_code_point(current_input_character.value());
                    continue;
                }
            }
            END_STATE

            // 13.2.5.56 After DOCTYPE name state, https://html.spec.whatwg.org/multipage/parsing.html#after-doctype-name-state
            BEGIN_STATE(AfterDOCTYPEName)
            {
                ON_WHITESPACE
                {
                    continue;
                }
                ON('>')
                {
                    SWITCH_TO_AND_EMIT_CURRENT_TOKEN(Data);
                }
                ON_EOF
                {
                    log_parse_error();
                    m_current_token.ensure_doctype_data().force_quirks = true;
                    m_queued_tokens.enqueue(move(m_current_token));
                    EMIT_EOF;
                }
                ANYTHING_ELSE
                {
                    if (to_ascii_uppercase(current_input_character.value()) == 'P') {
                        switch (consume_next_if_match("UBLIC"sv, stop_at_insertion_point, CaseSensitivity::CaseInsensitive)) {
                        case ConsumeNextResult::Consumed:
                            SWITCH_TO(AfterDOCTYPEPublicKeyword);
                            break;
                        case ConsumeNextResult::NotConsumed:
                            break;
                        case ConsumeNextResult::RanOutOfCharacters:
                            DONT_CONSUME_NEXT_INPUT_CHARACTER;
                            return {};
                        }
                    }
                    if (to_ascii_uppercase(current_input_character.value()) == 'S') {
                        switch (consume_next_if_match("YSTEM"sv, stop_at_insertion_point, CaseSensitivity::CaseInsensitive)) {
                        case ConsumeNextResult::Consumed:
                            SWITCH_TO(AfterDOCTYPESystemKeyword);
                            break;
                        case ConsumeNextResult::NotConsumed:
                            break;
                        case ConsumeNextResult::RanOutOfCharacters:
                            DONT_CONSUME_NEXT_INPUT_CHARACTER;
                            return {};
                        }
                    }
                    log_parse_error();
                    m_current_token.ensure_doctype_data().force_quirks = true;
                    RECONSUME_IN(BogusDOCTYPE);
                }
            }
            END_STATE

            // 13.2.5.57 After DOCTYPE public keyword state, https://html.spec.whatwg.org/multipage/parsing.html#after-doctype-public-keyword-state
            BEGIN_STATE(AfterDOCTYPEPublicKeyword)
            {
                ON_WHITESPACE
                {
                    SWITCH_TO(BeforeDOCTYPEPublicIdentifier);
                }
                ON('"')
                {
                    log_parse_error();
                    m_current_token.ensure_doctype_data().missing_public_identifier = false;
                    SWITCH_TO(DOCTYPEPublicIdentifierDoubleQuoted);
                }
                ON('\'')
                {
                    log_parse_error();
                    m_current_token.ensure_doctype_data().missing_public_identifier = false;
                    SWITCH_TO(DOCTYPEPublicIdentifierSingleQuoted);
                }
                ON('>')
                {
                    log_parse_error();
                    m_current_token.ensure_doctype_data().force_quirks = true;
                    SWITCH_TO_AND_EMIT_CURRENT_TOKEN(Data);
                }
                ON_EOF
                {
                    log_parse_error();
                    m_current_token.ensure_doctype_data().force_quirks = true;
                    m_queued_tokens.enqueue(move(m_current_token));
                    EMIT_EOF;
                }
                ANYTHING_ELSE
                {
                    log_parse_error();
                    m_current_token.ensure_doctype_data().force_quirks = true;
                    RECONSUME_IN(BogusDOCTYPE);
                }
            }
            END_STATE

            // 13.2.5.63 After DOCTYPE system keyword state, https://html.spec.whatwg.org/multipage/parsing.html#after-doctype-system-keyword-state
            BEGIN_STATE(AfterDOCTYPESystemKeyword)
            {
                ON_WHITESPACE
                {
                    SWITCH_TO(BeforeDOCTYPESystemIdentifier);
                }
                ON('"')
                {
                    log_parse_error();
                    m_current_token.ensure_doctype_data().system_identifier = {};
                    m_current_token.ensure_doctype_data().missing_system_identifier = false;
                    SWITCH_TO(DOCTYPESystemIdentifierDoubleQuoted);
                }
                ON('\'')
                {
                    log_parse_error();
                    m_current_token.ensure_doctype_data().system_identifier = {};
                    m_current_token.ensure_doctype_data().missing_system_identifier = false;
                    SWITCH_TO(DOCTYPESystemIdentifierSingleQuoted);
                }
                ON('>')
                {
                    log_parse_error();
                    m_current_token.ensure_doctype_data().force_quirks = true;
                    SWITCH_TO_AND_EMIT_CURRENT_TOKEN(Data);
                }
                ON_EOF
                {
                    log_parse_error();
                    m_current_token.ensure_doctype_data().force_quirks = true;
                    m_queued_tokens.enqueue(move(m_current_token));
                    EMIT_EOF;
                }
                ANYTHING_ELSE
                {
                    log_parse_error();
                    m_current_token.ensure_doctype_data().force_quirks = true;
                    RECONSUME_IN(BogusDOCTYPE);
                }
            }
            END_STATE

            // 13.2.5.58 Before DOCTYPE public identifier state, https://html.spec.whatwg.org/multipage/parsing.html#before-doctype-public-identifier-state
            BEGIN_STATE(BeforeDOCTYPEPublicIdentifier)
            {
                ON_WHITESPACE
                {
                    continue;
                }
                ON('"')
                {
                    m_current_token.ensure_doctype_data().missing_public_identifier = false;
                    SWITCH_TO(DOCTYPEPublicIdentifierDoubleQuoted);
                }
                ON('\'')
                {
                    m_current_token.ensure_doctype_data().missing_public_identifier = false;
                    SWITCH_TO(DOCTYPEPublicIdentifierSingleQuoted);
                }
                ON('>')
                {
                    log_parse_error();
                    m_current_token.ensure_doctype_data().force_quirks = true;
                    SWITCH_TO_AND_EMIT_CURRENT_TOKEN(Data);
                }
                ON_EOF
                {
                    log_parse_error();
                    m_current_token.ensure_doctype_data().force_quirks = true;
                    m_queued_tokens.enqueue(move(m_current_token));
                    EMIT_EOF;
                }
                ANYTHING_ELSE
                {
                    log_parse_error();
                    m_current_token.ensure_doctype_data().force_quirks = true;
                    RECONSUME_IN(BogusDOCTYPE);
                }
            }
            END_STATE

            // 13.2.5.64 Before DOCTYPE system identifier state, https://html.spec.whatwg.org/multipage/parsing.html#before-doctype-system-identifier-state
            BEGIN_STATE(BeforeDOCTYPESystemIdentifier)
            {
                ON_WHITESPACE
                {
                    continue;
                }
                ON('"')
                {
                    m_current_token.ensure_doctype_data().missing_system_identifier = false;
                    SWITCH_TO(DOCTYPESystemIdentifierDoubleQuoted);
                }
                ON('\'')
                {
                    m_current_token.ensure_doctype_data().missing_system_identifier = false;
                    SWITCH_TO(DOCTYPESystemIdentifierSingleQuoted);
                }
                ON('>')
                {
                    log_parse_error();
                    m_current_token.ensure_doctype_data().force_quirks = true;
                    SWITCH_TO_AND_EMIT_CURRENT_TOKEN(Data);
                }
                ON_EOF
                {
                    log_parse_error();
                    m_current_token.ensure_doctype_data().force_quirks = true;
                    m_queued_tokens.enqueue(move(m_current_token));
                    EMIT_EOF;
                }
                ANYTHING_ELSE
                {
                    log_parse_error();
                    m_current_token.ensure_doctype_data().force_quirks = true;
                    RECONSUME_IN(BogusDOCTYPE);
                }
            }
            END_STATE

            // 13.2.5.59 DOCTYPE public identifier (double-quoted) state, https://html.spec.whatwg.org/multipage/parsing.html#doctype-public-identifier-(double-quoted)-state
            BEGIN_STATE(DOCTYPEPublicIdentifierDoubleQuoted)
            {
                ON('"')
                {
                    m_current_token.ensure_doctype_data().public_identifier = consume_current_builder();
                    SWITCH_TO(AfterDOCTYPEPublicIdentifier);
                }
                ON(0)
                {
                    log_parse_error();
                    m_current_builder.append_code_point(0xFFFD);
                    continue;
                }
                ON('>')
                {
                    log_parse_error();
                    m_current_token.ensure_doctype_data().public_identifier = consume_current_builder();
                    m_current_token.ensure_doctype_data().force_quirks = true;
                    SWITCH_TO_AND_EMIT_CURRENT_TOKEN(Data);
                }
                ON_EOF
                {
                    log_parse_error();
                    m_current_token.ensure_doctype_data().force_quirks = true;
                    m_queued_tokens.enqueue(move(m_current_token));
                    EMIT_EOF;
                }
                ANYTHING_ELSE
                {
                    m_current_builder.append_code_point(current_input_character.value());
                    continue;
                }
            }
            END_STATE

            // 13.2.5.60 DOCTYPE public identifier (single-quoted) state, https://html.spec.whatwg.org/multipage/parsing.html#doctype-public-identifier-(single-quoted)-state
            BEGIN_STATE(DOCTYPEPublicIdentifierSingleQuoted)
            {
                ON('\'')
                {
                    m_current_token.ensure_doctype_data().public_identifier = consume_current_builder();
                    SWITCH_TO(AfterDOCTYPEPublicIdentifier);
                }
                ON(0)
                {
                    log_parse_error();
                    m_current_builder.append_code_point(0xFFFD);
                    continue;
                }
                ON('>')
                {
                    log_parse_error();
                    m_current_token.ensure_doctype_data().public_identifier = consume_current_builder();
                    m_current_token.ensure_doctype_data().force_quirks = true;
                    SWITCH_TO_AND_EMIT_CURRENT_TOKEN(Data);
                }
                ON_EOF
                {
                    log_parse_error();
                    m_current_token.ensure_doctype_data().force_quirks = true;
                    m_queued_tokens.enqueue(move(m_current_token));
                    EMIT_EOF;
                }
                ANYTHING_ELSE
                {
                    m_current_builder.append_code_point(current_input_character.value());
                    continue;
                }
            }
            END_STATE

            // 13.2.5.65 DOCTYPE system identifier (double-quoted) state, https://html.spec.whatwg.org/multipage/parsing.html#doctype-system-identifier-(double-quoted)-state
            BEGIN_STATE(DOCTYPESystemIdentifierDoubleQuoted)
            {
                ON('"')
                {
                    m_current_token.ensure_doctype_data().system_identifier = consume_current_builder();
                    SWITCH_TO(AfterDOCTYPESystemIdentifier);
                }
                ON(0)
                {
                    log_parse_error();
                    m_current_builder.append_code_point(0xFFFD);
                    continue;
                }
                ON('>')
                {
                    log_parse_error();
                    m_current_token.ensure_doctype_data().system_identifier = consume_current_builder();
                    m_current_token.ensure_doctype_data().force_quirks = true;
                    SWITCH_TO_AND_EMIT_CURRENT_TOKEN(Data);
                }
                ON_EOF
                {
                    log_parse_error();
                    m_current_token.ensure_doctype_data().force_quirks = true;
                    m_queued_tokens.enqueue(move(m_current_token));
                    EMIT_EOF;
                }
                ANYTHING_ELSE
                {
                    m_current_builder.append_code_point(current_input_character.value());
                    continue;
                }
            }
            END_STATE

            // 13.2.5.66 DOCTYPE system identifier (single-quoted) state, https://html.spec.whatwg.org/multipage/parsing.html#doctype-system-identifier-(single-quoted)-state
            BEGIN_STATE(DOCTYPESystemIdentifierSingleQuoted)
            {
                ON('\'')
                {
                    m_current_token.ensure_doctype_data().system_identifier = consume_current_builder();
                    SWITCH_TO(AfterDOCTYPESystemIdentifier);
                }
                ON(0)
                {
                    log_parse_error();
                    m_current_builder.append_code_point(0xFFFD);
                    continue;
                }
                ON('>')
                {
                    log_parse_error();
                    m_current_token.ensure_doctype_data().system_identifier = consume_current_builder();
                    m_current_token.ensure_doctype_data().force_quirks = true;
                    SWITCH_TO_AND_EMIT_CURRENT_TOKEN(Data);
                }
                ON_EOF
                {
                    log_parse_error();
                    m_current_token.ensure_doctype_data().force_quirks = true;
                    m_queued_tokens.enqueue(move(m_current_token));
                    EMIT_EOF;
                }
                ANYTHING_ELSE
                {
                    m_current_builder.append_code_point(current_input_character.value());
                    continue;
                }
            }
            END_STATE

            // 13.2.5.61 After DOCTYPE public identifier state, https://html.spec.whatwg.org/multipage/parsing.html#after-doctype-public-identifier-state
            BEGIN_STATE(AfterDOCTYPEPublicIdentifier)
            {
                ON_WHITESPACE
                {
                    SWITCH_TO(BetweenDOCTYPEPublicAndSystemIdentifiers);
                }
                ON('>')
                {
                    SWITCH_TO_AND_EMIT_CURRENT_TOKEN(Data);
                }
                ON('"')
                {
                    log_parse_error();
                    m_current_token.ensure_doctype_data().missing_system_identifier = false;
                    SWITCH_TO(DOCTYPESystemIdentifierDoubleQuoted);
                }
                ON('\'')
                {
                    log_parse_error();
                    m_current_token.ensure_doctype_data().missing_system_identifier = false;
                    SWITCH_TO(DOCTYPESystemIdentifierSingleQuoted);
                }
                ON_EOF
                {
                    log_parse_error();
                    m_current_token.ensure_doctype_data().force_quirks = true;
                    m_queued_tokens.enqueue(move(m_current_token));
                    EMIT_EOF;
                }
                ANYTHING_ELSE
                {
                    log_parse_error();
                    m_current_token.ensure_doctype_data().force_quirks = true;
                    RECONSUME_IN(BogusDOCTYPE);
                }
            }
            END_STATE

            // 13.2.5.62 Between DOCTYPE public and system identifiers state, https://html.spec.whatwg.org/multipage/parsing.html#between-doctype-public-and-system-identifiers-state
            BEGIN_STATE(BetweenDOCTYPEPublicAndSystemIdentifiers)
            {
                ON_WHITESPACE
                {
                    continue;
                }
                ON('>')
                {
                    SWITCH_TO_AND_EMIT_CURRENT_TOKEN(Data);
                }
                ON('"')
                {
                    m_current_token.ensure_doctype_data().missing_system_identifier = false;
                    SWITCH_TO(DOCTYPESystemIdentifierDoubleQuoted);
                }
                ON('\'')
                {
                    m_current_token.ensure_doctype_data().missing_system_identifier = false;
                    SWITCH_TO(DOCTYPESystemIdentifierSingleQuoted);
                }
                ON_EOF
                {
                    log_parse_error();
                    m_current_token.ensure_doctype_data().force_quirks = true;
                    m_queued_tokens.enqueue(move(m_current_token));
                    EMIT_EOF;
                }
                ANYTHING_ELSE
                {
                    log_parse_error();
                    m_current_token.ensure_doctype_data().force_quirks = true;
                    RECONSUME_IN(BogusDOCTYPE);
                }
            }
            END_STATE

            // 13.2.5.67 After DOCTYPE system identifier state, https://html.spec.whatwg.org/multipage/parsing.html#after-doctype-system-identifier-state
            BEGIN_STATE(AfterDOCTYPESystemIdentifier)
            {
                ON_WHITESPACE
                {
                    continue;
                }
                ON('>')
                {
                    SWITCH_TO_AND_EMIT_CURRENT_TOKEN(Data);
                }
                ON_EOF
                {
                    log_parse_error();
                    m_current_token.ensure_doctype_data().force_quirks = true;
                    m_queued_tokens.enqueue(move(m_current_token));
                    EMIT_EOF;
                }
                ANYTHING_ELSE
                {
                    log_parse_error();
                    RECONSUME_IN(BogusDOCTYPE);
                }
            }
            END_STATE

            // 13.2.5.68 Bogus DOCTYPE state, https://html.spec.whatwg.org/multipage/parsing.html#bogus-doctype-state
            BEGIN_STATE(BogusDOCTYPE)
            {
                ON('>')
                {
                    SWITCH_TO_AND_EMIT_CURRENT_TOKEN(Data);
                }
                ON(0)
                {
                    log_parse_error();
                    continue;
                }
                ON_EOF
                {
                    m_queued_tokens.enqueue(move(m_current_token));
                    EMIT_EOF;
                }
                ANYTHING_ELSE
                {
                    continue;
                }
            }
            END_STATE

            // 13.2.5.32 Before attribute name state, https://html.spec.whatwg.org/multipage/parsing.html#before-attribute-name-state
            BEGIN_STATE(BeforeAttributeName)
            {
                ON_WHITESPACE
                {
                    continue;
                }
                ON('/')
                {
                    RECONSUME_IN(AfterAttributeName);
                }
                ON('>')
                {
                    RECONSUME_IN(AfterAttributeName);
                }
                ON_EOF
                {
                    RECONSUME_IN(AfterAttributeName);
                }
                ON('=')
                {
                    log_parse_error();
                    HTMLToken::Attribute new_attribute;
                    new_attribute.name_start_position = nth_last_position(1);
                    m_current_builder.append_code_point(current_input_character.value());
                    m_current_token.add_attribute(move(new_attribute));
                    SWITCH_TO_WITH_UNCLEAN_BUILDER(AttributeName);
                }
                ANYTHING_ELSE
                {
                    HTMLToken::Attribute new_attribute;
                    new_attribute.name_start_position = nth_last_position(1);
                    m_current_token.add_attribute(move(new_attribute));
                    RECONSUME_IN(AttributeName);
                }
            }
            END_STATE

            // 13.2.5.40 Self-closing start tag state, https://html.spec.whatwg.org/multipage/parsing.html#self-closing-start-tag-state
            BEGIN_STATE(SelfClosingStartTag)
            {
                ON('>')
                {
                    m_current_token.set_self_closing(true);
                    SWITCH_TO_AND_EMIT_CURRENT_TOKEN(Data);
                }
                ON_EOF
                {
                    log_parse_error();
                    EMIT_EOF;
                }
                ANYTHING_ELSE
                {
                    log_parse_error();
                    RECONSUME_IN(BeforeAttributeName);
                }
            }
            END_STATE

            // 13.2.5.33 Attribute name state, https://html.spec.whatwg.org/multipage/parsing.html#attribute-name-state
            BEGIN_STATE(AttributeName)
            {
                ON_WHITESPACE
                {
                    m_current_token.last_attribute().name_end_position = nth_last_position(1);
                    m_current_token.last_attribute().local_name = consume_current_builder();
                    RECONSUME_IN(AfterAttributeName);
                }
                ON('/')
                {
                    m_current_token.last_attribute().name_end_position = nth_last_position(1);
                    m_current_token.last_attribute().local_name = consume_current_builder();
                    RECONSUME_IN(AfterAttributeName);
                }
                ON('>')
                {
                    m_current_token.last_attribute().name_end_position = nth_last_position(1);
                    m_current_token.last_attribute().local_name = consume_current_builder();
                    RECONSUME_IN(AfterAttributeName);
                }
                ON_EOF
                {
                    m_current_token.last_attribute().name_end_position = nth_last_position(1);
                    m_current_token.last_attribute().local_name = consume_current_builder();
                    RECONSUME_IN(AfterAttributeName);
                }
                ON('=')
                {
                    m_current_token.last_attribute().name_end_position = nth_last_position(1);
                    m_current_token.last_attribute().local_name = consume_current_builder();
                    SWITCH_TO(BeforeAttributeValue);
                }
                ON_ASCII_UPPER_ALPHA
                {
                    m_current_builder.append_code_point(to_ascii_lowercase(current_input_character.value()));
                    continue;
                }
                ON(0)
                {
                    log_parse_error();
                    m_current_builder.append_code_point(0xFFFD);
                    continue;
                }
                ON('"')
                {
                    log_parse_error();
                    goto AnythingElseAttributeName;
                }
                ON('\'')
                {
                    log_parse_error();
                    goto AnythingElseAttributeName;
                }
                ON('<')
                {
                    log_parse_error();
                    goto AnythingElseAttributeName;
                }
                ANYTHING_ELSE
                {
                AnythingElseAttributeName:
                    m_current_builder.append_code_point(current_input_character.value());
                    continue;
                }
            }
            END_STATE

            // 13.2.5.34 After attribute name state, https://html.spec.whatwg.org/multipage/parsing.html#after-attribute-name-state
            BEGIN_STATE(AfterAttributeName)
            {
                ON_WHITESPACE
                {
                    continue;
                }
                ON('/')
                {
                    SWITCH_TO(SelfClosingStartTag);
                }
                ON('=')
                {
                    m_current_token.last_attribute().name_end_position = nth_last_position(1);
                    SWITCH_TO(BeforeAttributeValue);
                }
                ON('>')
                {
                    SWITCH_TO_AND_EMIT_CURRENT_TOKEN(Data);
                }
                ON_EOF
                {
                    log_parse_error();
                    EMIT_EOF;
                }
                ANYTHING_ELSE
                {
                    m_current_token.add_attribute({});
                    if (!m_source_positions.is_empty())
                        m_current_token.last_attribute().name_start_position = nth_last_position(1);
                    RECONSUME_IN(AttributeName);
                }
            }
            END_STATE

            // 13.2.5.35 Before attribute value state, https://html.spec.whatwg.org/multipage/parsing.html#before-attribute-value-state
            BEGIN_STATE(BeforeAttributeValue)
            {
                m_current_token.last_attribute().value_start_position = nth_last_position(1);
                ON_WHITESPACE
                {
                    continue;
                }
                ON('"')
                {
                    SWITCH_TO(AttributeValueDoubleQuoted);
                }
                ON('\'')
                {
                    SWITCH_TO(AttributeValueSingleQuoted);
                }
                ON('>')
                {
                    log_parse_error();
                    SWITCH_TO_AND_EMIT_CURRENT_TOKEN(Data);
                }
                ANYTHING_ELSE
                {
                    RECONSUME_IN(AttributeValueUnquoted);
                }
            }
            END_STATE

            // 13.2.5.36 Attribute value (double-quoted) state, https://html.spec.whatwg.org/multipage/parsing.html#attribute-value-(double-quoted)-state
            BEGIN_STATE(AttributeValueDoubleQuoted)
            {
                ON('"')
                {
                    m_current_token.last_attribute().value = consume_current_builder();
                    SWITCH_TO(AfterAttributeValueQuoted);
                }
                ON('&')
                {
                    m_return_state = State::AttributeValueDoubleQuoted;
                    SWITCH_TO_WITH_UNCLEAN_BUILDER(CharacterReference);
                }
                ON(0)
                {
                    log_parse_error();
                    m_current_builder.append_code_point(0xFFFD);
                    continue;
                }
                ON_EOF
                {
                    log_parse_error();
                    EMIT_EOF;
                }
                ANYTHING_ELSE
                {
                    m_current_builder.append_code_point(current_input_character.value());
                    continue;
                }
            }
            END_STATE

            // 13.2.5.37 Attribute value (single-quoted) state, https://html.spec.whatwg.org/multipage/parsing.html#attribute-value-(single-quoted)-state
            BEGIN_STATE(AttributeValueSingleQuoted)
            {
                ON('\'')
                {
                    m_current_token.last_attribute().value = consume_current_builder();
                    SWITCH_TO(AfterAttributeValueQuoted);
                }
                ON('&')
                {
                    m_return_state = State::AttributeValueSingleQuoted;
                    SWITCH_TO_WITH_UNCLEAN_BUILDER(CharacterReference);
                }
                ON(0)
                {
                    log_parse_error();
                    m_current_builder.append_code_point(0xFFFD);
                    continue;
                }
                ON_EOF
                {
                    log_parse_error();
                    EMIT_EOF;
                }
                ANYTHING_ELSE
                {
                    m_current_builder.append_code_point(current_input_character.value());
                    continue;
                }
            }
            END_STATE

            // 13.2.5.38 Attribute value (unquoted) state, https://html.spec.whatwg.org/multipage/parsing.html#attribute-value-(single-quoted)-state
            BEGIN_STATE(AttributeValueUnquoted)
            {
                ON_WHITESPACE
                {
                    m_current_token.last_attribute().value = consume_current_builder();
                    m_current_token.last_attribute().value_end_position = nth_last_position(1);
                    SWITCH_TO(BeforeAttributeName);
                }
                ON('&')
                {
                    m_return_state = State::AttributeValueUnquoted;
                    SWITCH_TO_WITH_UNCLEAN_BUILDER(CharacterReference);
                }
                ON('>')
                {
                    m_current_token.last_attribute().value = consume_current_builder();
                    m_current_token.last_attribute().value_end_position = nth_last_position(1);
                    SWITCH_TO_AND_EMIT_CURRENT_TOKEN(Data);
                }
                ON(0)
                {
                    log_parse_error();
                    m_current_builder.append_code_point(0xFFFD);
                    continue;
                }
                ON('"')
                {
                    log_parse_error();
                    goto AnythingElseAttributeValueUnquoted;
                }
                ON('\'')
                {
                    log_parse_error();
                    goto AnythingElseAttributeValueUnquoted;
                }
                ON('<')
                {
                    log_parse_error();
                    goto AnythingElseAttributeValueUnquoted;
                }
                ON('=')
                {
                    log_parse_error();
                    goto AnythingElseAttributeValueUnquoted;
                }
                ON('`')
                {
                    log_parse_error();
                    goto AnythingElseAttributeValueUnquoted;
                }
                ON_EOF
                {
                    log_parse_error();
                    EMIT_EOF;
                }
                ANYTHING_ELSE
                {
                AnythingElseAttributeValueUnquoted:
                    m_current_builder.append_code_point(current_input_character.value());
                    continue;
                }
            }
            END_STATE

            // 13.2.5.39 After attribute value (quoted) state, https://html.spec.whatwg.org/multipage/parsing.html#after-attribute-value-(quoted)-state
            BEGIN_STATE(AfterAttributeValueQuoted)
            {
                m_current_token.last_attribute().value_end_position = nth_last_position(1);
                ON_WHITESPACE
                {
                    SWITCH_TO(BeforeAttributeName);
                }
                ON('/')
                {
                    SWITCH_TO(SelfClosingStartTag);
                }
                ON('>')
                {
                    SWITCH_TO_AND_EMIT_CURRENT_TOKEN(Data);
                }
                ON_EOF
                {
                    log_parse_error();
                    EMIT_EOF;
                }
                ANYTHING_ELSE
                {
                    log_parse_error();
                    RECONSUME_IN(BeforeAttributeName);
                }
            }
            END_STATE

            // 13.2.5.43 Comment start state, https://html.spec.whatwg.org/multipage/parsing.html#comment-start-state
            BEGIN_STATE(CommentStart)
            {
                ON('-')
                {
                    SWITCH_TO(CommentStartDash);
                }
                ON('>')
                {
                    log_parse_error();
                    SWITCH_TO_AND_EMIT_CURRENT_TOKEN(Data);
                }
                ANYTHING_ELSE
                {
                    RECONSUME_IN(Comment);
                }
            }
            END_STATE

            // 13.2.5.44 Comment start dash state, https://html.spec.whatwg.org/multipage/parsing.html#comment-start-dash-state
            BEGIN_STATE(CommentStartDash)
            {
                ON('-')
                {
                    SWITCH_TO(CommentEnd);
                }
                ON('>')
                {
                    log_parse_error();
                    SWITCH_TO_AND_EMIT_CURRENT_TOKEN(Data);
                }
                ON_EOF
                {
                    log_parse_error();
                    EMIT_CURRENT_TOKEN_FOLLOWED_BY_EOF;
                }
                ANYTHING_ELSE
                {
                    m_current_builder.append('-');
                    RECONSUME_IN(Comment);
                }
            }
            END_STATE

            // 13.2.5.45 Comment state, https://html.spec.whatwg.org/multipage/parsing.html#comment-state
            BEGIN_STATE(Comment)
            {
                ON('<')
                {
                    m_current_builder.append_code_point(current_input_character.value());
                    SWITCH_TO_WITH_UNCLEAN_BUILDER(CommentLessThanSign);
                }
                ON('-')
                {
                    SWITCH_TO_WITH_UNCLEAN_BUILDER(CommentEndDash);
                }
                ON(0)
                {
                    log_parse_error();
                    m_current_builder.append_code_point(0xFFFD);
                    continue;
                }
                ON_EOF
                {
                    log_parse_error();
                    m_current_token.set_comment(consume_current_builder());
                    EMIT_CURRENT_TOKEN_FOLLOWED_BY_EOF;
                }
                ANYTHING_ELSE
                {
                    m_current_builder.append_code_point(current_input_character.value());
                    continue;
                }
            }
            END_STATE

            // 13.2.5.51 Comment end state, https://html.spec.whatwg.org/multipage/parsing.html#comment-end-state
            BEGIN_STATE(CommentEnd)
            {
                ON('>')
                {
                    m_current_token.set_comment(consume_current_builder());
                    SWITCH_TO_AND_EMIT_CURRENT_TOKEN(Data);
                }
                ON('!')
                {
                    SWITCH_TO_WITH_UNCLEAN_BUILDER(CommentEndBang);
                }
                ON('-')
                {
                    m_current_builder.append('-');
                    continue;
                }
                ON_EOF
                {
                    log_parse_error();
                    m_current_token.set_comment(consume_current_builder());
                    EMIT_CURRENT_TOKEN_FOLLOWED_BY_EOF;
                }
                ANYTHING_ELSE
                {
                    m_current_builder.append("--"sv);
                    RECONSUME_IN(Comment);
                }
            }
            END_STATE

            // 13.2.5.52 Comment end bang state, https://html.spec.whatwg.org/multipage/parsing.html#comment-end-bang-state
            BEGIN_STATE(CommentEndBang)
            {
                ON('-')
                {
                    m_current_builder.append("--!"sv);
                    SWITCH_TO_WITH_UNCLEAN_BUILDER(CommentEndDash);
                }
                ON('>')
                {
                    log_parse_error();
                    m_current_token.set_comment(consume_current_builder());
                    SWITCH_TO_AND_EMIT_CURRENT_TOKEN(Data);
                }
                ON_EOF
                {
                    log_parse_error();
                    m_current_token.set_comment(consume_current_builder());
                    EMIT_CURRENT_TOKEN_FOLLOWED_BY_EOF;
                }
                ANYTHING_ELSE
                {
                    m_current_builder.append("--!"sv);
                    RECONSUME_IN(Comment);
                }
            }
            END_STATE

            // 13.2.5.50 Comment end dash state, https://html.spec.whatwg.org/multipage/parsing.html#comment-end-dash-state
            BEGIN_STATE(CommentEndDash)
            {
                ON('-')
                {
                    SWITCH_TO_WITH_UNCLEAN_BUILDER(CommentEnd);
                }
                ON_EOF
                {
                    log_parse_error();
                    m_current_token.set_comment(consume_current_builder());
                    EMIT_CURRENT_TOKEN_FOLLOWED_BY_EOF;
                }
                ANYTHING_ELSE
                {
                    m_current_builder.append('-');
                    RECONSUME_IN(Comment);
                }
            }
            END_STATE

            // 13.2.5.46 Comment less-than sign state, https://html.spec.whatwg.org/multipage/parsing.html#comment-less-than-sign-state
            BEGIN_STATE(CommentLessThanSign)
            {
                ON('!')
                {
                    m_current_builder.append_code_point(current_input_character.value());
                    SWITCH_TO_WITH_UNCLEAN_BUILDER(CommentLessThanSignBang);
                }
                ON('<')
                {
                    m_current_builder.append_code_point(current_input_character.value());
                    continue;
                }
                ANYTHING_ELSE
                {
                    RECONSUME_IN(Comment);
                }
            }
            END_STATE

            // 13.2.5.47 Comment less-than sign bang state, https://html.spec.whatwg.org/multipage/parsing.html#comment-less-than-sign-bang-state
            BEGIN_STATE(CommentLessThanSignBang)
            {
                ON('-')
                {
                    SWITCH_TO_WITH_UNCLEAN_BUILDER(CommentLessThanSignBangDash);
                }
                ANYTHING_ELSE
                {
                    RECONSUME_IN(Comment);
                }
            }
            END_STATE

            // 13.2.5.48 Comment less-than sign bang dash state, https://html.spec.whatwg.org/multipage/parsing.html#comment-less-than-sign-bang-dash-state
            BEGIN_STATE(CommentLessThanSignBangDash)
            {
                ON('-')
                {
                    SWITCH_TO_WITH_UNCLEAN_BUILDER(CommentLessThanSignBangDashDash);
                }
                ANYTHING_ELSE
                {
                    RECONSUME_IN(CommentEndDash);
                }
            }
            END_STATE

            // 13.2.5.49 Comment less-than sign bang dash dash state, https://html.spec.whatwg.org/multipage/parsing.html#comment-less-than-sign-bang-dash-dash-state
            BEGIN_STATE(CommentLessThanSignBangDashDash)
            {
                ON('>')
                {
                    RECONSUME_IN(CommentEnd);
                }
                ON_EOF
                {
                    RECONSUME_IN(CommentEnd);
                }
                ANYTHING_ELSE
                {
                    log_parse_error();
                    RECONSUME_IN(CommentEnd);
                }
            }
            END_STATE

            // 13.2.5.72 Character reference state, https://html.spec.whatwg.org/multipage/parsing.html#character-reference-state
            BEGIN_STATE(CharacterReference)
            {
                m_temporary_buffer.clear();
                m_temporary_buffer.append('&');

                ON_ASCII_ALPHANUMERIC
                {
                    m_named_character_reference_matcher = {};
                    RECONSUME_IN(NamedCharacterReference);
                }
                ON('#')
                {
                    m_temporary_buffer.append(current_input_character.value());
                    SWITCH_TO_WITH_UNCLEAN_BUILDER(NumericCharacterReference);
                }
                ANYTHING_ELSE
                {
                    FLUSH_CODEPOINTS_CONSUMED_AS_A_CHARACTER_REFERENCE;
                    RECONSUME_IN_RETURN_STATE;
                }
            }
            END_STATE

            // 13.2.5.73 Named character reference state, https://html.spec.whatwg.org/multipage/parsing.html#named-character-reference-state
            BEGIN_STATE(NamedCharacterReference)
            {
                if (current_input_character.has_value()) {
                    if (m_named_character_reference_matcher.try_consume_code_point(current_input_character.value())) {
                        m_temporary_buffer.append(current_input_character.value());
                        continue;
                    } else {
                        DONT_CONSUME_NEXT_INPUT_CHARACTER;
                    }
                }

                // Only consume the characters within the longest match. It's possible that we've overconsumed code points,
                // though, so we want to backtrack to the longest match found. For example, `&notindo` (which could still
                // have lead to `&notindot;`) would need to backtrack back to `&not`),
                auto overconsumed_code_points = m_named_character_reference_matcher.overconsumed_code_points();
                if (overconsumed_code_points > 0) {
                    auto current_byte_offset = m_utf8_view.byte_offset_of(m_utf8_iterator);
                    // All consumed code points during character reference matching are guaranteed to be
                    // within the ASCII range, so they are always 1 byte wide.
                    restore_to(m_utf8_view.iterator_at_byte_offset_without_validation(current_byte_offset - overconsumed_code_points));
                    m_temporary_buffer.resize_and_keep_capacity(m_temporary_buffer.size() - overconsumed_code_points);
                }

                auto mapped_codepoints = m_named_character_reference_matcher.code_points();
                // If there is a match
                if (mapped_codepoints.has_value()) {
                    if (consumed_as_part_of_an_attribute() && !m_named_character_reference_matcher.last_match_ends_with_semicolon()) {
                        auto next_code_point = peek_code_point(0, stop_at_insertion_point);
                        if (next_code_point.has_value() && (next_code_point.value() == '=' || is_ascii_alphanumeric(next_code_point.value()))) {
                            FLUSH_CODEPOINTS_CONSUMED_AS_A_CHARACTER_REFERENCE;
                            SWITCH_TO_RETURN_STATE;
                        }
                    }

                    if (!m_named_character_reference_matcher.last_match_ends_with_semicolon()) {
                        log_parse_error();
                    }

                    m_temporary_buffer.clear_with_capacity();
                    m_temporary_buffer.append(mapped_codepoints.value().first);
                    auto second_codepoint = named_character_reference_second_codepoint_value(mapped_codepoints.value().second);
                    if (second_codepoint.has_value()) {
                        m_temporary_buffer.append(second_codepoint.value());
                    }

                    FLUSH_CODEPOINTS_CONSUMED_AS_A_CHARACTER_REFERENCE;
                    SWITCH_TO_RETURN_STATE;
                } else {
                    FLUSH_CODEPOINTS_CONSUMED_AS_A_CHARACTER_REFERENCE;
                    SWITCH_TO_WITH_UNCLEAN_BUILDER(AmbiguousAmpersand);
                }
            }
            END_STATE

            // 13.2.5.74 Ambiguous ampersand state, https://html.spec.whatwg.org/multipage/parsing.html#ambiguous-ampersand-state
            BEGIN_STATE(AmbiguousAmpersand)
            {
                ON_ASCII_ALPHANUMERIC
                {
                    if (consumed_as_part_of_an_attribute()) {
                        m_current_builder.append_code_point(current_input_character.value());
                        continue;
                    } else {
                        EMIT_CURRENT_CHARACTER;
                    }
                }
                ON(';')
                {
                    log_parse_error();
                    RECONSUME_IN_RETURN_STATE;
                }
                ANYTHING_ELSE
                {
                    RECONSUME_IN_RETURN_STATE;
                }
            }
            END_STATE

            // 13.2.5.75 Numeric character reference state, https://html.spec.whatwg.org/multipage/parsing.html#numeric-character-reference-state
            BEGIN_STATE(NumericCharacterReference)
            {
                m_character_reference_code = 0;

                ON('X')
                {
                    m_temporary_buffer.append(current_input_character.value());
                    SWITCH_TO_WITH_UNCLEAN_BUILDER(HexadecimalCharacterReferenceStart);
                }
                ON('x')
                {
                    m_temporary_buffer.append(current_input_character.value());
                    SWITCH_TO_WITH_UNCLEAN_BUILDER(HexadecimalCharacterReferenceStart);
                }
                ANYTHING_ELSE
                {
                    RECONSUME_IN(DecimalCharacterReferenceStart);
                }
            }
            END_STATE

            // 13.2.5.76 Hexadecimal character reference start state, https://html.spec.whatwg.org/multipage/parsing.html#hexadecimal-character-reference-start-state
            BEGIN_STATE(HexadecimalCharacterReferenceStart)
            {
                ON_ASCII_HEX_DIGIT
                {
                    RECONSUME_IN(HexadecimalCharacterReference);
                }
                ANYTHING_ELSE
                {
                    log_parse_error();
                    FLUSH_CODEPOINTS_CONSUMED_AS_A_CHARACTER_REFERENCE;
                    RECONSUME_IN_RETURN_STATE;
                }
            }
            END_STATE

            // 13.2.5.77 Decimal character reference start state, https://html.spec.whatwg.org/multipage/parsing.html#decimal-character-reference-start-state
            BEGIN_STATE(DecimalCharacterReferenceStart)
            {
                ON_ASCII_DIGIT
                {
                    RECONSUME_IN(DecimalCharacterReference);
                }
                ANYTHING_ELSE
                {
                    log_parse_error();
                    FLUSH_CODEPOINTS_CONSUMED_AS_A_CHARACTER_REFERENCE;
                    RECONSUME_IN_RETURN_STATE;
                }
            }
            END_STATE

            // 13.2.5.78 Hexadecimal character reference state, https://html.spec.whatwg.org/multipage/parsing.html#decimal-character-reference-start-state
            BEGIN_STATE(HexadecimalCharacterReference)
            {
                ON_ASCII_DIGIT
                {
                    m_character_reference_code *= 16;
                    m_character_reference_code += current_input_character.value() - 0x30;
                    continue;
                }
                ON_ASCII_HEX_DIGIT
                {
                    m_character_reference_code *= 16;
                    auto hex_digit_min_ascii_value = is_ascii_upper_alpha(current_input_character.value()) ? 0x37 : 0x57;
                    m_character_reference_code += current_input_character.value() - hex_digit_min_ascii_value;
                    continue;
                }
                ON(';')
                {
                    SWITCH_TO_WITH_UNCLEAN_BUILDER(NumericCharacterReferenceEnd);
                }
                ANYTHING_ELSE
                {
                    log_parse_error();
                    RECONSUME_IN(NumericCharacterReferenceEnd);
                }
            }
            END_STATE

            // 13.2.5.79 Decimal character reference state, https://html.spec.whatwg.org/multipage/parsing.html#decimal-character-reference-state
            BEGIN_STATE(DecimalCharacterReference)
            {
                ON_ASCII_DIGIT
                {
                    m_character_reference_code *= 10;
                    m_character_reference_code += current_input_character.value() - 0x30;
                    continue;
                }
                ON(';')
                {
                    SWITCH_TO_WITH_UNCLEAN_BUILDER(NumericCharacterReferenceEnd);
                }
                ANYTHING_ELSE
                {
                    log_parse_error();
                    RECONSUME_IN(NumericCharacterReferenceEnd);
                }
            }
            END_STATE

            // 13.2.5.80 Numeric character reference end state, https://html.spec.whatwg.org/multipage/parsing.html#numeric-character-reference-end-state
            BEGIN_STATE(NumericCharacterReferenceEnd)
            {
                DONT_CONSUME_NEXT_INPUT_CHARACTER;

                if (m_character_reference_code == 0) {
                    log_parse_error();
                    m_character_reference_code = 0xFFFD;
                }
                if (m_character_reference_code > 0x10ffff) {
                    log_parse_error();
                    m_character_reference_code = 0xFFFD;
                }
                if (is_unicode_surrogate(m_character_reference_code)) {
                    log_parse_error();
                    m_character_reference_code = 0xFFFD;
                }
                if (is_unicode_noncharacter(m_character_reference_code)) {
                    log_parse_error();
                }
                if (m_character_reference_code == 0xd || (is_unicode_control(m_character_reference_code) && !is_ascii_space(m_character_reference_code))) {
                    log_parse_error();
                    constexpr struct {
                        u32 number;
                        u32 code_point;
                    } conversion_table[] = {
                        { 0x80, 0x20AC },
                        { 0x82, 0x201A },
                        { 0x83, 0x0192 },
                        { 0x84, 0x201E },
                        { 0x85, 0x2026 },
                        { 0x86, 0x2020 },
                        { 0x87, 0x2021 },
                        { 0x88, 0x02C6 },
                        { 0x89, 0x2030 },
                        { 0x8A, 0x0160 },
                        { 0x8B, 0x2039 },
                        { 0x8C, 0x0152 },
                        { 0x8E, 0x017D },
                        { 0x91, 0x2018 },
                        { 0x92, 0x2019 },
                        { 0x93, 0x201C },
                        { 0x94, 0x201D },
                        { 0x95, 0x2022 },
                        { 0x96, 0x2013 },
                        { 0x97, 0x2014 },
                        { 0x98, 0x02DC },
                        { 0x99, 0x2122 },
                        { 0x9A, 0x0161 },
                        { 0x9B, 0x203A },
                        { 0x9C, 0x0153 },
                        { 0x9E, 0x017E },
                        { 0x9F, 0x0178 },
                    };
                    for (auto& entry : conversion_table) {
                        if (m_character_reference_code == entry.number) {
                            m_character_reference_code = entry.code_point;
                            break;
                        }
                    }
                }

                m_temporary_buffer.clear();
                m_temporary_buffer.append(m_character_reference_code);
                FLUSH_CODEPOINTS_CONSUMED_AS_A_CHARACTER_REFERENCE;
                SWITCH_TO_RETURN_STATE;
            }
            END_STATE

            // 13.2.5.2 RCDATA state, https://html.spec.whatwg.org/multipage/parsing.html#rcdata-state
            BEGIN_STATE(RCDATA)
            {
                ON('&')
                {
                    m_return_state = State::RCDATA;
                    SWITCH_TO(CharacterReference);
                }
                ON('<')
                {
                    SWITCH_TO(RCDATALessThanSign);
                }
                ON(0)
                {
                    log_parse_error();
                    EMIT_CHARACTER(0xFFFD);
                }
                ON_EOF
                {
                    EMIT_EOF;
                }
                ANYTHING_ELSE
                {
                    EMIT_CURRENT_CHARACTER;
                }
            }
            END_STATE

            // 13.2.5.9 RCDATA less-than sign state, https://html.spec.whatwg.org/multipage/parsing.html#rcdata-less-than-sign-state
            BEGIN_STATE(RCDATALessThanSign)
            {
                ON('/')
                {
                    m_temporary_buffer.clear();
                    SWITCH_TO(RCDATAEndTagOpen);
                }
                ANYTHING_ELSE
                {
                    EMIT_CHARACTER_AND_RECONSUME_IN('<', RCDATA);
                }
            }
            END_STATE

            // 13.2.5.10 RCDATA end tag open state, https://html.spec.whatwg.org/multipage/parsing.html#rcdata-end-tag-open-state
            BEGIN_STATE(RCDATAEndTagOpen)
            {
                ON_ASCII_ALPHA
                {
                    create_new_token(HTMLToken::Type::EndTag);
                    RECONSUME_IN(RCDATAEndTagName);
                }
                ANYTHING_ELSE
                {
                    m_queued_tokens.enqueue(HTMLToken::make_character('<'));
                    m_queued_tokens.enqueue(HTMLToken::make_character('/'));
                    RECONSUME_IN(RCDATA);
                }
            }
            END_STATE

            // 13.2.5.11 RCDATA end tag name state, https://html.spec.whatwg.org/multipage/parsing.html#rcdata-end-tag-name-state
            BEGIN_STATE(RCDATAEndTagName)
            {
                ON_WHITESPACE
                {
                    m_current_token.set_tag_name(consume_current_builder());
                    if (!current_end_tag_token_is_appropriate()) {
                        m_queued_tokens.enqueue(HTMLToken::make_character('<'));
                        m_queued_tokens.enqueue(HTMLToken::make_character('/'));
                        for (auto code_point : m_temporary_buffer)
                            m_queued_tokens.enqueue(HTMLToken::make_character(code_point));
                        RECONSUME_IN(RCDATA);
                    }
                    SWITCH_TO(BeforeAttributeName);
                }
                ON('/')
                {
                    m_current_token.set_tag_name(consume_current_builder());
                    if (!current_end_tag_token_is_appropriate()) {
                        m_queued_tokens.enqueue(HTMLToken::make_character('<'));
                        m_queued_tokens.enqueue(HTMLToken::make_character('/'));
                        for (auto code_point : m_temporary_buffer)
                            m_queued_tokens.enqueue(HTMLToken::make_character(code_point));
                        RECONSUME_IN(RCDATA);
                    }
                    SWITCH_TO(SelfClosingStartTag);
                }
                ON('>')
                {
                    m_current_token.set_tag_name(consume_current_builder());
                    if (!current_end_tag_token_is_appropriate()) {
                        m_queued_tokens.enqueue(HTMLToken::make_character('<'));
                        m_queued_tokens.enqueue(HTMLToken::make_character('/'));
                        for (auto code_point : m_temporary_buffer)
                            m_queued_tokens.enqueue(HTMLToken::make_character(code_point));
                        RECONSUME_IN(RCDATA);
                    }
                    SWITCH_TO_AND_EMIT_CURRENT_TOKEN(Data);
                }
                ON_ASCII_UPPER_ALPHA
                {
                    m_current_builder.append_code_point(to_ascii_lowercase(current_input_character.value()));
                    m_temporary_buffer.append(current_input_character.value());
                    continue;
                }
                ON_ASCII_LOWER_ALPHA
                {
                    m_current_builder.append_code_point(current_input_character.value());
                    m_temporary_buffer.append(current_input_character.value());
                    continue;
                }
                ANYTHING_ELSE
                {
                    m_queued_tokens.enqueue(HTMLToken::make_character('<'));
                    m_queued_tokens.enqueue(HTMLToken::make_character('/'));
                    // NOTE: The spec doesn't mention this, but it seems that m_current_token (an end tag) is just dropped in this case.
                    m_current_builder.clear();
                    for (auto code_point : m_temporary_buffer)
                        m_queued_tokens.enqueue(HTMLToken::make_character(code_point));
                    RECONSUME_IN(RCDATA);
                }
            }
            END_STATE

            // 13.2.5.3 RAWTEXT state, https://html.spec.whatwg.org/multipage/parsing.html#rawtext-state
            BEGIN_STATE(RAWTEXT)
            {
                ON('<')
                {
                    SWITCH_TO(RAWTEXTLessThanSign);
                }
                ON(0)
                {
                    log_parse_error();
                    EMIT_CHARACTER(0xFFFD);
                }
                ON_EOF
                {
                    EMIT_EOF;
                }
                ANYTHING_ELSE
                {
                    EMIT_CURRENT_CHARACTER;
                }
            }
            END_STATE

            // 13.2.5.12 RAWTEXT less-than sign state, https://html.spec.whatwg.org/multipage/parsing.html#rawtext-less-than-sign-state
            BEGIN_STATE(RAWTEXTLessThanSign)
            {
                ON('/')
                {
                    m_temporary_buffer.clear();
                    SWITCH_TO(RAWTEXTEndTagOpen);
                }
                ANYTHING_ELSE
                {
                    EMIT_CHARACTER_AND_RECONSUME_IN('<', RAWTEXT);
                }
            }
            END_STATE

            // 13.2.5.13 RAWTEXT end tag open state, https://html.spec.whatwg.org/multipage/parsing.html#rawtext-end-tag-open-state
            BEGIN_STATE(RAWTEXTEndTagOpen)
            {
                ON_ASCII_ALPHA
                {
                    create_new_token(HTMLToken::Type::EndTag);
                    RECONSUME_IN(RAWTEXTEndTagName);
                }
                ANYTHING_ELSE
                {
                    m_queued_tokens.enqueue(HTMLToken::make_character('<'));
                    m_queued_tokens.enqueue(HTMLToken::make_character('/'));
                    RECONSUME_IN(RAWTEXT);
                }
            }
            END_STATE

            // 13.2.5.14 RAWTEXT end tag name state, https://html.spec.whatwg.org/multipage/parsing.html#rawtext-end-tag-name-state
            BEGIN_STATE(RAWTEXTEndTagName)
            {
                ON_WHITESPACE
                {
                    m_current_token.set_tag_name(consume_current_builder());
                    if (!current_end_tag_token_is_appropriate()) {
                        m_queued_tokens.enqueue(HTMLToken::make_character('<'));
                        m_queued_tokens.enqueue(HTMLToken::make_character('/'));
                        for (auto code_point : m_temporary_buffer)
                            m_queued_tokens.enqueue(HTMLToken::make_character(code_point));
                        RECONSUME_IN(RAWTEXT);
                    }
                    SWITCH_TO(BeforeAttributeName);
                }
                ON('/')
                {
                    m_current_token.set_tag_name(consume_current_builder());
                    if (!current_end_tag_token_is_appropriate()) {
                        m_queued_tokens.enqueue(HTMLToken::make_character('<'));
                        m_queued_tokens.enqueue(HTMLToken::make_character('/'));
                        for (auto code_point : m_temporary_buffer)
                            m_queued_tokens.enqueue(HTMLToken::make_character(code_point));
                        RECONSUME_IN(RAWTEXT);
                    }
                    SWITCH_TO(SelfClosingStartTag);
                }
                ON('>')
                {
                    m_current_token.set_tag_name(consume_current_builder());
                    if (!current_end_tag_token_is_appropriate()) {
                        m_queued_tokens.enqueue(HTMLToken::make_character('<'));
                        m_queued_tokens.enqueue(HTMLToken::make_character('/'));
                        for (auto code_point : m_temporary_buffer)
                            m_queued_tokens.enqueue(HTMLToken::make_character(code_point));
                        RECONSUME_IN(RAWTEXT);
                    }
                    SWITCH_TO_AND_EMIT_CURRENT_TOKEN(Data);
                }
                ON_ASCII_UPPER_ALPHA
                {
                    m_current_builder.append_code_point(to_ascii_lowercase(current_input_character.value()));
                    m_temporary_buffer.append(current_input_character.value());
                    continue;
                }
                ON_ASCII_LOWER_ALPHA
                {
                    m_current_builder.append(current_input_character.value());
                    m_temporary_buffer.append(current_input_character.value());
                    continue;
                }
                ANYTHING_ELSE
                {
                    m_queued_tokens.enqueue(HTMLToken::make_character('<'));
                    m_queued_tokens.enqueue(HTMLToken::make_character('/'));
                    // NOTE: The spec doesn't mention this, but it seems that m_current_token (an end tag) is just dropped in this case.
                    m_current_builder.clear();
                    for (auto code_point : m_temporary_buffer)
                        m_queued_tokens.enqueue(HTMLToken::make_character(code_point));
                    RECONSUME_IN(RAWTEXT);
                }
            }
            END_STATE

            // 13.2.5.4 Script data state, https://html.spec.whatwg.org/multipage/parsing.html#script-data-state
            BEGIN_STATE(ScriptData)
            {
                ON('<')
                {
                    SWITCH_TO(ScriptDataLessThanSign);
                }
                ON(0)
                {
                    log_parse_error();
                    EMIT_CHARACTER(0xFFFD);
                }
                ON_EOF
                {
                    EMIT_EOF;
                }
                ANYTHING_ELSE
                {
                    EMIT_CURRENT_CHARACTER;
                }
            }
            END_STATE

            // 13.2.5.5 PLAINTEXT state, https://html.spec.whatwg.org/multipage/parsing.html#plaintext-state
            BEGIN_STATE(PLAINTEXT)
            {
                ON(0)
                {
                    log_parse_error();
                    EMIT_CHARACTER(0xFFFD);
                }
                ON_EOF
                {
                    EMIT_EOF;
                }
                ANYTHING_ELSE
                {
                    EMIT_CURRENT_CHARACTER;
                }
            }
            END_STATE

            // 13.2.5.15 Script data less-than sign state, https://html.spec.whatwg.org/multipage/parsing.html#script-data-less-than-sign-state
            BEGIN_STATE(ScriptDataLessThanSign)
            {
                ON('/')
                {
                    m_temporary_buffer.clear();
                    SWITCH_TO(ScriptDataEndTagOpen);
                }
                ON('!')
                {
                    m_queued_tokens.enqueue(HTMLToken::make_character('<'));
                    m_queued_tokens.enqueue(HTMLToken::make_character('!'));
                    SWITCH_TO(ScriptDataEscapeStart);
                }
                ANYTHING_ELSE
                {
                    EMIT_CHARACTER_AND_RECONSUME_IN('<', ScriptData);
                }
            }
            END_STATE

            // 13.2.5.18 Script data escape start state, https://html.spec.whatwg.org/multipage/parsing.html#script-data-escape-start-state
            BEGIN_STATE(ScriptDataEscapeStart)
            {
                ON('-')
                {
                    SWITCH_TO_AND_EMIT_CHARACTER('-', ScriptDataEscapeStartDash);
                }
                ANYTHING_ELSE
                {
                    RECONSUME_IN(ScriptData);
                }
            }
            END_STATE

            // 13.2.5.19 Script data escape start dash state, https://html.spec.whatwg.org/multipage/parsing.html#script-data-escape-start-dash-state
            BEGIN_STATE(ScriptDataEscapeStartDash)
            {
                ON('-')
                {
                    SWITCH_TO_AND_EMIT_CHARACTER('-', ScriptDataEscapedDashDash);
                }
                ANYTHING_ELSE
                {
                    RECONSUME_IN(ScriptData);
                }
            }
            END_STATE

            // 13.2.5.22 Script data escaped dash dash state, https://html.spec.whatwg.org/multipage/parsing.html#script-data-escaped-dash-dash-state
            BEGIN_STATE(ScriptDataEscapedDashDash)
            {
                ON('-')
                {
                    EMIT_CHARACTER('-');
                }
                ON('<')
                {
                    SWITCH_TO(ScriptDataEscapedLessThanSign);
                }
                ON('>')
                {
                    SWITCH_TO_AND_EMIT_CHARACTER('>', ScriptData);
                }
                ON(0)
                {
                    log_parse_error();
                    SWITCH_TO_AND_EMIT_CHARACTER(0xFFFD, ScriptDataEscaped);
                }
                ON_EOF
                {
                    log_parse_error();
                    EMIT_EOF;
                }
                ANYTHING_ELSE
                {
                    SWITCH_TO_AND_EMIT_CURRENT_CHARACTER(ScriptDataEscaped);
                }
            }
            END_STATE

            // 13.2.5.23 Script data escaped less-than sign state, https://html.spec.whatwg.org/multipage/parsing.html#script-data-escaped-less-than-sign-state
            BEGIN_STATE(ScriptDataEscapedLessThanSign)
            {
                ON('/')
                {
                    m_temporary_buffer.clear();
                    SWITCH_TO(ScriptDataEscapedEndTagOpen);
                }
                ON_ASCII_ALPHA
                {
                    m_temporary_buffer.clear();
                    EMIT_CHARACTER_AND_RECONSUME_IN('<', ScriptDataDoubleEscapeStart);
                }
                ANYTHING_ELSE
                {
                    EMIT_CHARACTER_AND_RECONSUME_IN('<', ScriptDataEscaped);
                }
            }
            END_STATE

            // 13.2.5.24 Script data escaped end tag open state, https://html.spec.whatwg.org/multipage/parsing.html#script-data-escaped-end-tag-open-state
            BEGIN_STATE(ScriptDataEscapedEndTagOpen)
            {
                ON_ASCII_ALPHA
                {
                    create_new_token(HTMLToken::Type::EndTag);
                    RECONSUME_IN(ScriptDataEscapedEndTagName);
                }
                ANYTHING_ELSE
                {
                    m_queued_tokens.enqueue(HTMLToken::make_character('<'));
                    m_queued_tokens.enqueue(HTMLToken::make_character('/'));
                    RECONSUME_IN(ScriptDataEscaped);
                }
            }
            END_STATE

            // 13.2.5.25 Script data escaped end tag name state, https://html.spec.whatwg.org/multipage/parsing.html#script-data-escaped-end-tag-name-state
            BEGIN_STATE(ScriptDataEscapedEndTagName)
            {
                ON_WHITESPACE
                {
                    m_current_token.set_tag_name(consume_current_builder());
                    if (current_end_tag_token_is_appropriate())
                        SWITCH_TO(BeforeAttributeName);

                    m_queued_tokens.enqueue(HTMLToken::make_character('<'));
                    m_queued_tokens.enqueue(HTMLToken::make_character('/'));
                    // NOTE: The spec doesn't mention this, but it seems that m_current_token (an end tag) is just dropped in this case.
                    m_current_builder.clear();
                    for (auto code_point : m_temporary_buffer) {
                        m_queued_tokens.enqueue(HTMLToken::make_character(code_point));
                    }
                    RECONSUME_IN(ScriptDataEscaped);
                }
                ON('/')
                {
                    m_current_token.set_tag_name(consume_current_builder());
                    if (current_end_tag_token_is_appropriate())
                        SWITCH_TO(SelfClosingStartTag);

                    m_queued_tokens.enqueue(HTMLToken::make_character('<'));
                    m_queued_tokens.enqueue(HTMLToken::make_character('/'));
                    // NOTE: The spec doesn't mention this, but it seems that m_current_token (an end tag) is just dropped in this case.
                    m_current_builder.clear();
                    for (auto code_point : m_temporary_buffer) {
                        m_queued_tokens.enqueue(HTMLToken::make_character(code_point));
                    }
                    RECONSUME_IN(ScriptDataEscaped);
                }
                ON('>')
                {
                    m_current_token.set_tag_name(consume_current_builder());
                    if (current_end_tag_token_is_appropriate())
                        SWITCH_TO_AND_EMIT_CURRENT_TOKEN(Data);

                    m_queued_tokens.enqueue(HTMLToken::make_character('<'));
                    m_queued_tokens.enqueue(HTMLToken::make_character('/'));
                    // NOTE: The spec doesn't mention this, but it seems that m_current_token (an end tag) is just dropped in this case.
                    m_current_builder.clear();
                    for (auto code_point : m_temporary_buffer) {
                        m_queued_tokens.enqueue(HTMLToken::make_character(code_point));
                    }
                    RECONSUME_IN(ScriptDataEscaped);
                }
                ON_ASCII_UPPER_ALPHA
                {
                    m_current_builder.append_code_point(to_ascii_lowercase(current_input_character.value()));
                    m_temporary_buffer.append(current_input_character.value());
                    continue;
                }
                ON_ASCII_LOWER_ALPHA
                {
                    m_current_builder.append(current_input_character.value());
                    m_temporary_buffer.append(current_input_character.value());
                    continue;
                }
                ANYTHING_ELSE
                {
                    m_queued_tokens.enqueue(HTMLToken::make_character('<'));
                    m_queued_tokens.enqueue(HTMLToken::make_character('/'));
                    // NOTE: The spec doesn't mention this, but it seems that m_current_token (an end tag) is just dropped in this case.
                    m_current_builder.clear();
                    for (auto code_point : m_temporary_buffer) {
                        m_queued_tokens.enqueue(HTMLToken::make_character(code_point));
                    }
                    RECONSUME_IN(ScriptDataEscaped);
                }
            }
            END_STATE

            // 13.2.5.26 Script data double escape start state, https://html.spec.whatwg.org/multipage/parsing.html#script-data-double-escape-start-state
            BEGIN_STATE(ScriptDataDoubleEscapeStart)
            {
                auto temporary_buffer_equal_to_script = [this]() -> bool {
                    if (m_temporary_buffer.size() != 6)
                        return false;

                    // FIXME: Is there a better way of doing this?
                    return m_temporary_buffer[0] == 's' && m_temporary_buffer[1] == 'c' && m_temporary_buffer[2] == 'r' && m_temporary_buffer[3] == 'i' && m_temporary_buffer[4] == 'p' && m_temporary_buffer[5] == 't';
                };
                ON_WHITESPACE
                {
                    if (temporary_buffer_equal_to_script())
                        SWITCH_TO_AND_EMIT_CURRENT_CHARACTER(ScriptDataDoubleEscaped);
                    else
                        SWITCH_TO_AND_EMIT_CURRENT_CHARACTER(ScriptDataEscaped);
                }
                ON('/')
                {
                    if (temporary_buffer_equal_to_script())
                        SWITCH_TO_AND_EMIT_CURRENT_CHARACTER(ScriptDataDoubleEscaped);
                    else
                        SWITCH_TO_AND_EMIT_CURRENT_CHARACTER(ScriptDataEscaped);
                }
                ON('>')
                {
                    if (temporary_buffer_equal_to_script())
                        SWITCH_TO_AND_EMIT_CURRENT_CHARACTER(ScriptDataDoubleEscaped);
                    else
                        SWITCH_TO_AND_EMIT_CURRENT_CHARACTER(ScriptDataEscaped);
                }
                ON_ASCII_UPPER_ALPHA
                {
                    m_temporary_buffer.append(to_ascii_lowercase(current_input_character.value()));
                    EMIT_CURRENT_CHARACTER;
                }
                ON_ASCII_LOWER_ALPHA
                {
                    m_temporary_buffer.append(current_input_character.value());
                    EMIT_CURRENT_CHARACTER;
                }
                ANYTHING_ELSE
                {
                    RECONSUME_IN(ScriptDataEscaped);
                }
            }
            END_STATE

            // 13.2.5.27 Script data double escaped state, https://html.spec.whatwg.org/multipage/parsing.html#script-data-double-escaped-state
            BEGIN_STATE(ScriptDataDoubleEscaped)
            {
                ON('-')
                {
                    SWITCH_TO_AND_EMIT_CHARACTER('-', ScriptDataDoubleEscapedDash);
                }
                ON('<')
                {
                    SWITCH_TO_AND_EMIT_CHARACTER('<', ScriptDataDoubleEscapedLessThanSign);
                }
                ON(0)
                {
                    log_parse_error();
                    EMIT_CHARACTER(0xFFFD);
                }
                ON_EOF
                {
                    log_parse_error();
                    EMIT_EOF;
                }
                ANYTHING_ELSE
                {
                    EMIT_CURRENT_CHARACTER;
                }
            }
            END_STATE

            // 13.2.5.28 Script data double escaped dash state, https://html.spec.whatwg.org/multipage/parsing.html#script-data-double-escaped-dash-state
            BEGIN_STATE(ScriptDataDoubleEscapedDash)
            {
                ON('-')
                {
                    SWITCH_TO_AND_EMIT_CHARACTER('-', ScriptDataDoubleEscapedDashDash);
                }
                ON('<')
                {
                    SWITCH_TO_AND_EMIT_CHARACTER('<', ScriptDataDoubleEscapedLessThanSign);
                }
                ON(0)
                {
                    log_parse_error();
                    SWITCH_TO_AND_EMIT_CHARACTER(0xFFFD, ScriptDataDoubleEscaped);
                }
                ON_EOF
                {
                    log_parse_error();
                    EMIT_EOF;
                }
                ANYTHING_ELSE
                {
                    SWITCH_TO_AND_EMIT_CURRENT_CHARACTER(ScriptDataDoubleEscaped);
                }
            }
            END_STATE

            // 13.2.5.29 Script data double escaped dash dash state, https://html.spec.whatwg.org/multipage/parsing.html#script-data-double-escaped-dash-dash-state
            BEGIN_STATE(ScriptDataDoubleEscapedDashDash)
            {
                ON('-')
                {
                    EMIT_CHARACTER('-');
                }
                ON('<')
                {
                    SWITCH_TO_AND_EMIT_CHARACTER('<', ScriptDataDoubleEscapedLessThanSign);
                }
                ON('>')
                {
                    SWITCH_TO_AND_EMIT_CHARACTER('>', ScriptData);
                }
                ON(0)
                {
                    log_parse_error();
                    SWITCH_TO_AND_EMIT_CHARACTER(0xFFFD, ScriptDataDoubleEscaped);
                }
                ON_EOF
                {
                    log_parse_error();
                    EMIT_EOF;
                }
                ANYTHING_ELSE
                {
                    SWITCH_TO_AND_EMIT_CURRENT_CHARACTER(ScriptDataDoubleEscaped);
                }
            }
            END_STATE

            // 13.2.5.30 Script data double escaped less-than sign state, https://html.spec.whatwg.org/multipage/parsing.html#script-data-double-escaped-less-than-sign-state
            BEGIN_STATE(ScriptDataDoubleEscapedLessThanSign)
            {
                ON('/')
                {
                    m_temporary_buffer.clear();
                    SWITCH_TO_AND_EMIT_CHARACTER('/', ScriptDataDoubleEscapeEnd);
                }
                ANYTHING_ELSE
                {
                    RECONSUME_IN(ScriptDataDoubleEscaped);
                }
            }
            END_STATE

            // 13.2.5.31 Script data double escape end state, https://html.spec.whatwg.org/multipage/parsing.html#script-data-double-escape-end-state
            BEGIN_STATE(ScriptDataDoubleEscapeEnd)
            {
                auto temporary_buffer_equal_to_script = [this]() -> bool {
                    if (m_temporary_buffer.size() != 6)
                        return false;

                    // FIXME: Is there a better way of doing this?
                    return m_temporary_buffer[0] == 's' && m_temporary_buffer[1] == 'c' && m_temporary_buffer[2] == 'r' && m_temporary_buffer[3] == 'i' && m_temporary_buffer[4] == 'p' && m_temporary_buffer[5] == 't';
                };
                ON_WHITESPACE
                {
                    if (temporary_buffer_equal_to_script())
                        SWITCH_TO_AND_EMIT_CURRENT_CHARACTER(ScriptDataEscaped);
                    else
                        SWITCH_TO_AND_EMIT_CURRENT_CHARACTER(ScriptDataDoubleEscaped);
                }
                ON('/')
                {
                    if (temporary_buffer_equal_to_script())
                        SWITCH_TO_AND_EMIT_CURRENT_CHARACTER(ScriptDataEscaped);
                    else
                        SWITCH_TO_AND_EMIT_CURRENT_CHARACTER(ScriptDataDoubleEscaped);
                }
                ON('>')
                {
                    if (temporary_buffer_equal_to_script())
                        SWITCH_TO_AND_EMIT_CURRENT_CHARACTER(ScriptDataEscaped);
                    else
                        SWITCH_TO_AND_EMIT_CURRENT_CHARACTER(ScriptDataDoubleEscaped);
                }
                ON_ASCII_UPPER_ALPHA
                {
                    m_temporary_buffer.append(to_ascii_lowercase(current_input_character.value()));
                    EMIT_CURRENT_CHARACTER;
                }
                ON_ASCII_LOWER_ALPHA
                {
                    m_temporary_buffer.append(current_input_character.value());
                    EMIT_CURRENT_CHARACTER;
                }
                ANYTHING_ELSE
                {
                    RECONSUME_IN(ScriptDataDoubleEscaped);
                }
            }
            END_STATE

            // 13.2.5.21 Script data escaped dash state, https://html.spec.whatwg.org/multipage/parsing.html#script-data-escaped-dash-state
            BEGIN_STATE(ScriptDataEscapedDash)
            {
                ON('-')
                {
                    SWITCH_TO_AND_EMIT_CHARACTER('-', ScriptDataEscapedDashDash);
                }
                ON('<')
                {
                    SWITCH_TO(ScriptDataEscapedLessThanSign);
                }
                ON(0)
                {
                    log_parse_error();
                    SWITCH_TO_AND_EMIT_CHARACTER(0xFFFD, ScriptDataEscaped);
                }
                ON_EOF
                {
                    log_parse_error();
                    EMIT_EOF;
                }
                ANYTHING_ELSE
                {
                    SWITCH_TO_AND_EMIT_CURRENT_CHARACTER(ScriptDataEscaped);
                }
            }
            END_STATE

            // 13.2.5.20 Script data escaped state, https://html.spec.whatwg.org/multipage/parsing.html#script-data-escaped-state
            BEGIN_STATE(ScriptDataEscaped)
            {
                ON('-')
                {
                    SWITCH_TO_AND_EMIT_CHARACTER('-', ScriptDataEscapedDash);
                }
                ON('<')
                {
                    SWITCH_TO(ScriptDataEscapedLessThanSign);
                }
                ON(0)
                {
                    log_parse_error();
                    EMIT_CHARACTER(0xFFFD);
                }
                ON_EOF
                {
                    log_parse_error();
                    EMIT_EOF;
                }
                ANYTHING_ELSE
                {
                    EMIT_CURRENT_CHARACTER;
                }
            }
            END_STATE

            // 13.2.5.16 Script data end tag open state, https://html.spec.whatwg.org/multipage/parsing.html#script-data-end-tag-open-state
            BEGIN_STATE(ScriptDataEndTagOpen)
            {
                ON_ASCII_ALPHA
                {
                    create_new_token(HTMLToken::Type::EndTag);
                    RECONSUME_IN(ScriptDataEndTagName);
                }
                ANYTHING_ELSE
                {
                    m_queued_tokens.enqueue(HTMLToken::make_character('<'));
                    m_queued_tokens.enqueue(HTMLToken::make_character('/'));
                    RECONSUME_IN(ScriptData);
                }
            }
            END_STATE

            // 13.2.5.17 Script data end tag name state, https://html.spec.whatwg.org/multipage/parsing.html#script-data-end-tag-name-state
            BEGIN_STATE(ScriptDataEndTagName)
            {
                ON_WHITESPACE
                {
                    m_current_token.set_tag_name(consume_current_builder());
                    if (current_end_tag_token_is_appropriate())
                        SWITCH_TO(BeforeAttributeName);
                    m_queued_tokens.enqueue(HTMLToken::make_character('<'));
                    m_queued_tokens.enqueue(HTMLToken::make_character('/'));
                    // NOTE: The spec doesn't mention this, but it seems that m_current_token (an end tag) is just dropped in this case.
                    m_current_builder.clear();
                    for (auto code_point : m_temporary_buffer)
                        m_queued_tokens.enqueue(HTMLToken::make_character(code_point));
                    RECONSUME_IN(ScriptData);
                }
                ON('/')
                {
                    m_current_token.set_tag_name(consume_current_builder());
                    if (current_end_tag_token_is_appropriate())
                        SWITCH_TO(SelfClosingStartTag);
                    m_queued_tokens.enqueue(HTMLToken::make_character('<'));
                    m_queued_tokens.enqueue(HTMLToken::make_character('/'));
                    // NOTE: The spec doesn't mention this, but it seems that m_current_token (an end tag) is just dropped in this case.
                    m_current_builder.clear();
                    for (auto code_point : m_temporary_buffer)
                        m_queued_tokens.enqueue(HTMLToken::make_character(code_point));
                    RECONSUME_IN(ScriptData);
                }
                ON('>')
                {
                    m_current_token.set_tag_name(consume_current_builder());
                    if (current_end_tag_token_is_appropriate())
                        SWITCH_TO_AND_EMIT_CURRENT_TOKEN(Data);
                    m_queued_tokens.enqueue(HTMLToken::make_character('<'));
                    m_queued_tokens.enqueue(HTMLToken::make_character('/'));
                    // NOTE: The spec doesn't mention this, but it seems that m_current_token (an end tag) is just dropped in this case.
                    m_current_builder.clear();
                    for (auto code_point : m_temporary_buffer)
                        m_queued_tokens.enqueue(HTMLToken::make_character(code_point));
                    RECONSUME_IN(ScriptData);
                }
                ON_ASCII_UPPER_ALPHA
                {
                    m_current_builder.append_code_point(to_ascii_lowercase(current_input_character.value()));
                    m_temporary_buffer.append(current_input_character.value());
                    continue;
                }
                ON_ASCII_LOWER_ALPHA
                {
                    m_current_builder.append(current_input_character.value());
                    m_temporary_buffer.append(current_input_character.value());
                    continue;
                }
                ANYTHING_ELSE
                {
                    m_queued_tokens.enqueue(HTMLToken::make_character('<'));
                    m_queued_tokens.enqueue(HTMLToken::make_character('/'));
                    // NOTE: The spec doesn't mention this, but it seems that m_current_token (an end tag) is just dropped in this case.
                    m_current_builder.clear();
                    for (auto code_point : m_temporary_buffer)
                        m_queued_tokens.enqueue(HTMLToken::make_character(code_point));
                    RECONSUME_IN(ScriptData);
                }
            }
            END_STATE

            // 13.2.5.69 CDATA section state, https://html.spec.whatwg.org/multipage/parsing.html#cdata-section-state
            BEGIN_STATE(CDATASection)
            {
                ON(']')
                {
                    SWITCH_TO(CDATASectionBracket);
                }
                ON_EOF
                {
                    log_parse_error();
                    EMIT_EOF;
                }
                ANYTHING_ELSE
                {
                    EMIT_CURRENT_CHARACTER;
                }
            }
            END_STATE

            // 13.2.5.70 CDATA section bracket state, https://html.spec.whatwg.org/multipage/parsing.html#cdata-section-bracket-state
            BEGIN_STATE(CDATASectionBracket)
            {
                ON(']')
                {
                    SWITCH_TO(CDATASectionEnd);
                }
                ANYTHING_ELSE
                {
                    EMIT_CHARACTER_AND_RECONSUME_IN(']', CDATASection);
                }
            }
            END_STATE

            // 13.2.5.71 CDATA section end state, https://html.spec.whatwg.org/multipage/parsing.html#cdata-section-end-state
            BEGIN_STATE(CDATASectionEnd)
            {
                ON(']')
                {
                    EMIT_CHARACTER(']');
                }
                ON('>')
                {
                    SWITCH_TO(Data);
                }
                ANYTHING_ELSE
                {
                    m_queued_tokens.enqueue(HTMLToken::make_character(']'));
                    m_queued_tokens.enqueue(HTMLToken::make_character(']'));
                    RECONSUME_IN(CDATASection);
                }
            }
            END_STATE

        default:
            TODO();
        }
    }
}

HTMLTokenizer::ConsumeNextResult HTMLTokenizer::consume_next_if_match(StringView string, StopAtInsertionPoint stop_at_insertion_point, CaseSensitivity case_sensitivity)
{
    for (size_t i = 0; i < string.length(); ++i) {
        auto code_point = peek_code_point(i, stop_at_insertion_point);
        if (!code_point.has_value()) {
            if (StopAtInsertionPoint::Yes == stop_at_insertion_point) {
                return ConsumeNextResult::RanOutOfCharacters;
            }
            return ConsumeNextResult::NotConsumed;
        }
        // FIXME: This should be more Unicode-aware.
        if (case_sensitivity == CaseSensitivity::CaseInsensitive) {
            if (code_point.value() < 0x80) {
                if (to_ascii_lowercase(code_point.value()) != to_ascii_lowercase(string[i]))
                    return ConsumeNextResult::NotConsumed;
                continue;
            }
        }
        if (code_point.value() != (u32)string[i])
            return ConsumeNextResult::NotConsumed;
    }
    skip(string.length());
    return ConsumeNextResult::Consumed;
}

void HTMLTokenizer::create_new_token(HTMLToken::Type type)
{
    m_current_token = { type };

    auto is_start_or_end_tag = type == HTMLToken::Type::StartTag || type == HTMLToken::Type::EndTag;
    m_current_token.set_start_position({}, nth_last_position(is_start_or_end_tag ? 1 : 0));
}

HTMLTokenizer::HTMLTokenizer()
{
    m_decoded_input = "";
    m_utf8_view = Utf8View(m_decoded_input);
    m_utf8_iterator = m_utf8_view.begin();
    m_prev_utf8_iterator = m_utf8_view.begin();
    m_source_positions.empend(0u, 0u);
}

HTMLTokenizer::HTMLTokenizer(StringView input, ByteString const& encoding)
{
    auto decoder = TextCodec::decoder_for(encoding);
    VERIFY(decoder.has_value());
    m_decoded_input = decoder->to_utf8(input).release_value_but_fixme_should_propagate_errors().to_byte_string();
    m_utf8_view = Utf8View(m_decoded_input);
    m_utf8_iterator = m_utf8_view.begin();
    m_prev_utf8_iterator = m_utf8_view.begin();
    m_source_positions.empend(0u, 0u);
}

void HTMLTokenizer::insert_input_at_insertion_point(StringView input)
{
    auto utf8_iterator_byte_offset = m_utf8_view.byte_offset_of(m_utf8_iterator);
    auto prev_utf8_iterator_byte_offset = m_utf8_view.byte_offset_of(m_prev_utf8_iterator);

    // FIXME: Implement a InputStream to handle insertion_point and iterators.
    StringBuilder builder {};
    builder.append(m_decoded_input.substring_view(0, m_insertion_point.position));
    builder.append(input);
    builder.append(m_decoded_input.substring_view(m_insertion_point.position));
    m_decoded_input = builder.to_byte_string();

    m_utf8_view = Utf8View(m_decoded_input);
    m_utf8_iterator = m_utf8_view.iterator_at_byte_offset(utf8_iterator_byte_offset);
    m_prev_utf8_iterator = m_utf8_view.iterator_at_byte_offset(prev_utf8_iterator_byte_offset);

    m_insertion_point.position += input.length();
}

void HTMLTokenizer::insert_eof()
{
    m_explicit_eof_inserted = true;
}

bool HTMLTokenizer::is_eof_inserted()
{
    return m_explicit_eof_inserted;
}

void HTMLTokenizer::will_switch_to([[maybe_unused]] State new_state)
{
    dbgln_if(TOKENIZER_TRACE_DEBUG, "[{}] Switch to {}", state_name(m_state), state_name(new_state));
}

void HTMLTokenizer::will_reconsume_in([[maybe_unused]] State new_state)
{
    dbgln_if(TOKENIZER_TRACE_DEBUG, "[{}] Reconsume in {}", state_name(m_state), state_name(new_state));
}

void HTMLTokenizer::switch_to(Badge<HTMLParser>, State new_state)
{
    dbgln_if(TOKENIZER_TRACE_DEBUG, "[{}] Parser switches tokenizer state to {}", state_name(m_state), state_name(new_state));
    m_state = new_state;
}

void HTMLTokenizer::will_emit(HTMLToken& token)
{
    if (token.is_start_tag())
        m_last_emitted_start_tag_name = token.tag_name();

    auto is_start_or_end_tag = token.type() == HTMLToken::Type::StartTag || token.type() == HTMLToken::Type::EndTag;
    token.set_end_position({}, nth_last_position(is_start_or_end_tag ? 1 : 0));

    if (is_start_or_end_tag)
        token.normalize_attributes();
}

bool HTMLTokenizer::current_end_tag_token_is_appropriate() const
{
    VERIFY(m_current_token.is_end_tag());
    if (!m_last_emitted_start_tag_name.has_value())
        return false;
    return m_current_token.tag_name() == m_last_emitted_start_tag_name.value();
}

bool HTMLTokenizer::consumed_as_part_of_an_attribute() const
{
    return m_return_state == State::AttributeValueUnquoted || m_return_state == State::AttributeValueSingleQuoted || m_return_state == State::AttributeValueDoubleQuoted;
}

void HTMLTokenizer::restore_to(Utf8CodePointIterator const& new_iterator)
{
    auto diff = m_utf8_iterator - new_iterator;
    if (diff > 0) {
        for (ssize_t i = 0; i < diff; ++i) {
            if (!m_source_positions.is_empty())
                m_source_positions.take_last();
        }
    } else {
        // Going forwards...?
        TODO();
    }
    m_utf8_iterator = new_iterator;
}

String HTMLTokenizer::consume_current_builder()
{
    auto string = m_current_builder.to_string_without_validation();
    m_current_builder.clear();
    return string;
}

}
