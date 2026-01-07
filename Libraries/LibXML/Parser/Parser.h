/*
 * Copyright (c) 2022, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/GenericLexer.h>
#include <AK/HashMap.h>
#include <AK/OwnPtr.h>
#include <LibXML/DOM/Document.h>
#include <LibXML/DOM/DocumentTypeDeclaration.h>
#include <LibXML/DOM/Node.h>
#include <LibXML/Export.h>
#include <LibXML/Forward.h>

namespace XML {

struct Expectation {
    StringView expected;
};

struct ParseError {
    LineTrackingLexer::Position position {};
    Variant<ByteString, Expectation> error;
};

struct Listener {
    virtual ~Listener() { }

    virtual ErrorOr<void> set_source(ByteString) { return {}; }
    virtual void set_doctype(XML::Doctype) { }
    virtual void document_start() { }
    virtual void document_end() { }
    virtual void element_start(Name const&, OrderedHashMap<Name, ByteString> const&) { }
    virtual void element_end(Name const&) { }
    virtual void text(StringView) { }
    virtual void cdata_section(StringView) { }
    virtual void processing_instruction(StringView, StringView) { }
    virtual void comment(StringView) { }
    virtual void error(ParseError const&) { }
};

class XML_API Parser {
public:
    struct Options {
        bool preserve_cdata { true };
        bool preserve_comments { false };
        bool treat_errors_as_fatal { true };
        Function<ErrorOr<Variant<ByteString, Vector<MarkupDeclaration>>>(SystemID const&, Optional<PublicID> const&)> resolve_external_resource {};
    };

    Parser(StringView source, Options options)
        : m_source(source)
        , m_options(move(options))
    {
    }

    explicit Parser(StringView source)
        : m_source(source)
    {
    }

    ErrorOr<Document, ParseError> parse();
    ErrorOr<void, ParseError> parse_with_listener(Listener&);

    Vector<ParseError> const& parse_error_causes() const { return m_parse_errors; }

private:
    StringView m_source;
    Options m_options;
    Vector<ParseError> m_parse_errors;
};

}

template<>
struct AK::Formatter<XML::ParseError> : public AK::Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, XML::ParseError const& error)
    {
        auto error_string = error.error.visit(
            [](ByteString const& error) -> ByteString { return error; },
            [](XML::Expectation const& expectation) -> ByteString { return ByteString::formatted("Expected {}", expectation.expected); });
        return Formatter<FormatString>::format(builder, "{} at line: {}, col: {} (offset {})"sv, error_string, error.position.line, error.position.column, error.position.offset);
    }
};
