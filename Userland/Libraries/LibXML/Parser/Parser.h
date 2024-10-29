/*
 * Copyright (c) 2022, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Debug.h>
#include <AK/Function.h>
#include <AK/GenericLexer.h>
#include <AK/HashMap.h>
#include <AK/OwnPtr.h>
#include <AK/SourceLocation.h>
#include <AK/TemporaryChange.h>
#include <LibXML/DOM/Document.h>
#include <LibXML/DOM/DocumentTypeDeclaration.h>
#include <LibXML/DOM/Node.h>
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

    virtual void set_source(ByteString) { }
    virtual void set_doctype(XML::Doctype) { }
    virtual void document_start() { }
    virtual void document_end() { }
    virtual void element_start(Name const&, HashMap<Name, ByteString> const&) { }
    virtual void element_end(Name const&) { }
    virtual void text(StringView) { }
    virtual void comment(StringView) { }
    virtual void error(ParseError const&) { }
};

class Parser {
public:
    struct Options {
        bool preserve_cdata { true };
        bool preserve_comments { false };
        bool treat_errors_as_fatal { true };
        Function<ErrorOr<Variant<ByteString, Vector<MarkupDeclaration>>>(SystemID const&, Optional<PublicID> const&)> resolve_external_resource {};
    };

    Parser(StringView source, Options options)
        : m_source(source)
        , m_lexer(source)
        , m_options(move(options))
    {
    }

    explicit Parser(StringView source)
        : m_source(source)
        , m_lexer(source)
    {
    }

    ErrorOr<Document, ParseError> parse();
    ErrorOr<void, ParseError> parse_with_listener(Listener&);

    Vector<ParseError> const& parse_error_causes() const { return m_parse_errors; }

    ErrorOr<Vector<MarkupDeclaration>, ParseError> parse_external_subset();

private:
    struct EntityReference {
        Name name;
    };

    ErrorOr<void, ParseError> parse_internal();
    void append_node(NonnullOwnPtr<Node>);
    void append_text(StringView, LineTrackingLexer::Position);
    void append_comment(StringView, LineTrackingLexer::Position);
    void enter_node(Node&);
    void leave_node();

    enum class ReferencePlacement {
        AttributeValue,
        Content,
    };
    ErrorOr<ByteString, ParseError> resolve_reference(EntityReference const&, ReferencePlacement);

    enum class Required {
        No,
        Yes,
    };
    ErrorOr<void, ParseError> skip_whitespace(Required = Required::No);

    ErrorOr<void, ParseError> parse_prolog();
    ErrorOr<void, ParseError> parse_element();
    ErrorOr<void, ParseError> parse_misc();
    ErrorOr<void, ParseError> parse_xml_decl();
    ErrorOr<void, ParseError> parse_doctype_decl();
    ErrorOr<void, ParseError> parse_version_info();
    ErrorOr<void, ParseError> parse_encoding_decl();
    ErrorOr<void, ParseError> parse_standalone_document_decl();
    ErrorOr<void, ParseError> parse_eq();
    ErrorOr<void, ParseError> parse_comment();
    ErrorOr<void, ParseError> parse_processing_instruction();
    ErrorOr<Name, ParseError> parse_processing_instruction_target();
    ErrorOr<Name, ParseError> parse_name();
    ErrorOr<NonnullOwnPtr<Node>, ParseError> parse_empty_element_tag();
    ErrorOr<NonnullOwnPtr<Node>, ParseError> parse_start_tag();
    ErrorOr<Name, ParseError> parse_end_tag();
    ErrorOr<void, ParseError> parse_content();
    ErrorOr<Attribute, ParseError> parse_attribute();
    ErrorOr<ByteString, ParseError> parse_attribute_value();
    ErrorOr<Variant<EntityReference, ByteString>, ParseError> parse_reference();
    ErrorOr<StringView, ParseError> parse_char_data();
    ErrorOr<Vector<MarkupDeclaration>, ParseError> parse_internal_subset();
    ErrorOr<Optional<MarkupDeclaration>, ParseError> parse_markup_declaration();
    ErrorOr<Optional<ByteString>, ParseError> parse_declaration_separator();
    ErrorOr<Vector<MarkupDeclaration>, ParseError> parse_external_subset_declaration();
    ErrorOr<ElementDeclaration, ParseError> parse_element_declaration();
    ErrorOr<AttributeListDeclaration, ParseError> parse_attribute_list_declaration();
    ErrorOr<EntityDeclaration, ParseError> parse_entity_declaration();
    ErrorOr<NotationDeclaration, ParseError> parse_notation_declaration();
    ErrorOr<Name, ParseError> parse_parameter_entity_reference();
    ErrorOr<ElementDeclaration::ContentSpec, ParseError> parse_content_spec();
    ErrorOr<AttributeListDeclaration::Definition, ParseError> parse_attribute_definition();
    ErrorOr<StringView, ParseError> parse_nm_token();
    ErrorOr<EntityDeclaration, ParseError> parse_general_entity_declaration();
    ErrorOr<EntityDeclaration, ParseError> parse_parameter_entity_declaration();
    ErrorOr<PublicID, ParseError> parse_public_id();
    ErrorOr<SystemID, ParseError> parse_system_id();
    ErrorOr<ExternalID, ParseError> parse_external_id();
    ErrorOr<ByteString, ParseError> parse_entity_value();
    ErrorOr<Name, ParseError> parse_notation_data_declaration();
    ErrorOr<StringView, ParseError> parse_public_id_literal();
    ErrorOr<StringView, ParseError> parse_system_id_literal();
    ErrorOr<StringView, ParseError> parse_cdata_section();
    ErrorOr<ByteString, ParseError> parse_attribute_value_inner(StringView disallow);
    ErrorOr<void, ParseError> parse_text_declaration();

    ErrorOr<void, ParseError> expect(StringView);
    template<typename Pred>
    requires(IsCallableWithArguments<Pred, bool, char>) ErrorOr<StringView, ParseError> expect(Pred, StringView description);
    template<typename Pred>
    requires(IsCallableWithArguments<Pred, bool, char>) ErrorOr<StringView, ParseError> expect_many(Pred, StringView description, bool allow_empty = false);

    static size_t s_debug_indent_level;
    [[nodiscard]] auto rollback_point(SourceLocation location = SourceLocation::current())
    {
        return ArmedScopeGuard {
            [this, position = m_lexer.tell(), location] {
                m_lexer.retreat(m_lexer.tell() - position);
                (void)location;
                dbgln_if(XML_PARSER_DEBUG, "{:->{}}FAIL @ {} -- \x1b[31m{}\x1b[0m", " ", s_debug_indent_level * 2, location, m_lexer.remaining().substring_view(0, min(16, m_lexer.tell_remaining())).replace("\n"sv, "\\n"sv, ReplaceMode::All));
            }
        };
    }

    [[nodiscard]] auto accept_rule()
    {
        return TemporaryChange { m_current_rule.accept, true };
    }
    [[nodiscard]] auto enter_rule(SourceLocation location = SourceLocation::current())
    {
        dbgln_if(XML_PARSER_DEBUG, "{:->{}}Enter {}", " ", s_debug_indent_level * 2, location);
        ++s_debug_indent_level;
        auto rule = m_current_rule;
        m_current_rule = { location.function_name(), false };
        return ScopeGuard {
            [location, rule, this] {
                m_current_rule = rule;
                --s_debug_indent_level;
                (void)location;
                dbgln_if(XML_PARSER_DEBUG, "{:->{}}Leave {}", " ", s_debug_indent_level * 2, location);
            }
        };
    }

    template<typename... Ts>
    ParseError parse_error(Ts&&... args)
    {
        auto error = ParseError { forward<Ts>(args)... };
        if (m_current_rule.accept) {
            auto rule_name = m_current_rule.rule.value_or("<?>"sv);
            if (rule_name.starts_with("parse_"sv))
                rule_name = rule_name.substring_view(6);

            auto error_string = error.error.visit(
                [](ByteString const& error) -> ByteString { return error; },
                [](XML::Expectation const& expectation) -> ByteString { return ByteString::formatted("Expected {}", expectation.expected); });
            m_parse_errors.append({
                error.position,
                ByteString::formatted("{}: {}", rule_name, error_string),
            });
        }
        return error;
    }

    StringView m_source;
    LineTrackingLexer m_lexer;
    Options m_options;
    Listener* m_listener { nullptr };

    OwnPtr<Node> m_root_node;
    Node* m_entered_node { nullptr };
    Version m_version { Version::Version11 };
    bool m_in_compatibility_mode { false };
    ByteString m_encoding;
    bool m_standalone { false };
    HashMap<Name, ByteString> m_processing_instructions;
    struct AcceptedRule {
        Optional<StringView> rule {};
        bool accept { false };
    } m_current_rule {};

    Vector<ParseError> m_parse_errors;

    Optional<Doctype> m_doctype;
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
