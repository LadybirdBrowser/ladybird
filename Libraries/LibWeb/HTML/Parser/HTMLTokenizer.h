/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Queue.h>
#include <AK/StringBuilder.h>
#include <AK/StringView.h>
#include <AK/Types.h>
#include <AK/Utf8View.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/Parser/Entities.h>
#include <LibWeb/HTML/Parser/HTMLToken.h>

namespace Web::HTML {

#define ENUMERATE_TOKENIZER_STATES                                        \
    __ENUMERATE_TOKENIZER_STATE(Data)                                     \
    __ENUMERATE_TOKENIZER_STATE(RCDATA)                                   \
    __ENUMERATE_TOKENIZER_STATE(RAWTEXT)                                  \
    __ENUMERATE_TOKENIZER_STATE(ScriptData)                               \
    __ENUMERATE_TOKENIZER_STATE(PLAINTEXT)                                \
    __ENUMERATE_TOKENIZER_STATE(TagOpen)                                  \
    __ENUMERATE_TOKENIZER_STATE(EndTagOpen)                               \
    __ENUMERATE_TOKENIZER_STATE(TagName)                                  \
    __ENUMERATE_TOKENIZER_STATE(RCDATALessThanSign)                       \
    __ENUMERATE_TOKENIZER_STATE(RCDATAEndTagOpen)                         \
    __ENUMERATE_TOKENIZER_STATE(RCDATAEndTagName)                         \
    __ENUMERATE_TOKENIZER_STATE(RAWTEXTLessThanSign)                      \
    __ENUMERATE_TOKENIZER_STATE(RAWTEXTEndTagOpen)                        \
    __ENUMERATE_TOKENIZER_STATE(RAWTEXTEndTagName)                        \
    __ENUMERATE_TOKENIZER_STATE(ScriptDataLessThanSign)                   \
    __ENUMERATE_TOKENIZER_STATE(ScriptDataEndTagOpen)                     \
    __ENUMERATE_TOKENIZER_STATE(ScriptDataEndTagName)                     \
    __ENUMERATE_TOKENIZER_STATE(ScriptDataEscapeStart)                    \
    __ENUMERATE_TOKENIZER_STATE(ScriptDataEscapeStartDash)                \
    __ENUMERATE_TOKENIZER_STATE(ScriptDataEscaped)                        \
    __ENUMERATE_TOKENIZER_STATE(ScriptDataEscapedDash)                    \
    __ENUMERATE_TOKENIZER_STATE(ScriptDataEscapedDashDash)                \
    __ENUMERATE_TOKENIZER_STATE(ScriptDataEscapedLessThanSign)            \
    __ENUMERATE_TOKENIZER_STATE(ScriptDataEscapedEndTagOpen)              \
    __ENUMERATE_TOKENIZER_STATE(ScriptDataEscapedEndTagName)              \
    __ENUMERATE_TOKENIZER_STATE(ScriptDataDoubleEscapeStart)              \
    __ENUMERATE_TOKENIZER_STATE(ScriptDataDoubleEscaped)                  \
    __ENUMERATE_TOKENIZER_STATE(ScriptDataDoubleEscapedDash)              \
    __ENUMERATE_TOKENIZER_STATE(ScriptDataDoubleEscapedDashDash)          \
    __ENUMERATE_TOKENIZER_STATE(ScriptDataDoubleEscapedLessThanSign)      \
    __ENUMERATE_TOKENIZER_STATE(ScriptDataDoubleEscapeEnd)                \
    __ENUMERATE_TOKENIZER_STATE(BeforeAttributeName)                      \
    __ENUMERATE_TOKENIZER_STATE(AttributeName)                            \
    __ENUMERATE_TOKENIZER_STATE(AfterAttributeName)                       \
    __ENUMERATE_TOKENIZER_STATE(BeforeAttributeValue)                     \
    __ENUMERATE_TOKENIZER_STATE(AttributeValueDoubleQuoted)               \
    __ENUMERATE_TOKENIZER_STATE(AttributeValueSingleQuoted)               \
    __ENUMERATE_TOKENIZER_STATE(AttributeValueUnquoted)                   \
    __ENUMERATE_TOKENIZER_STATE(AfterAttributeValueQuoted)                \
    __ENUMERATE_TOKENIZER_STATE(SelfClosingStartTag)                      \
    __ENUMERATE_TOKENIZER_STATE(BogusComment)                             \
    __ENUMERATE_TOKENIZER_STATE(MarkupDeclarationOpen)                    \
    __ENUMERATE_TOKENIZER_STATE(CommentStart)                             \
    __ENUMERATE_TOKENIZER_STATE(CommentStartDash)                         \
    __ENUMERATE_TOKENIZER_STATE(Comment)                                  \
    __ENUMERATE_TOKENIZER_STATE(CommentLessThanSign)                      \
    __ENUMERATE_TOKENIZER_STATE(CommentLessThanSignBang)                  \
    __ENUMERATE_TOKENIZER_STATE(CommentLessThanSignBangDash)              \
    __ENUMERATE_TOKENIZER_STATE(CommentLessThanSignBangDashDash)          \
    __ENUMERATE_TOKENIZER_STATE(CommentEndDash)                           \
    __ENUMERATE_TOKENIZER_STATE(CommentEnd)                               \
    __ENUMERATE_TOKENIZER_STATE(CommentEndBang)                           \
    __ENUMERATE_TOKENIZER_STATE(DOCTYPE)                                  \
    __ENUMERATE_TOKENIZER_STATE(BeforeDOCTYPEName)                        \
    __ENUMERATE_TOKENIZER_STATE(DOCTYPEName)                              \
    __ENUMERATE_TOKENIZER_STATE(AfterDOCTYPEName)                         \
    __ENUMERATE_TOKENIZER_STATE(AfterDOCTYPEPublicKeyword)                \
    __ENUMERATE_TOKENIZER_STATE(BeforeDOCTYPEPublicIdentifier)            \
    __ENUMERATE_TOKENIZER_STATE(DOCTYPEPublicIdentifierDoubleQuoted)      \
    __ENUMERATE_TOKENIZER_STATE(DOCTYPEPublicIdentifierSingleQuoted)      \
    __ENUMERATE_TOKENIZER_STATE(AfterDOCTYPEPublicIdentifier)             \
    __ENUMERATE_TOKENIZER_STATE(BetweenDOCTYPEPublicAndSystemIdentifiers) \
    __ENUMERATE_TOKENIZER_STATE(AfterDOCTYPESystemKeyword)                \
    __ENUMERATE_TOKENIZER_STATE(BeforeDOCTYPESystemIdentifier)            \
    __ENUMERATE_TOKENIZER_STATE(DOCTYPESystemIdentifierDoubleQuoted)      \
    __ENUMERATE_TOKENIZER_STATE(DOCTYPESystemIdentifierSingleQuoted)      \
    __ENUMERATE_TOKENIZER_STATE(AfterDOCTYPESystemIdentifier)             \
    __ENUMERATE_TOKENIZER_STATE(BogusDOCTYPE)                             \
    __ENUMERATE_TOKENIZER_STATE(CDATASection)                             \
    __ENUMERATE_TOKENIZER_STATE(CDATASectionBracket)                      \
    __ENUMERATE_TOKENIZER_STATE(CDATASectionEnd)                          \
    __ENUMERATE_TOKENIZER_STATE(CharacterReference)                       \
    __ENUMERATE_TOKENIZER_STATE(NamedCharacterReference)                  \
    __ENUMERATE_TOKENIZER_STATE(AmbiguousAmpersand)                       \
    __ENUMERATE_TOKENIZER_STATE(NumericCharacterReference)                \
    __ENUMERATE_TOKENIZER_STATE(HexadecimalCharacterReferenceStart)       \
    __ENUMERATE_TOKENIZER_STATE(DecimalCharacterReferenceStart)           \
    __ENUMERATE_TOKENIZER_STATE(HexadecimalCharacterReference)            \
    __ENUMERATE_TOKENIZER_STATE(DecimalCharacterReference)                \
    __ENUMERATE_TOKENIZER_STATE(NumericCharacterReferenceEnd)

class HTMLTokenizer {
public:
    explicit HTMLTokenizer();
    explicit HTMLTokenizer(StringView input, ByteString const& encoding);

    enum class State {
#define __ENUMERATE_TOKENIZER_STATE(state) state,
        ENUMERATE_TOKENIZER_STATES
#undef __ENUMERATE_TOKENIZER_STATE
    };

    enum class StopAtInsertionPoint {
        No,
        Yes,
    };
    Optional<HTMLToken> next_token(StopAtInsertionPoint = StopAtInsertionPoint::No);

    void set_parser(Badge<HTMLParser>, HTMLParser& parser) { m_parser = &parser; }

    void switch_to(Badge<HTMLParser>, State new_state);
    void switch_to(State new_state)
    {
        m_state = new_state;
    }

    void set_blocked(bool b) { m_blocked = b; }
    bool is_blocked() const { return m_blocked; }

    ByteString source() const { return m_decoded_input; }

    void insert_input_at_insertion_point(StringView input);
    void insert_eof();
    bool is_eof_inserted();

    bool is_insertion_point_defined() const { return m_insertion_point.defined; }
    bool is_insertion_point_reached()
    {
        return m_insertion_point.defined && m_utf8_view.iterator_offset(m_utf8_iterator) >= m_insertion_point.position;
    }
    void undefine_insertion_point() { m_insertion_point.defined = false; }
    void store_insertion_point() { m_old_insertion_point = m_insertion_point; }
    void restore_insertion_point() { m_insertion_point = m_old_insertion_point; }
    void update_insertion_point()
    {
        m_insertion_point.defined = true;
        m_insertion_point.position = m_utf8_view.iterator_offset(m_utf8_iterator);
    }

    // This permanently cuts off the tokenizer input stream.
    void abort() { m_aborted = true; }

private:
    void skip(size_t count);
    Optional<u32> next_code_point(StopAtInsertionPoint);
    Optional<u32> peek_code_point(size_t offset, StopAtInsertionPoint) const;

    enum class ConsumeNextResult {
        Consumed,
        NotConsumed,
        RanOutOfCharacters,
    };
    [[nodiscard]] ConsumeNextResult consume_next_if_match(StringView, StopAtInsertionPoint, CaseSensitivity = CaseSensitivity::CaseSensitive);

    void create_new_token(HTMLToken::Type);
    bool current_end_tag_token_is_appropriate() const;
    String consume_current_builder();

    static char const* state_name(State state)
    {
        switch (state) {
#define __ENUMERATE_TOKENIZER_STATE(state) \
    case State::state:                     \
        return #state;
            ENUMERATE_TOKENIZER_STATES
#undef __ENUMERATE_TOKENIZER_STATE
        };
        VERIFY_NOT_REACHED();
    }

    void will_emit(HTMLToken&);
    void will_switch_to(State);
    void will_reconsume_in(State);

    bool consumed_as_part_of_an_attribute() const;

    void restore_to(Utf8CodePointIterator const& new_iterator);
    HTMLToken::Position nth_last_position(size_t n = 0);

    GC::Ptr<HTMLParser> m_parser;

    State m_state { State::Data };
    State m_return_state { State::Data };

    Vector<u32> m_temporary_buffer;

    ByteString m_decoded_input;

    struct InsertionPoint {
        size_t position { 0 };
        bool defined { false };
    };
    InsertionPoint m_insertion_point {};
    InsertionPoint m_old_insertion_point {};

    Utf8View m_utf8_view;
    Utf8CodePointIterator m_utf8_iterator;
    Utf8CodePointIterator m_prev_utf8_iterator;

    HTMLToken m_current_token;
    StringBuilder m_current_builder;

    NamedCharacterReferenceMatcher m_named_character_reference_matcher;

    Optional<FlyString> m_last_emitted_start_tag_name;

    bool m_explicit_eof_inserted { false };
    bool m_has_emitted_eof { false };

    Queue<HTMLToken> m_queued_tokens;

    u32 m_character_reference_code { 0 };

    bool m_blocked { false };

    bool m_aborted { false };

    Vector<HTMLToken::Position> m_source_positions;
};

}
