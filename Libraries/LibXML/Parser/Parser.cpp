/*
 * Copyright (c) 2022, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibXML/DOM/Document.h>
#include <LibXML/Parser/Parser.h>

struct Range {
    consteval Range(u32 start, u32 end)
        : start(start)
        , end(end)
    {
    }

    u32 start;
    u32 end;
};

template<auto... ranges>
struct ranges_for_search {
    auto contains(u32 value) const
    {
        return ((value >= ranges.start && value <= ranges.end) || ...);
    }

    bool operator()(u32 value) const
    {
        return contains(value);
    }

    template<auto... ranges_to_include>
    consteval auto with() const
    {
        return ranges_for_search<ranges..., ranges_to_include...>();
    }

    template<auto... ranges_to_include>
    consteval auto unify(ranges_for_search<ranges_to_include...> const&) const
    {
        return ranges_for_search<ranges..., ranges_to_include...>();
    }
};

template<size_t Count, typename Element>
struct StringSet {
    consteval StringSet(Element const (&entries)[Count])
    {
        for (size_t i = 0; i < Count - 1; ++i)
            elements[i] = entries[i];
    }

    consteval auto operator[](size_t i) const { return elements[i]; }

    Element elements[Count - 1];
};

template<StringSet chars>
consteval static auto set_to_search()
{
    return ([&]<auto... Ix>(IndexSequence<Ix...>) {
        return ranges_for_search<Range(chars[Ix], chars[Ix])...>();
    }(MakeIndexSequence<array_size(chars.elements)>()));
}

namespace XML {

size_t Parser::s_debug_indent_level { 0 };

void Parser::append_node(NonnullOwnPtr<Node> node)
{
    if (m_entered_node) {
        auto& entered_element = m_entered_node->content.get<Node::Element>();
        entered_element.children.append(move(node));
        enter_node(*entered_element.children.last());
    } else {
        m_root_node = move(node);
        enter_node(*m_root_node);
    }
}

void Parser::append_text(StringView text, LineTrackingLexer::Position position)
{
    if (m_listener) {
        m_listener->text(text);
        return;
    }

    if (!m_entered_node) {
        Node::Text node;
        node.builder.append(text);
        m_root_node = make<Node>(position, move(node));
        return;
    }

    m_entered_node->content.visit(
        [&](Node::Element& node) {
            if (!node.children.is_empty()) {
                auto* text_node = node.children.last()->content.get_pointer<Node::Text>();
                if (text_node) {
                    text_node->builder.append(text);
                    return;
                }
            }
            Node::Text text_node;
            text_node.builder.append(text);
            node.children.append(make<Node>(position, move(text_node), m_entered_node));
        },
        [&](auto&) {
            // Can't enter a text or comment node.
            VERIFY_NOT_REACHED();
        });
}

void Parser::append_comment(StringView text, LineTrackingLexer::Position position)
{
    if (m_listener) {
        m_listener->comment(text);
        return;
    }

    // If there's no node to attach this to, drop it on the floor.
    // This can happen to comments in the prolog.
    if (!m_entered_node)
        return;

    m_entered_node->content.visit(
        [&](Node::Element& node) {
            node.children.append(make<Node>(position, Node::Comment { text }, m_entered_node));
        },
        [&](auto&) {
            // Can't enter a text or comment node.
            VERIFY_NOT_REACHED();
        });
}

void Parser::enter_node(Node& node)
{
    if (m_listener) {
        auto& element = node.content.get<Node::Element>();
        m_listener->element_start(element.name, element.attributes);
    }

    if (&node != m_root_node.ptr())
        node.parent = m_entered_node;
    m_entered_node = &node;
}

void Parser::leave_node()
{
    if (m_listener) {
        auto& element = m_entered_node->content.get<Node::Element>();
        m_listener->element_end(element.name);
    }

    m_entered_node = m_entered_node->parent;
}

ErrorOr<Document, ParseError> Parser::parse()
{
    if (auto result = parse_internal(); result.is_error()) {
        if (m_parse_errors.is_empty())
            return result.release_error();
        return m_parse_errors.take_first();
    }
    return Document {
        m_root_node.release_nonnull(),
        move(m_doctype),
        move(m_processing_instructions),
        m_version,
    };
}

ErrorOr<void, ParseError> Parser::parse_with_listener(Listener& listener)
{
    m_listener = &listener;
    ScopeGuard unset_listener { [this] { m_listener = nullptr; } };
    m_listener->set_source(m_source);
    m_listener->document_start();
    auto result = parse_internal();
    if (result.is_error())
        m_listener->error(result.error());
    m_listener->document_end();
    if (m_doctype.has_value()) {
        m_listener->set_doctype(m_doctype.release_value());
    }
    m_root_node.clear();
    return result;
}

// 2.3.3. S, https://www.w3.org/TR/2006/REC-xml11-20060816/#NT-S
ErrorOr<void, ParseError> Parser::skip_whitespace(Required required)
{
    auto rollback = rollback_point();
    auto rule = enter_rule();

    // S ::= (#x20 | #x9 | #xD | #xA)+
    auto matched = m_lexer.consume_while(is_any_of("\x20\x09\x0d\x0a"sv));
    if (required == Required::Yes && matched.is_empty())
        return parse_error(m_lexer.current_position(), Expectation { "whitespace"sv });

    rollback.disarm();
    return {};
}

// 2.2.a. RestrictedChar, https://www.w3.org/TR/2006/REC-xml11-20060816/#NT-RestrictedChar
constexpr static auto s_restricted_characters = ranges_for_search<Range(0x1, 0x8), Range(0xb, 0xc), Range(0xe, 0x1f), Range(0x7f, 0x84), Range(0x86, 0x9f)>();

// 2.1.1. Document, https://www.w3.org/TR/2006/REC-xml11-20060816/#sec-well-formed
ErrorOr<void, ParseError> Parser::parse_internal()
{
    auto rule = enter_rule();

    // document ::= ( prolog element Misc* ) - ( Char* RestrictedChar Char* )
    TRY(parse_prolog());
    TRY(parse_element());
    while (true) {
        if (auto result = parse_misc(); result.is_error())
            break;
    }

    auto matched_source = m_source.substring_view(0, m_lexer.tell());
    if (auto it = find_if(matched_source.begin(), matched_source.end(), s_restricted_characters); !it.is_end()) {
        return parse_error(
            m_lexer.position_for(it.index()),
            ByteString::formatted("Invalid character #{:x} used in document", *it));
    }

    if (!m_lexer.is_eof())
        return parse_error(m_lexer.current_position(), ByteString { "Garbage after document"sv });

    return {};
}

ErrorOr<void, ParseError> Parser::expect(StringView expected)
{
    auto rollback = rollback_point();

    if (!m_lexer.consume_specific(expected)) {
        if (m_options.treat_errors_as_fatal)
            return parse_error(m_lexer.current_position(), ByteString::formatted("Expected '{}'", expected));
    }

    rollback.disarm();
    return {};
}

template<typename Pred>
requires(IsCallableWithArguments<Pred, bool, char>) ErrorOr<StringView, ParseError> Parser::expect(Pred predicate, StringView description)
{
    auto rollback = rollback_point();
    auto start = m_lexer.tell();
    if (!m_lexer.next_is(predicate)) {
        if (m_options.treat_errors_as_fatal)
            return parse_error(m_lexer.current_position(), Expectation { description });
    }

    m_lexer.ignore();
    rollback.disarm();
    return m_source.substring_view(start, m_lexer.tell() - start);
}

template<typename Pred>
requires(IsCallableWithArguments<Pred, bool, char>) ErrorOr<StringView, ParseError> Parser::expect_many(Pred predicate, StringView description, bool allow_empty)
{
    auto rollback = rollback_point();
    auto start = m_lexer.tell();
    while (m_lexer.next_is(predicate)) {
        if (m_lexer.is_eof())
            break;
        m_lexer.ignore();
    }

    if (m_lexer.tell() == start && !allow_empty) {
        if (m_options.treat_errors_as_fatal) {
            return parse_error(m_lexer.current_position(), Expectation { description });
        }
    }

    rollback.disarm();
    return m_source.substring_view(start, m_lexer.tell() - start);
}

// 2.8.22. Prolog, https://www.w3.org/TR/2006/REC-xml11-20060816/#NT-prolog
ErrorOr<void, ParseError> Parser::parse_prolog()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();

    // prolog ::= XMLDecl Misc* (doctypedecl Misc*)?
    // The following is valid in XML 1.0.
    // prolog ::= XMLDecl? Misc* (doctypedecl Misc*)?
    if (auto result = parse_xml_decl(); result.is_error()) {
        m_version = Version::Version10;
        m_in_compatibility_mode = true;
    }
    auto accept = accept_rule();

    while (true) {
        if (auto result = parse_misc(); result.is_error())
            break;
    }

    if (auto result = parse_doctype_decl(); !result.is_error()) {
        while (true) {
            if (auto result = parse_misc(); result.is_error())
                break;
        }
    }

    rollback.disarm();
    return {};
}

// 2.8.23. XMLDecl, https://www.w3.org/TR/2006/REC-xml11-20060816/#NT-XMLDecl
ErrorOr<void, ParseError> Parser::parse_xml_decl()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();

    // XMLDecl::= '<?xml' VersionInfo EncodingDecl? SDDecl? S? '?>'

    TRY(expect("<?xml"sv));
    auto accept = accept_rule();

    TRY(parse_version_info());
    (void)parse_encoding_decl();
    (void)parse_standalone_document_decl();
    TRY(skip_whitespace());
    TRY(expect("?>"sv));

    rollback.disarm();
    return {};
}

// 2.8.24. VersionInfo, https://www.w3.org/TR/2006/REC-xml11-20060816/#NT-VersionInfo
ErrorOr<void, ParseError> Parser::parse_version_info()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();

    // VersionInfo ::= S 'version' Eq ("'" VersionNum "'" | '"' VersionNum '"')
    TRY(skip_whitespace(Required::Yes));
    TRY(expect("version"sv));
    auto accept = accept_rule();

    TRY(parse_eq());
    TRY(expect(is_any_of("'\""sv), "one of ' or \""sv));
    m_lexer.retreat();

    auto version_string = m_lexer.consume_quoted_string();
    if (version_string == "1.0") {
        // FIXME: Compatibility mode, figure out which rules are different in XML 1.0.
        m_version = Version::Version10;
        m_in_compatibility_mode = true;
    } else {
        if (version_string != "1.1" && m_options.treat_errors_as_fatal)
            return parse_error(m_lexer.current_position(), ByteString::formatted("Expected '1.1', found '{}'", version_string));
    }

    m_version = Version::Version11;
    rollback.disarm();
    return {};
}

// 2.8.25. Eq, https://www.w3.org/TR/2006/REC-xml11-20060816/#NT-Eq
ErrorOr<void, ParseError> Parser::parse_eq()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();

    // Eq ::= S? '=' S?
    auto accept = accept_rule();
    TRY(skip_whitespace());
    TRY(expect("="sv));
    TRY(skip_whitespace());
    rollback.disarm();
    return {};
}

// 4.3.3.80. EncodingDecl, https://www.w3.org/TR/2006/REC-xml11-20060816/#NT-EncodingDecl
ErrorOr<void, ParseError> Parser::parse_encoding_decl()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();

    // EncodingDecl ::= S 'encoding' Eq ('"' EncName '"' | "'" EncName "'" )
    TRY(skip_whitespace(Required::Yes));
    TRY(expect("encoding"sv));
    auto accept = accept_rule();

    TRY(parse_eq());
    TRY(expect(is_any_of("'\""sv), "one of ' or \""sv));
    m_lexer.retreat();

    // FIXME: Actually do something with this encoding.
    m_encoding = m_lexer.consume_quoted_string();

    rollback.disarm();
    return {};
}

// 2.9.32 SDDecl, https://www.w3.org/TR/2006/REC-xml11-20060816/#sec-rmd
ErrorOr<void, ParseError> Parser::parse_standalone_document_decl()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();

    // SDDecl ::= S 'standalone' Eq (("'" ('yes' | 'no') "'") | ('"' ('yes' | 'no') '"'))
    TRY(skip_whitespace(Required::Yes));
    TRY(expect("standalone"sv));
    auto accept = accept_rule();

    TRY(parse_eq());
    TRY(expect(is_any_of("'\""sv), "one of ' or \""sv));
    m_lexer.retreat();

    auto value = m_lexer.consume_quoted_string();
    if (!value.is_one_of("yes", "no"))
        return parse_error(m_lexer.position_for(m_lexer.tell() - value.length()), Expectation { "one of 'yes' or 'no'"sv });

    m_standalone = value == "yes";

    rollback.disarm();
    return {};
}

// 2.8.27. Misc, https://www.w3.org/TR/2006/REC-xml11-20060816/#NT-Misc
ErrorOr<void, ParseError> Parser::parse_misc()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();

    // Misc ::= Comment | PI | S
    if (auto result = parse_comment(); !result.is_error()) {
        rollback.disarm();
        return {};
    }

    if (auto result = parse_processing_instruction(); !result.is_error()) {
        rollback.disarm();
        return {};
    }

    if (auto result = skip_whitespace(Required::Yes); !result.is_error()) {
        rollback.disarm();
        return {};
    }

    return parse_error(m_lexer.current_position(), Expectation { "a match for 'Misc'"sv });
}

// 2.5.15 Comment, https://www.w3.org/TR/2006/REC-xml11-20060816/#NT-Comment
ErrorOr<void, ParseError> Parser::parse_comment()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();

    // Comment ::= '<!--' ((Char - '-') | ('-' (Char - '-')))* '-->'
    auto comment_start = m_lexer.tell();
    TRY(expect("<!--"sv));
    auto accept = accept_rule();

    bool last_seen_a_dash = false;
    // FIXME: This should disallow surrogate blocks
    auto text = m_lexer.consume_while([&](auto ch) {
        if (ch != '-') {
            last_seen_a_dash = false;
            return true;
        }

        if (last_seen_a_dash)
            return false;

        last_seen_a_dash = true;
        return true;
    });

    if (last_seen_a_dash) {
        m_lexer.retreat();
        text = text.substring_view(0, text.length() - 1);
    }

    TRY(expect("-->"sv));

    if (m_options.preserve_comments)
        append_comment(text, m_lexer.position_for(comment_start));

    rollback.disarm();
    return {};
}

// 2.6.16 PI, https://www.w3.org/TR/2006/REC-xml11-20060816/#NT-PI
ErrorOr<void, ParseError> Parser::parse_processing_instruction()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();

    // PI ::= '<?' PITarget (S (Char* - (Char* '?>' Char*)))? '?>'
    TRY(expect("<?"sv));
    auto accept = accept_rule();

    auto target = TRY(parse_processing_instruction_target());
    ByteString data;
    if (auto result = skip_whitespace(Required::Yes); !result.is_error())
        data = m_lexer.consume_until("?>");
    TRY(expect("?>"sv));

    m_processing_instructions.set(target, data);
    rollback.disarm();
    return {};
}

// 2.6.17. PITarget, https://www.w3.org/TR/2006/REC-xml11-20060816/#NT-PITarget
ErrorOr<Name, ParseError> Parser::parse_processing_instruction_target()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();

    // PITarget ::= Name - (('X' | 'x') ('M' | 'm') ('L' | 'l'))
    auto target = TRY(parse_name());
    auto accept = accept_rule();

    if (target.equals_ignoring_ascii_case("xml"sv) && m_options.treat_errors_as_fatal) {
        return parse_error(
            m_lexer.position_for(m_lexer.tell() - target.length()),
            ByteString { "Use of the reserved 'xml' name for processing instruction target name is disallowed"sv });
    }

    rollback.disarm();
    return target;
}

// NameStartChar ::= ":" | [A-Z] | "_" | [a-z] | [#xC0-#xD6] | [#xD8-#xF6] | [#xF8-#x2FF] | [#x370-#x37D] | [#x37F-#x1FFF] | [#x200C-#x200D] | [#x2070-#x218F] | [#x2C00-#x2FEF] | [#x3001-#xD7FF] | [#xF900-#xFDCF] | [#xFDF0-#xFFFD] | [#x10000-#xEFFFF]
constexpr static auto s_name_start_characters = ranges_for_search<Range(':', ':'), Range('A', 'Z'), Range('_', '_'), Range('a', 'z'), Range(0xc0, 0xd6), Range(0xd8, 0xf6), Range(0xf8, 0x2ff), Range(0x370, 0x37d), Range(0x37f, 0x1fff), Range(0x200c, 0x200d), Range(0x2070, 0x218f), Range(0x2c00, 0x2fef), Range(0x3001, 0xd7ff), Range(0xf900, 0xfdcf), Range(0xfdf0, 0xfffd), Range(0x10000, 0xeffff)> {};

// NameChar ::= NameStartChar | "-" | "." | [0-9] | #xB7 | [#x0300-#x036F] | [#x203F-#x2040]
constexpr static auto s_name_characters = s_name_start_characters.with<Range('-', '-'), Range('.', '.'), Range('0', '9'), Range(0xb7, 0xb7), Range(0x0300, 0x036f), Range(0x203f, 0x2040)>();

// 2.3.5. Name, https://www.w3.org/TR/2006/REC-xml11-20060816/#NT-Name
ErrorOr<Name, ParseError> Parser::parse_name()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();

    // Name ::= NameStartChar (NameChar)*

    // FIXME: This is a hacky workaround to read code points instead of bytes.
    // Replace this once we have a unicode-aware lexer.
    auto start = m_lexer.tell();
    StringView remaining = m_lexer.input().substring_view(start);
    Utf8View view { remaining };
    auto code_points = view.begin();
    if (code_points.done() || !s_name_start_characters.contains(*code_points)) {
        if (m_options.treat_errors_as_fatal)
            return parse_error(m_lexer.current_position(), Expectation { "a NameStartChar"sv });
    }

    m_lexer.ignore(code_points.underlying_code_point_length_in_bytes());
    ++code_points;

    auto accept = accept_rule();

    while (!code_points.done() && s_name_characters.contains(*code_points)) {
        m_lexer.ignore(code_points.underlying_code_point_length_in_bytes());
        ++code_points;
    }

    rollback.disarm();
    return remaining.substring_view(0, m_lexer.tell() - start);
}

// 2.8.28. doctypedecl, https://www.w3.org/TR/2006/REC-xml11-20060816/#NT-doctypedecl
ErrorOr<void, ParseError> Parser::parse_doctype_decl()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();
    Doctype doctype;

    // doctypedecl ::= '<!DOCTYPE' S Name (S ExternalID)? S? ('[' intSubset ']' S?)? '>'
    TRY(expect("<!DOCTYPE"sv));
    auto accept = accept_rule();

    TRY(skip_whitespace(Required::Yes));
    doctype.type = TRY(parse_name());
    if (auto result = skip_whitespace(Required::Yes); !result.is_error()) {
        if (auto id_result = parse_external_id(); !id_result.is_error()) {
            doctype.external_id = id_result.release_value();
            if (m_options.resolve_external_resource) {
                auto resource_result = m_options.resolve_external_resource(doctype.external_id->system_id, doctype.external_id->public_id);
                if (!resource_result.is_error()) {
                    auto declarations = TRY(resource_result.release_value().visit(
                        [&](ByteString resolved_source) -> ErrorOr<Vector<MarkupDeclaration>, ParseError> {
                            TemporaryChange source { m_source, resolved_source.view() };
                            TemporaryChange lexer { m_lexer, LineTrackingLexer(m_source) };
                            auto declarations = TRY(parse_external_subset());
                            if (!m_lexer.is_eof()) {
                                return parse_error(
                                    m_lexer.current_position(),
                                    ByteString::formatted("Failed to resolve external subset '{}': garbage after declarations", doctype.external_id->system_id.system_literal));
                            }
                            return declarations;
                        },
                        [&](Vector<MarkupDeclaration> declarations) -> ErrorOr<Vector<MarkupDeclaration>, ParseError> {
                            return declarations;
                        }));
                    doctype.markup_declarations.extend(move(declarations));
                }
            }
        }
    }
    TRY(skip_whitespace(Required::No));
    if (m_lexer.consume_specific('[')) {
        auto internal_subset = TRY(parse_internal_subset());
        TRY(expect("]"sv));
        TRY(skip_whitespace());
        doctype.markup_declarations.extend(internal_subset);
    }

    TRY(expect(">"sv));

    rollback.disarm();
    m_doctype = move(doctype);
    return {};
}

// 3.39. element, https://www.w3.org/TR/2006/REC-xml11-20060816/#NT-element
ErrorOr<void, ParseError> Parser::parse_element()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();

    // element ::= EmptyElemTag
    //           | STag content ETag
    if (auto result = parse_empty_element_tag(); !result.is_error()) {
        append_node(result.release_value());
        leave_node();
        rollback.disarm();
        return {};
    }

    auto accept = accept_rule();
    auto start_tag = TRY(parse_start_tag());
    auto& node = *start_tag;
    auto& tag = node.content.get<Node::Element>();
    append_node(move(start_tag));
    ScopeGuard quit {
        [&] {
            leave_node();
        }
    };

    TRY(parse_content());

    auto tag_location = m_lexer.tell();
    auto closing_name = TRY(parse_end_tag());

    // Well-formedness constraint: The Name in an element's end-tag MUST match the element type in the start-tag.
    if (m_options.treat_errors_as_fatal && closing_name != tag.name)
        return parse_error(m_lexer.position_for(tag_location), ByteString { "Invalid closing tag"sv });

    rollback.disarm();
    return {};
}

// 3.1.44. EmptyElemTag, https://www.w3.org/TR/2006/REC-xml11-20060816/#NT-EmptyElemTag
ErrorOr<NonnullOwnPtr<Node>, ParseError> Parser::parse_empty_element_tag()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();

    // EmptyElemTag ::= '<' Name (S Attribute)* S? '/>'
    auto tag_start = m_lexer.tell();
    TRY(expect("<"sv));

    auto name = TRY(parse_name());
    HashMap<Name, ByteString> attributes;

    while (true) {
        if (auto result = skip_whitespace(Required::Yes); result.is_error())
            break;

        if (auto result = parse_attribute(); !result.is_error()) {
            auto attribute = result.release_value();
            attributes.set(move(attribute.name), move(attribute.value));
        } else {
            break;
        }
    }

    TRY(skip_whitespace());
    TRY(expect("/>"sv));

    auto accept = accept_rule();

    rollback.disarm();
    return make<Node>(m_lexer.position_for(tag_start), Node::Element { move(name), move(attributes), {} });
}

// 3.1.41. Attribute, https://www.w3.org/TR/2006/REC-xml11-20060816/#NT-Attribute
ErrorOr<Attribute, ParseError> Parser::parse_attribute()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();

    // Attribute ::= Name Eq AttValue
    auto name = TRY(parse_name());
    auto accept = accept_rule();

    TRY(parse_eq());
    auto value = TRY(parse_attribute_value());

    rollback.disarm();
    return Attribute {
        move(name),
        move(value),
    };
}

// 2.3.10. AttValue, https://www.w3.org/TR/2006/REC-xml11-20060816/#NT-AttValue
ErrorOr<ByteString, ParseError> Parser::parse_attribute_value()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();

    // AttValue ::= '"' ([^<&"] | Reference)* '"'
    //            | "'" ([^<&'] | Reference)* "'"
    auto quote = TRY(expect(is_any_of("'\""sv), "one of ' or \""sv));
    auto accept = accept_rule();

    auto text = TRY(parse_attribute_value_inner(quote));
    TRY(expect(quote));

    rollback.disarm();
    return text;
}

ErrorOr<ByteString, ParseError> Parser::parse_attribute_value_inner(StringView disallow)
{
    StringBuilder builder;
    while (true) {
        if (m_lexer.next_is(is_any_of(disallow)) || m_lexer.is_eof())
            break;

        if (m_lexer.next_is('<')) {
            // Not allowed, return a nice error to make it easier to debug.
            return parse_error(m_lexer.current_position(), ByteString { "Unescaped '<' not allowed in attribute values"sv });
        }

        if (m_lexer.next_is('&')) {
            auto reference = TRY(parse_reference());
            if (auto* char_reference = reference.get_pointer<ByteString>())
                builder.append(*char_reference);
            else
                builder.append(TRY(resolve_reference(reference.get<EntityReference>(), ReferencePlacement::AttributeValue)));
        } else {
            builder.append(m_lexer.consume());
        }
    }
    return builder.to_byte_string();
}

// Char ::= [#x1-#xD7FF] | [#xE000-#xFFFD] | [#x10000-#x10FFFF]
constexpr static auto s_characters = ranges_for_search<Range(0x1, 0xd7ff), Range(0xe000, 0xfffd), Range(0x10000, 0x10ffff)>();

// 4.1.67. Reference, https://www.w3.org/TR/2006/REC-xml11-20060816/#NT-Reference
ErrorOr<Variant<Parser::EntityReference, ByteString>, ParseError> Parser::parse_reference()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();
    // Reference ::= EntityRef | CharRef

    // 4.1.68. EntityRef
    // EntityRef ::= '&' Name ';'

    // 4.1.66. CharRef
    // CharRef ::= '&#' [0-9]+ ';'
    //           | '&#x' [0-9a-fA-F]+ ';'

    auto reference_start = m_lexer.tell();
    TRY(expect("&"sv));
    auto accept = accept_rule();

    auto name_result = parse_name();
    if (name_result.is_error()) {
        TRY(expect("#"sv));
        Optional<u32> code_point;
        if (m_lexer.consume_specific('x')) {
            auto hex = TRY(expect_many(
                ranges_for_search<Range('0', '9'), Range('a', 'f'), Range('A', 'F')>(),
                "any of [0-9a-fA-F]"sv));
            code_point = AK::StringUtils::convert_to_uint_from_hex<u32>(hex);
        } else {
            auto decimal = TRY(expect_many(
                ranges_for_search<Range('0', '9')>(),
                "any of [0-9]"sv));
            code_point = decimal.to_number<u32>();
        }

        if (!code_point.has_value() || !s_characters.contains(*code_point))
            return parse_error(m_lexer.position_for(reference_start), ByteString { "Invalid character reference"sv });

        TRY(expect(";"sv));

        StringBuilder builder;
        builder.append_code_point(*code_point);

        rollback.disarm();
        return builder.to_byte_string();
    }

    auto name = name_result.release_value();
    TRY(expect(";"sv));

    rollback.disarm();
    return EntityReference { move(name) };
}

// 3.1.40 STag, https://www.w3.org/TR/2006/REC-xml11-20060816/#NT-STag
ErrorOr<NonnullOwnPtr<Node>, ParseError> Parser::parse_start_tag()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();

    // STag ::= '<' Name (S Attribute)* S? '>'
    auto tag_start = m_lexer.tell();
    TRY(expect("<"sv));
    auto accept = accept_rule();

    auto name = TRY(parse_name());
    HashMap<Name, ByteString> attributes;

    while (true) {
        if (auto result = skip_whitespace(Required::Yes); result.is_error())
            break;

        if (auto result = parse_attribute(); !result.is_error()) {
            auto attribute = result.release_value();
            attributes.set(move(attribute.name), move(attribute.value));
        } else {
            break;
        }
    }

    TRY(skip_whitespace());
    TRY(expect(">"sv));

    rollback.disarm();
    return make<Node>(m_lexer.position_for(tag_start), Node::Element { move(name), move(attributes), {} });
}

// 3.1.42 ETag, https://www.w3.org/TR/2006/REC-xml11-20060816/#NT-ETag
ErrorOr<Name, ParseError> Parser::parse_end_tag()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();

    // ETag ::= '</' Name S? '>'
    TRY(expect("</"sv));
    auto accept = accept_rule();

    auto name = TRY(parse_name());
    TRY(skip_whitespace());
    TRY(expect(">"sv));

    rollback.disarm();
    return name;
}

// 3.1.42 content, https://www.w3.org/TR/2006/REC-xml11-20060816/#NT-content
ErrorOr<void, ParseError> Parser::parse_content()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();
    auto accept = accept_rule();

    // content ::= CharData? ((element | Reference | CDSect | PI | Comment) CharData?)*
    auto content_start = m_lexer.tell();
    if (auto result = parse_char_data(); !result.is_error())
        append_text(result.release_value(), m_lexer.position_for(content_start));

    while (true) {
        auto node_start = m_lexer.tell();

        if (auto result = parse_element(); !result.is_error())
            goto try_char_data;
        if (auto result = parse_reference(); !result.is_error()) {
            auto reference = result.release_value();
            auto reference_offset = m_lexer.position_for(node_start);
            if (auto char_reference = reference.get_pointer<ByteString>())
                append_text(*char_reference, reference_offset);
            else
                append_text(TRY(resolve_reference(reference.get<EntityReference>(), ReferencePlacement::Content)), reference_offset);
            goto try_char_data;
        }
        if (auto result = parse_cdata_section(); !result.is_error()) {
            if (m_options.preserve_cdata)
                append_text(result.release_value(), m_lexer.position_for(node_start));
            goto try_char_data;
        }
        if (auto result = parse_processing_instruction(); !result.is_error())
            goto try_char_data;
        if (auto result = parse_comment(); !result.is_error())
            goto try_char_data;

        break;

    try_char_data:;
        if (auto result = parse_char_data(); !result.is_error())
            append_text(result.release_value(), m_lexer.position_for(node_start));
    }

    rollback.disarm();
    return {};
}

// 2.4.14 CharData, https://www.w3.org/TR/2006/REC-xml11-20060816/#NT-CharData
ErrorOr<StringView, ParseError> Parser::parse_char_data()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();

    // CharData ::= [^<&]* - ([^<&]* ']]>' [^<&]*)
    auto cend_state = 0; // 1: ], 2: ], 3: >
    auto text = m_lexer.consume_while([&](auto ch) {
        if (ch == '<' || ch == '&' || cend_state == 3)
            return false;
        switch (cend_state) {
        case 0:
        case 1:
            if (ch == ']')
                cend_state++;
            else
                cend_state = 0;
            return true;
        case 2:
            if (ch == '>') {
                cend_state++;
                return true;
            }
            cend_state = 0;
            return true;
        default:
            VERIFY_NOT_REACHED();
        }
    });
    if (cend_state == 3) {
        m_lexer.retreat(3);
        text = text.substring_view(0, text.length() - 3);
    }

    rollback.disarm();
    return text;
}

// 2.8.28b intSubset, https://www.w3.org/TR/2006/REC-xml11-20060816/#NT-intSubset
ErrorOr<Vector<MarkupDeclaration>, ParseError> Parser::parse_internal_subset()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();
    Vector<MarkupDeclaration> declarations;

    // intSubset ::= (markupdecl | DeclSep)*
    while (true) {
        if (auto result = parse_markup_declaration(); !result.is_error()) {
            auto maybe_declaration = result.release_value();
            if (maybe_declaration.has_value())
                declarations.append(maybe_declaration.release_value());
            continue;
        }
        if (auto result = parse_declaration_separator(); !result.is_error()) {
            // The markup declarations may be made up in whole or in part of the replacement text of parameter entities.
            // The replacement text of a parameter entity reference in a DeclSep MUST match the production extSubsetDecl.
            auto maybe_replacement_text = result.release_value();
            if (maybe_replacement_text.has_value()) {
                TemporaryChange<StringView> source { m_source, maybe_replacement_text.value() };
                TemporaryChange lexer { m_lexer, LineTrackingLexer { m_source } };

                auto contained_declarations = TRY(parse_external_subset_declaration());
                declarations.extend(move(contained_declarations));
            }
            continue;
        }
        break;
    }

    rollback.disarm();
    return declarations;
}

// 2.8.29 markupdecl, https://www.w3.org/TR/2006/REC-xml11-20060816/#NT-markupdecl
ErrorOr<Optional<MarkupDeclaration>, ParseError> Parser::parse_markup_declaration()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();

    // markupdecl ::= elementdecl | AttlistDecl | EntityDecl | NotationDecl | PI | Comment
    if (auto result = parse_element_declaration(); !result.is_error()) {
        rollback.disarm();
        return MarkupDeclaration { result.release_value() };
    }
    if (auto result = parse_attribute_list_declaration(); !result.is_error()) {
        rollback.disarm();
        return MarkupDeclaration { result.release_value() };
    }
    if (auto result = parse_entity_declaration(); !result.is_error()) {
        rollback.disarm();
        return MarkupDeclaration { result.release_value() };
    }
    if (auto result = parse_notation_declaration(); !result.is_error()) {
        rollback.disarm();
        return MarkupDeclaration { result.release_value() };
    }
    if (auto result = parse_processing_instruction(); !result.is_error()) {
        rollback.disarm();
        return Optional<MarkupDeclaration> {};
    }
    if (auto result = parse_comment(); !result.is_error()) {
        rollback.disarm();
        return Optional<MarkupDeclaration> {};
    }

    return parse_error(m_lexer.current_position(), Expectation { "one of elementdecl, attlistdecl, entitydecl, notationdecl, PI or comment"sv });
}

// 2.8.28a DeclSep, https://www.w3.org/TR/2006/REC-xml11-20060816/#NT-DeclSep
ErrorOr<Optional<ByteString>, ParseError> Parser::parse_declaration_separator()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();

    // DeclSep ::= PEReference | S
    if (auto name = parse_parameter_entity_reference(); !name.is_error()) {
        rollback.disarm();
        // FIXME: Resolve this PEReference.
        return "";
    }

    if (auto result = skip_whitespace(Required::Yes); !result.is_error()) {
        rollback.disarm();
        return Optional<ByteString> {};
    }

    return parse_error(m_lexer.current_position(), Expectation { "either whitespace, or a PEReference"sv });
}

// 4.1.69 PEReference, https://www.w3.org/TR/2006/REC-xml11-20060816/#NT-PEReference
ErrorOr<Name, ParseError> Parser::parse_parameter_entity_reference()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();

    // PEReference ::= '%' Name ';'
    TRY(expect("%"sv));
    auto accept = accept_rule();

    auto name = TRY(parse_name());
    TRY(expect(";"sv));

    rollback.disarm();
    return name;
}

// 3.2.46 elementdecl, https://www.w3.org/TR/2006/REC-xml11-20060816/#NT-elementdecl
ErrorOr<ElementDeclaration, ParseError> Parser::parse_element_declaration()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();

    // FIXME: Apparently both name _and_ contentspec here are allowed to be PEReferences,
    //        but the grammar does not allow that, figure this out.
    // elementdecl ::= '<!ELEMENT' S Name S contentspec S? '>'
    TRY(expect("<!ELEMENT"sv));
    auto accept = accept_rule();

    TRY(skip_whitespace(Required::Yes));
    auto name = TRY(parse_name());
    TRY(skip_whitespace(Required::Yes));
    auto spec = TRY(parse_content_spec());
    TRY(expect(">"sv));

    rollback.disarm();
    return ElementDeclaration {
        move(name),
        move(spec),
    };
}

// 3.3.52 AttlistDecl, https://www.w3.org/TR/2006/REC-xml11-20060816/#NT-AttlistDecl
ErrorOr<AttributeListDeclaration, ParseError> Parser::parse_attribute_list_declaration()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();
    AttributeListDeclaration declaration;

    // AttlistDecl ::= '<!ATTLIST' S Name AttDef* S? '>'
    TRY(expect("<!ATTLIST"sv));
    auto accept = accept_rule();

    TRY(skip_whitespace(Required::Yes));
    declaration.type = TRY(parse_name());

    while (true) {
        if (auto result = parse_attribute_definition(); !result.is_error())
            declaration.attributes.append(result.release_value());
        else
            break;
    }

    TRY(skip_whitespace());
    TRY(expect(">"sv));

    rollback.disarm();
    return declaration;
}

// 3.3.53 AttDef, https://www.w3.org/TR/2006/REC-xml11-20060816/#NT-AttDef
ErrorOr<AttributeListDeclaration::Definition, ParseError> Parser::parse_attribute_definition()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();
    Optional<AttributeListDeclaration::Type> type;
    Optional<AttributeListDeclaration::Default> default_;

    // AttDef ::= S Name S AttType S DefaultDecl
    TRY(skip_whitespace(Required::Yes));
    auto name = TRY(parse_name());
    auto accept = accept_rule();

    TRY(skip_whitespace(Required::Yes));

    // AttType ::= StringType | TokenizedType | EnumeratedType
    // StringType ::= 'CDATA'
    // TokenizedType ::= 'ID'
    //                  | 'IDREF'
    //                  | 'IDREFS'
    //                  | 'ENTITY'
    //                  | 'ENTITIES'
    //                  | 'NMTOKEN'
    //                  | 'NMTOKENS'
    // EnumeratedType ::= NotationType | Enumeration
    // NotationType ::= 'NOTATION' S '(' S? Name (S? '|' S? Name)* S? ')'
    // Enumeration ::= '(' S? Nmtoken (S? '|' S? Nmtoken)* S? ')'
    if (m_lexer.consume_specific("CDATA"sv)) {
        type = AttributeListDeclaration::StringType::CData;
    } else if (m_lexer.consume_specific("IDREFS"sv)) {
        type = AttributeListDeclaration::TokenizedType::IDRefs;
    } else if (m_lexer.consume_specific("IDREF"sv)) {
        type = AttributeListDeclaration::TokenizedType::IDRef;
    } else if (m_lexer.consume_specific("ID"sv)) {
        type = AttributeListDeclaration::TokenizedType::ID;
    } else if (m_lexer.consume_specific("ENTITIES"sv)) {
        type = AttributeListDeclaration::TokenizedType::Entities;
    } else if (m_lexer.consume_specific("ENTITY"sv)) {
        type = AttributeListDeclaration::TokenizedType::Entity;
    } else if (m_lexer.consume_specific("NMTOKENS"sv)) {
        type = AttributeListDeclaration::TokenizedType::NMTokens;
    } else if (m_lexer.consume_specific("NMTOKEN"sv)) {
        type = AttributeListDeclaration::TokenizedType::NMToken;
    } else if (m_lexer.consume_specific("NOTATION"sv)) {
        HashTable<Name> names;
        TRY(skip_whitespace(Required::Yes));
        TRY(expect("("sv));
        TRY(skip_whitespace());
        names.set(TRY(parse_name()));
        while (true) {
            TRY(skip_whitespace());
            if (auto result = expect("|"sv); result.is_error())
                break;
            TRY(skip_whitespace());
            names.set(TRY(parse_name()));
        }
        TRY(skip_whitespace());
        TRY(expect(")"sv));
        type = AttributeListDeclaration::NotationType { move(names) };
    } else {
        HashTable<ByteString> names;
        TRY(expect("("sv));
        TRY(skip_whitespace());
        names.set(TRY(parse_nm_token()));
        while (true) {
            TRY(skip_whitespace());
            if (auto result = expect("|"sv); result.is_error())
                break;
            TRY(skip_whitespace());
            names.set(TRY(parse_nm_token()));
        }
        TRY(skip_whitespace());
        TRY(expect(")"sv));
        type = AttributeListDeclaration::Enumeration { move(names) };
    }

    TRY(skip_whitespace(Required::Yes));

    // DefaultDecl ::= '#REQUIRED' | '#IMPLIED'
    //               | (('#FIXED' S)? AttValue)
    if (m_lexer.consume_specific("#REQUIRED"sv)) {
        default_ = AttributeListDeclaration::Required {};
    } else if (m_lexer.consume_specific("#IMPLIED"sv)) {
        default_ = AttributeListDeclaration::Implied {};
    } else {
        bool fixed = false;
        if (m_lexer.consume_specific("#FIXED"sv)) {
            TRY(skip_whitespace(Required::Yes));
            fixed = true;
        }
        auto value = TRY(parse_attribute_value());
        if (fixed)
            default_ = AttributeListDeclaration::Fixed { move(value) };
        else
            default_ = AttributeListDeclaration::DefaultValue { move(value) };
    }

    rollback.disarm();
    return AttributeListDeclaration::Definition {
        move(name),
        type.release_value(),
        default_.release_value(),
    };
}

// 2.3.7 Nmtoken, https://www.w3.org/TR/2006/REC-xml11-20060816/#NT-Nmtoken
ErrorOr<StringView, ParseError> Parser::parse_nm_token()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();

    // Nmtoken ::= (NameChar)+
    auto token = TRY(expect_many(s_name_characters, "a NameChar"sv));

    rollback.disarm();
    return token;
}

// 4.7.82 NotationDecl, https://www.w3.org/TR/2006/REC-xml11-20060816/#Notations
ErrorOr<NotationDeclaration, ParseError> Parser::parse_notation_declaration()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();
    Variant<ExternalID, PublicID, Empty> notation;

    // NotationDecl ::= '<!NOTATION' S Name S (ExternalID | PublicID) S? '>'
    TRY(expect("<!NOTATION"sv));
    auto accept = accept_rule();

    TRY(skip_whitespace(Required::Yes));
    auto name = TRY(parse_name());
    TRY(skip_whitespace(Required::Yes));

    if (auto result = parse_external_id(); !result.is_error())
        notation = result.release_value();
    else
        notation = TRY(parse_public_id());

    TRY(expect(">"sv));

    rollback.disarm();
    return NotationDeclaration {
        move(name),
        move(notation).downcast<ExternalID, PublicID>(),
    };
}

// 3.2.46 contentspec, https://www.w3.org/TR/2006/REC-xml11-20060816/#NT-contentspec
ErrorOr<ElementDeclaration::ContentSpec, ParseError> Parser::parse_content_spec()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();
    Optional<ElementDeclaration::ContentSpec> content_spec;

    // contentspec ::= 'EMPTY' | 'ANY' | Mixed | children
    if (m_lexer.consume_specific("EMPTY"sv)) {
        content_spec = ElementDeclaration::Empty {};
    } else if (m_lexer.consume_specific("ANY"sv)) {
        content_spec = ElementDeclaration::Any {};
    } else {
        TRY(expect("("sv));
        TRY(skip_whitespace());
        if (m_lexer.consume_specific("#PCDATA"sv)) {
            HashTable<Name> names;
            // Mixed ::= '(' S? '#PCDATA' (S? '|' S? Name)* S? ')*'
            //         | '(' S? '#PCDATA' S? ')'
            TRY(skip_whitespace());
            if (m_lexer.consume_specific(")*"sv)) {
                content_spec = ElementDeclaration::Mixed { .types = {}, .many = true };
            } else if (m_lexer.consume_specific(')')) {
                content_spec = ElementDeclaration::Mixed { .types = {}, .many = false };
            } else {
                while (true) {
                    TRY(skip_whitespace());
                    if (!m_lexer.consume_specific('|'))
                        break;
                    TRY(skip_whitespace());
                    if (auto result = parse_name(); !result.is_error())
                        names.set(result.release_value());
                    else
                        return parse_error(m_lexer.current_position(), Expectation { "a Name"sv });
                }
                TRY(skip_whitespace());
                TRY(expect(")*"sv));
                content_spec = ElementDeclaration::Mixed { .types = move(names), .many = true };
            }
        } else {
            while (!m_lexer.next_is('('))
                m_lexer.retreat();
            // children ::= (choice | seq) ('?' | '*' | '+')?
            //   cp ::= (Name | choice | seq) ('?' | '*' | '+')?
            //   choice ::= '(' S? cp ( S? '|' S? cp )+ S? ')'
            //   seq ::= '(' S? cp ( S? ',' S? cp )* S? ')'
            Function<ErrorOr<ElementDeclaration::Children::Choice, ParseError>()> parse_choice;
            Function<ErrorOr<ElementDeclaration::Children::Sequence, ParseError>()> parse_sequence;

            auto parse_cp_init = [&]() -> ErrorOr<Variant<Name, ElementDeclaration::Children::Choice, ElementDeclaration::Children::Sequence>, ParseError> {
                if (auto result = parse_name(); !result.is_error())
                    return result.release_value();
                if (auto result = parse_choice(); !result.is_error())
                    return result.release_value();
                return TRY(parse_sequence());
            };
            auto parse_qualifier = [&]() -> ElementDeclaration::Children::Qualifier {
                ElementDeclaration::Children::Qualifier qualifier { ElementDeclaration::Children::Qualifier::ExactlyOnce };
                if (m_lexer.consume_specific('?'))
                    qualifier = ElementDeclaration::Children::Qualifier::Optional;
                else if (m_lexer.consume_specific('*'))
                    qualifier = ElementDeclaration::Children::Qualifier::Any;
                else if (m_lexer.consume_specific('+'))
                    qualifier = ElementDeclaration::Children::Qualifier::OneOrMore;
                return qualifier;
            };
            auto parse_cp = [&]() -> ErrorOr<ElementDeclaration::Children::Entry, ParseError> {
                auto sub_entry = TRY(parse_cp_init());
                auto qualifier = parse_qualifier();
                return ElementDeclaration::Children::Entry {
                    move(sub_entry),
                    qualifier,
                };
            };
            parse_choice = [&]() -> ErrorOr<ElementDeclaration::Children::Choice, ParseError> {
                auto rollback = rollback_point();
                auto rule = enter_rule();

                TRY(expect("("sv));
                auto accept = accept_rule();

                TRY(skip_whitespace());
                Vector<ElementDeclaration::Children::Entry> choices;
                choices.append(TRY(parse_cp()));
                while (true) {
                    TRY(skip_whitespace());
                    if (!m_lexer.consume_specific('|'))
                        break;
                    TRY(skip_whitespace());
                    choices.append(TRY(parse_cp()));
                }

                TRY(expect(")"sv));

                if (choices.size() < 2)
                    return parse_error(m_lexer.current_position(), Expectation { "more than one choice"sv });

                TRY(skip_whitespace());
                auto qualifier = parse_qualifier();

                rollback.disarm();
                return ElementDeclaration::Children::Choice {
                    move(choices),
                    qualifier,
                };
            };
            parse_sequence = [&]() -> ErrorOr<ElementDeclaration::Children::Sequence, ParseError> {
                auto rollback = rollback_point();
                auto rule = enter_rule();

                TRY(expect("("sv));
                auto accept = accept_rule();

                TRY(skip_whitespace());
                Vector<ElementDeclaration::Children::Entry> entries;
                entries.append(TRY(parse_cp()));
                while (true) {
                    TRY(skip_whitespace());
                    if (!m_lexer.consume_specific(','))
                        break;
                    TRY(skip_whitespace());
                    entries.append(TRY(parse_cp()));
                }

                TRY(expect(")"sv));

                TRY(skip_whitespace());
                auto qualifier = parse_qualifier();

                rollback.disarm();
                return ElementDeclaration::Children::Sequence {
                    move(entries),
                    qualifier,
                };
            };
            if (auto result = parse_choice(); !result.is_error()) {
                auto qualifier = parse_qualifier();
                content_spec = ElementDeclaration::Children {
                    result.release_value(),
                    qualifier,
                };
            } else {
                auto sequence = TRY(parse_sequence());
                auto qualifier = parse_qualifier();
                content_spec = ElementDeclaration::Children {
                    move(sequence),
                    qualifier,
                };
            }
        }
    }

    rollback.disarm();
    return content_spec.release_value();
}

// 2.8.31 extSubsetDecl, https://www.w3.org/TR/2006/REC-xml11-20060816/#NT-extSubsetDecl
ErrorOr<Vector<MarkupDeclaration>, ParseError> Parser::parse_external_subset_declaration()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();
    Vector<MarkupDeclaration> declarations;

    // extSubsetDecl ::= ( markupdecl | conditionalSect | DeclSep )*
    while (true) {
        if (auto result = parse_markup_declaration(); !result.is_error()) {
            if (result.value().has_value())
                declarations.append(result.release_value().release_value());
            continue;
        }

        // FIXME: conditionalSect

        if (auto result = parse_declaration_separator(); !result.is_error())
            continue;

        break;
    }

    rollback.disarm();
    return declarations;
}

// 4.2.70 EntityDecl, https://www.w3.org/TR/xml/#NT-EntityDecl
ErrorOr<EntityDeclaration, ParseError> Parser::parse_entity_declaration()
{
    // EntityDecl ::= GEDecl | PEDecl
    if (auto result = parse_general_entity_declaration(); !result.is_error())
        return result;

    return parse_parameter_entity_declaration();
}

// 4.2.71 GEDecl, https://www.w3.org/TR/xml/#NT-GEDecl
ErrorOr<EntityDeclaration, ParseError> Parser::parse_general_entity_declaration()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();
    Variant<ByteString, EntityDefinition, Empty> definition;

    // GEDecl ::= '<!ENTITY' S Name S EntityDef S? '>'
    TRY(expect("<!ENTITY"sv));
    auto accept = accept_rule();

    TRY(skip_whitespace(Required::Yes));
    auto name = TRY(parse_name());
    TRY(skip_whitespace(Required::Yes));
    // EntityDef ::= EntityValue | (ExternalID NDataDecl?)
    if (auto result = parse_entity_value(); !result.is_error()) {
        definition = result.release_value();
    } else {
        auto external_id = TRY(parse_external_id());
        Optional<Name> notation;
        if (auto notation_result = parse_notation_data_declaration(); !notation_result.is_error())
            notation = notation_result.release_value();

        definition = EntityDefinition {
            move(external_id),
            move(notation),
        };
    }

    TRY(skip_whitespace());
    TRY(expect(">"sv));

    rollback.disarm();
    return GEDeclaration {
        move(name),
        move(definition).downcast<ByteString, EntityDefinition>(),
    };
}

// 4.2.72 PEDecl, https://www.w3.org/TR/xml/#NT-PEDecl
ErrorOr<EntityDeclaration, ParseError> Parser::parse_parameter_entity_declaration()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();

    Variant<ByteString, ExternalID, Empty> definition;
    // PEDecl ::= '<!ENTITY' S '%' S Name S PEDef S? '>'
    TRY(expect("<!ENTITY"sv));
    auto accept = accept_rule();

    TRY(skip_whitespace(Required::Yes));
    TRY(expect("%"sv));
    TRY(skip_whitespace(Required::Yes));
    auto name = TRY(parse_name());
    TRY(skip_whitespace(Required::Yes));
    // PEDef ::= EntityValue | ExternalID
    if (auto result = parse_entity_value(); !result.is_error())
        definition = result.release_value();
    else
        definition = TRY(parse_external_id());

    TRY(skip_whitespace());
    TRY(expect(">"sv));

    rollback.disarm();
    return PEDeclaration {
        move(name),
        move(definition).downcast<ByteString, ExternalID>(),
    };
}

// 4.7.83 PublicID, https://www.w3.org/TR/2006/REC-xml11-20060816/#NT-PublicID
ErrorOr<PublicID, ParseError> Parser::parse_public_id()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();

    // PublicID ::= 'PUBLIC' S PubidLiteral
    TRY(expect("PUBLIC"sv));
    auto accept = accept_rule();

    TRY(skip_whitespace(Required::Yes));
    auto text = TRY(parse_public_id_literal());

    rollback.disarm();
    return PublicID {
        text,
    };
}

constexpr static auto s_public_id_characters = set_to_search<StringSet("\x20\x0d\x0a-'()+,./:=?;!*#@$_%")>().unify(ranges_for_search<Range('a', 'z'), Range('A', 'Z'), Range('0', '9')>());

// 2.3.12, PubidLiteral, https://www.w3.org/TR/2006/REC-xml11-20060816/#NT-PubidLiteral
ErrorOr<StringView, ParseError> Parser::parse_public_id_literal()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();

    // PubidLiteral ::= '"' PubidChar* '"' | "'" (PubidChar - "'")* "'"
    auto quote = TRY(expect(is_any_of("'\""sv), "any of ' or \""sv));
    auto accept = accept_rule();

    auto id = TRY(expect_many(
        [q = quote[0]](auto x) {
            return (q == '\'' ? x != '\'' : true) && s_public_id_characters.contains(x);
        },
        "a PubidChar"sv,
        true));
    TRY(expect(quote));

    rollback.disarm();
    return id;
}

// 2.3.11 SystemLiteral, https://www.w3.org/TR/xml/#NT-SystemLiteral
ErrorOr<StringView, ParseError> Parser::parse_system_id_literal()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();

    // SystemLiteral ::= ('"' [^"]* '"') | ("'" [^']* "'")
    auto quote = TRY(expect(is_any_of("'\""sv), "any of ' or \""sv));
    auto accept = accept_rule();

    auto id = TRY(expect_many(is_not_any_of(quote), "not a quote"sv, true));
    TRY(expect(quote));

    rollback.disarm();
    return id;
}

// 4.2.75 ExternalID, https://www.w3.org/TR/xml/#NT-ExternalID
ErrorOr<ExternalID, ParseError> Parser::parse_external_id()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();

    // ExternalID ::= 'SYSTEM' S SystemLiteral
    //              | 'PUBLIC' S PubidLiteral S SystemLiteral
    Optional<PublicID> public_id;
    SystemID system_id;

    if (m_lexer.consume_specific("SYSTEM"sv)) {
        auto accept = accept_rule();
        TRY(skip_whitespace(Required::Yes));
        system_id = SystemID { TRY(parse_system_id_literal()) };
    } else {
        TRY(expect("PUBLIC"sv));
        auto accept = accept_rule();

        TRY(skip_whitespace(Required::Yes));
        public_id = PublicID { TRY(parse_public_id_literal()) };
        TRY(skip_whitespace(Required::Yes));
        system_id = SystemID { TRY(parse_system_id_literal()) };
    }

    rollback.disarm();
    return ExternalID {
        move(public_id),
        move(system_id),
    };
}

// 4.2.2.76 NDataDecl, https://www.w3.org/TR/xml/#NT-NDataDecl
ErrorOr<Name, ParseError> Parser::parse_notation_data_declaration()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();

    // NDataDecl ::= S 'NDATA' S Name
    TRY(skip_whitespace(Required::Yes));
    auto accept = accept_rule();

    TRY(expect("NDATA"sv));
    TRY(skip_whitespace(Required::Yes));
    auto name = TRY(parse_name());

    rollback.disarm();
    return name;
}

// 2.3.9 EntityValue, https://www.w3.org/TR/2006/REC-xml11-20060816/#NT-EntityValue
ErrorOr<ByteString, ParseError> Parser::parse_entity_value()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();
    StringBuilder builder;

    // EntityValue ::= '"' ([^%&"] | PEReference | Reference)* '"'
    //               |  "'" ([^%&'] | PEReference | Reference)* "'"
    auto quote = TRY(expect(is_any_of("'\""sv), "any of ' or \""sv));
    auto accept = accept_rule();

    while (true) {
        if (m_lexer.is_eof())
            break;
        if (m_lexer.next_is(quote))
            break;
        if (m_lexer.next_is('%')) {
            auto start = m_lexer.tell();
            // FIXME: Resolve this PEReference.
            TRY(parse_parameter_entity_reference());
            builder.append(m_source.substring_view(start, m_lexer.tell() - start));
            continue;
        }
        if (m_lexer.next_is('&')) {
            auto reference = TRY(parse_reference());
            if (auto char_reference = reference.get_pointer<ByteString>())
                builder.append(*char_reference);
            else
                builder.append(TRY(resolve_reference(reference.get<EntityReference>(), ReferencePlacement::AttributeValue)));
            continue;
        }
        builder.append(m_lexer.consume());
    }
    TRY(expect(quote));

    rollback.disarm();
    return builder.to_byte_string();
}

// 2.7.18 CDSect, https://www.w3.org/TR/2006/REC-xml11-20060816/#NT-CDSect
ErrorOr<StringView, ParseError> Parser::parse_cdata_section()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();

    // CDSect ::= CDStart CData CDEnd
    // CDStart ::= '<![CDATA['
    // CData ::= (Char* - (Char* ']]>' Char*))
    // CDEnd ::= ']]>'
    TRY(expect("<![CDATA["sv));
    auto accept = accept_rule();

    auto section_start = m_lexer.tell();
    while (!m_lexer.next_is("]]>")) {
        if (m_lexer.is_eof())
            break;
        m_lexer.ignore();
    }
    auto section_end = m_lexer.tell();
    TRY(expect("]]>"sv));

    rollback.disarm();
    return m_source.substring_view(section_start, section_end - section_start);
}

// 2.8.30 extSubset, https://www.w3.org/TR/2006/REC-xml11-20060816/#NT-extSubset
ErrorOr<Vector<MarkupDeclaration>, ParseError> Parser::parse_external_subset()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();

    // extSubset ::= TextDecl? extSubsetDecl
    (void)parse_text_declaration();
    auto result = TRY(parse_external_subset_declaration());

    rollback.disarm();
    return result;
}

// 4.3.1.77 TextDecl, https://www.w3.org/TR/2006/REC-xml11-20060816/#NT-TextDecl
ErrorOr<void, ParseError> Parser::parse_text_declaration()
{
    auto rollback = rollback_point();
    auto rule = enter_rule();

    // TextDecl ::= '<?xml' VersionInfo? EncodingDecl S? '?>'
    TRY(expect("<?xml"sv));
    auto accept = accept_rule();

    (void)parse_version_info();
    TRY(parse_encoding_decl());
    TRY(skip_whitespace());
    TRY(expect("?>"sv));

    rollback.disarm();
    return {};
}

ErrorOr<ByteString, ParseError> Parser::resolve_reference(EntityReference const& reference, ReferencePlacement placement)
{
    static HashTable<Name> reference_lookup {};
    if (reference_lookup.contains(reference.name))
        return parse_error(m_lexer.current_position(), ByteString::formatted("Invalid recursive definition for '{}'", reference.name));

    reference_lookup.set(reference.name);
    ScopeGuard remove_lookup {
        [&] {
            reference_lookup.remove(reference.name);
        }
    };

    Optional<ByteString> resolved;
    if (m_doctype.has_value()) {
        // FIXME: Split these up and resolve them ahead of time.
        for (auto& declaration : m_doctype->markup_declarations) {
            auto entity = declaration.get_pointer<EntityDeclaration>();
            if (!entity)
                continue;
            auto ge_declaration = entity->get_pointer<GEDeclaration>();
            if (!ge_declaration)
                continue;
            if (ge_declaration->name != reference.name)
                continue;
            TRY(ge_declaration->definition.visit(
                [&](ByteString const& definition) -> ErrorOr<void, ParseError> {
                    resolved = definition;
                    return {};
                },
                [&](EntityDefinition const& definition) -> ErrorOr<void, ParseError> {
                    if (placement == ReferencePlacement::AttributeValue)
                        return parse_error(m_lexer.current_position(), ByteString::formatted("Attribute references external entity '{}'", reference.name));

                    if (definition.notation.has_value())
                        return parse_error(m_lexer.position_for(0), ByteString::formatted("Entity reference to unparsed entity '{}'", reference.name));

                    if (!m_options.resolve_external_resource)
                        return parse_error(m_lexer.position_for(0), ByteString::formatted("Failed to resolve external entity '{}'", reference.name));

                    auto result = m_options.resolve_external_resource(definition.id.system_id, definition.id.public_id);
                    if (result.is_error())
                        return parse_error(m_lexer.position_for(0), ByteString::formatted("Failed to resolve external entity '{}': {}", reference.name, result.error()));

                    if (!result.value().has<ByteString>())
                        return parse_error(m_lexer.position_for(0), ByteString::formatted("Failed to resolve external entity '{}': Resource is of the wrong type", reference.name));

                    resolved = result.release_value().get<ByteString>();
                    return {};
                }));
            break;
        }
    }

    if (!resolved.has_value()) {
        if (reference.name == "amp")
            return "&";
        if (reference.name == "lt")
            return "<";
        if (reference.name == "gt")
            return ">";
        if (reference.name == "apos")
            return "'";
        if (reference.name == "quot")
            return "\"";
        return parse_error(m_lexer.position_for(0), ByteString::formatted("Reference to undeclared entity '{}'", reference.name));
    }

    StringView resolved_source = *resolved;
    TemporaryChange source { m_source, resolved_source };
    TemporaryChange lexer { m_lexer, LineTrackingLexer(m_source) };
    switch (placement) {
    case ReferencePlacement::AttributeValue:
        return TRY(parse_attribute_value_inner(""sv));
    case ReferencePlacement::Content:
        TRY(parse_content());
        return "";
    default:
        VERIFY_NOT_REACHED();
    }
}

}
