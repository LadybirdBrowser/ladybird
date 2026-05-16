/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Types.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/Parser/HTMLToken.h>

struct RustFfiTokenizerHandle;

namespace Web::HTML {

class HTMLParser;

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

class WEB_API HTMLTokenizer {
public:
    explicit HTMLTokenizer();
    explicit HTMLTokenizer(StringView input, ByteString const& encoding);
    ~HTMLTokenizer();

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

    void switch_to(State new_state);

    auto const& source() const { return m_source; }

    String unparsed_input() const;

    void append_to_input_stream(StringView input);
    void close_input_stream();
    bool is_input_stream_closed() const { return m_input_stream_closed; }
    void insert_input_at_insertion_point(StringView input);
    void insert_eof();

    bool is_insertion_point_defined() const;
    bool is_insertion_point_reached();
    void undefine_insertion_point();
    void store_insertion_point();
    void restore_insertion_point();
    void store_old_insertion_point() { store_insertion_point(); }
    void restore_old_insertion_point() { restore_insertion_point(); }
    void update_insertion_point();

    // This permanently cuts off the tokenizer input stream.
    void abort();

    void parser_did_run(Badge<HTMLParser>);
    RustFfiTokenizerHandle* ffi_handle(Badge<HTMLParser>) { return m_tokenizer; }

private:
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

    State m_state { State::Data };
    String m_source;
    bool m_input_stream_closed { false };

    RustFfiTokenizerHandle* m_tokenizer { nullptr };
};

}
