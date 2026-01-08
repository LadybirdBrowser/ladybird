/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibXML/Parser/Parser.h>

#include <libxml/encoding.h>
#include <libxml/parser.h>
#include <libxml/parserInternals.h>
#include <libxml/xmlerror.h>

namespace XML {

static constexpr int MAX_XML_TREE_DEPTH = 5000;

struct ParserContext {
    Listener* listener { nullptr };
    Optional<ParseError> error;
    bool document_ended { false };

    OwnPtr<Node> root_node;
    Node* current_node { nullptr };
    Optional<Doctype> doctype;
    HashMap<Name, ByteString> processing_instructions;
    Version version { Version::Version11 };

    Vector<ParseError> parse_errors;

    Parser::Options const* options { nullptr };
    bool is_xhtml_document { false };
    int depth { 0 };
};

static ByteString xml_char_to_byte_string(xmlChar const* str)
{
    if (!str)
        return {};
    return ByteString(reinterpret_cast<char const*>(str));
}

static ByteString xml_char_to_byte_string(xmlChar const* str, int len)
{
    if (!str || len <= 0)
        return {};
    return ByteString(StringView(reinterpret_cast<char const*>(str), static_cast<size_t>(len)));
}

static StringView xml_char_to_string_view(xmlChar const* str)
{
    if (!str)
        return {};
    return StringView(reinterpret_cast<char const*>(str), strlen(reinterpret_cast<char const*>(str)));
}

static bool is_known_xhtml_public_id(StringView public_id)
{
    return public_id.is_one_of(
        "-//W3C//DTD XHTML 1.0 Transitional//EN"sv,
        "-//W3C//DTD XHTML 1.1//EN"sv,
        "-//W3C//DTD XHTML 1.0 Strict//EN"sv,
        "-//W3C//DTD XHTML 1.0 Frameset//EN"sv,
        "-//W3C//DTD XHTML Basic 1.0//EN"sv,
        "-//W3C//DTD XHTML 1.1 plus MathML 2.0//EN"sv,
        "-//W3C//DTD XHTML 1.1 plus MathML 2.0 plus SVG 1.1//EN"sv,
        "-//W3C//DTD MathML 2.0//EN"sv,
        "-//WAPFORUM//DTD XHTML Mobile 1.0//EN"sv,
        "-//WAPFORUM//DTD XHTML Mobile 1.1//EN"sv,
        "-//WAPFORUM//DTD XHTML Mobile 1.2//EN"sv);
}

static void external_subset_handler(void* ctx, xmlChar const*, xmlChar const* external_id, xmlChar const*)
{
    auto* parser_ctx = static_cast<xmlParserCtxtPtr>(ctx);
    auto* context = static_cast<ParserContext*>(parser_ctx->_private);
    if (!context || !external_id)
        return;

    auto public_id = xml_char_to_string_view(external_id);
    if (is_known_xhtml_public_id(public_id))
        context->is_xhtml_document = true;
}

static xmlEntity s_xhtml_entity_result;
static char s_xhtml_entity_utf8_buffer[32];

static xmlEntityPtr get_entity_handler(void* ctx, xmlChar const* name)
{
    auto* parser_ctx = static_cast<xmlParserCtxtPtr>(ctx);

    auto* predefined = xmlGetPredefinedEntity(name);
    if (predefined)
        return predefined;

    if (parser_ctx->myDoc) {
        auto* doc_entity = xmlGetDocEntity(parser_ctx->myDoc, name);
        if (doc_entity)
            return doc_entity;
    }

    auto* context = static_cast<ParserContext*>(parser_ctx->_private);
    if (!context || !context->is_xhtml_document)
        return nullptr;

    // For XHTML documents, resolve named character entities (e.g., &nbsp;) using the
    // HTML entity table. This avoids parsing a large embedded DTD on every document
    // and matches the approach used by Blink and WebKit.
    if (!context->options || !context->options->resolve_named_html_entity)
        return nullptr;

    auto entity_name = xml_char_to_string_view(name);
    auto resolved = context->options->resolve_named_html_entity(entity_name);
    if (!resolved.has_value())
        return nullptr;

    auto utf8_bytes = resolved->bytes_as_string_view();
    if (utf8_bytes.length() >= sizeof(s_xhtml_entity_utf8_buffer))
        return nullptr;

    (void)utf8_bytes.copy_characters_to_buffer(s_xhtml_entity_utf8_buffer, sizeof(s_xhtml_entity_utf8_buffer));

    s_xhtml_entity_result = {};
    s_xhtml_entity_result.type = XML_ENTITY_DECL;
    s_xhtml_entity_result.name = name;
    s_xhtml_entity_result.content = reinterpret_cast<xmlChar*>(s_xhtml_entity_utf8_buffer);
    s_xhtml_entity_result.length = static_cast<int>(utf8_bytes.length());
    s_xhtml_entity_result.etype = XML_INTERNAL_PREDEFINED_ENTITY;

    return &s_xhtml_entity_result;
}

static void start_document_handler(void* ctx)
{
    auto* parser_ctx = static_cast<xmlParserCtxtPtr>(ctx);
    auto* context = static_cast<ParserContext*>(parser_ctx->_private);
    if (!context)
        return;

    if (parser_ctx->version) {
        auto version_str = xml_char_to_byte_string(parser_ctx->version);
        if (version_str == "1.0"sv)
            context->version = Version::Version10;
        else
            context->version = Version::Version11;
    }

    if (context->listener)
        context->listener->document_start();
}

static void end_document_handler(void* ctx)
{
    auto* parser_ctx = static_cast<xmlParserCtxtPtr>(ctx);
    auto* context = static_cast<ParserContext*>(parser_ctx->_private);
    if (!context)
        return;

    context->document_ended = true;
    if (context->listener)
        context->listener->document_end();
}

static void start_element_ns_handler(void* ctx, xmlChar const* localname, xmlChar const* prefix,
    xmlChar const*, int nb_namespaces, xmlChar const** namespaces,
    int nb_attributes, int nb_defaulted, xmlChar const** attributes)
{
    (void)nb_defaulted;

    auto* parser_ctx = static_cast<xmlParserCtxtPtr>(ctx);
    auto* context = static_cast<ParserContext*>(parser_ctx->_private);
    if (!context)
        return;

    if (++context->depth > MAX_XML_TREE_DEPTH) {
        size_t offset = 0;
        if (parser_ctx->input && parser_ctx->input->cur && parser_ctx->input->base)
            offset = static_cast<size_t>(parser_ctx->input->cur - parser_ctx->input->base);

        ParseError parse_error {
            .position = LineTrackingLexer::Position { .offset = offset },
            .error = ByteString("Excessive node nesting."sv),
        };
        context->parse_errors.append(parse_error);

        if (context->listener)
            context->listener->error(parse_error);

        xmlStopParser(parser_ctx);
        return;
    }

    StringBuilder name_builder;
    if (prefix) {
        name_builder.append(xml_char_to_string_view(prefix));
        name_builder.append(':');
    }
    name_builder.append(xml_char_to_string_view(localname));
    auto name = name_builder.to_byte_string();

    OrderedHashMap<Name, ByteString> attrs;

    for (int i = 0; i < nb_namespaces; i++) {
        auto* ns_prefix = namespaces[i * 2];
        auto* ns_uri = namespaces[i * 2 + 1];

        StringBuilder attr_name;
        if (ns_prefix) {
            attr_name.append("xmlns:"sv);
            attr_name.append(xml_char_to_string_view(ns_prefix));
        } else {
            attr_name.append("xmlns"sv);
        }
        attrs.set(attr_name.to_byte_string(), xml_char_to_byte_string(ns_uri));
    }

    for (int i = 0; i < nb_attributes; i++) {
        auto* attr_localname = attributes[i * 5 + 0];
        auto* attr_prefix = attributes[i * 5 + 1];
        auto* value_begin = attributes[i * 5 + 3];
        auto* value_end = attributes[i * 5 + 4];

        StringBuilder attr_name;
        if (attr_prefix) {
            attr_name.append(xml_char_to_string_view(attr_prefix));
            attr_name.append(':');
        }
        attr_name.append(xml_char_to_string_view(attr_localname));

        auto value_len = static_cast<int>(value_end - value_begin);
        auto value = xml_char_to_byte_string(value_begin, value_len);
        attrs.set(attr_name.to_byte_string(), value);
    }

    if (context->listener) {
        context->listener->element_start(name, attrs);
    } else {
        auto element = adopt_own(*new Node {
            .offset = {},
            .content = Node::Element { name, move(attrs), {} },
            .parent = context->current_node,
        });

        auto* element_ptr = element.ptr();

        if (context->current_node) {
            VERIFY(context->current_node->is_element());
            context->current_node->content.get<Node::Element>().children.append(move(element));
        } else {
            context->root_node = move(element);
        }

        context->current_node = element_ptr;
    }
}

static void end_element_ns_handler(void* ctx, xmlChar const* localname, xmlChar const* prefix, xmlChar const*)
{
    auto* parser_ctx = static_cast<xmlParserCtxtPtr>(ctx);
    auto* context = static_cast<ParserContext*>(parser_ctx->_private);
    if (!context)
        return;

    --context->depth;

    StringBuilder name_builder;
    if (prefix) {
        name_builder.append(xml_char_to_string_view(prefix));
        name_builder.append(':');
    }
    name_builder.append(xml_char_to_string_view(localname));
    auto name = name_builder.to_byte_string();

    if (context->listener) {
        context->listener->element_end(name);
    } else if (context->current_node) {
        context->current_node = context->current_node->parent;
    }
}

static void characters_handler(void* ctx, xmlChar const* ch, int len)
{
    auto* parser_ctx = static_cast<xmlParserCtxtPtr>(ctx);
    auto* context = static_cast<ParserContext*>(parser_ctx->_private);
    if (!context)
        return;

    auto text = StringView(reinterpret_cast<char const*>(ch), static_cast<size_t>(len));

    if (context->listener) {
        context->listener->text(text);
    } else if (context->current_node && context->current_node->is_element()) {
        auto& children = context->current_node->content.get<Node::Element>().children;
        if (!children.is_empty() && children.last()->is_text()) {
            children.last()->content.get<Node::Text>().builder.append(text);
        } else {
            Node::Text text_content;
            text_content.builder.append(text);
            auto text_node = adopt_own(*new Node {
                .offset = {},
                .content = move(text_content),
                .parent = context->current_node,
            });
            children.append(move(text_node));
        }
    }
}

static void cdata_block_handler(void* ctx, xmlChar const* value, int len)
{
    auto* parser_ctx = static_cast<xmlParserCtxtPtr>(ctx);
    auto* context = static_cast<ParserContext*>(parser_ctx->_private);
    if (!context)
        return;

    auto text = StringView(reinterpret_cast<char const*>(value), static_cast<size_t>(len));

    if (context->listener) {
        context->listener->cdata_section(text);
    } else if (context->current_node && context->current_node->is_element()) {
        auto& children = context->current_node->content.get<Node::Element>().children;
        Node::Text text_content;
        text_content.builder.append(text);
        auto text_node = adopt_own(*new Node {
            .offset = {},
            .content = move(text_content),
            .parent = context->current_node,
        });
        children.append(move(text_node));
    }
}

static void comment_handler(void* ctx, xmlChar const* value)
{
    auto* parser_ctx = static_cast<xmlParserCtxtPtr>(ctx);
    auto* context = static_cast<ParserContext*>(parser_ctx->_private);
    if (!context)
        return;

    auto comment_text = xml_char_to_byte_string(value);

    if (context->listener) {
        context->listener->comment(comment_text);
    } else if (context->current_node && context->current_node->is_element()) {
        auto& children = context->current_node->content.get<Node::Element>().children;
        auto comment_node = adopt_own(*new Node {
            .offset = {},
            .content = Node::Comment { comment_text },
            .parent = context->current_node,
        });
        children.append(move(comment_node));
    }
}

static void processing_instruction_handler(void* ctx, xmlChar const* target, xmlChar const* data)
{
    auto* parser_ctx = static_cast<xmlParserCtxtPtr>(ctx);
    auto* context = static_cast<ParserContext*>(parser_ctx->_private);
    if (!context)
        return;

    auto target_str = xml_char_to_byte_string(target);
    auto data_str = xml_char_to_byte_string(data);

    if (context->listener) {
        context->listener->processing_instruction(target_str, data_str);
    } else {
        context->processing_instructions.set(target_str, data_str);
    }
}

static void internal_subset_handler(void* ctx, xmlChar const* name, xmlChar const* external_id, xmlChar const* system_id)
{
    auto* parser_ctx = static_cast<xmlParserCtxtPtr>(ctx);
    auto* context = static_cast<ParserContext*>(parser_ctx->_private);
    if (!context)
        return;

    Doctype doctype;
    doctype.type = xml_char_to_byte_string(name);

    if (external_id || system_id) {
        ExternalID ext_id;
        if (external_id)
            ext_id.public_id = PublicID { xml_char_to_byte_string(external_id) };
        ext_id.system_id = SystemID { xml_char_to_byte_string(system_id) };
        doctype.external_id = move(ext_id);
    }

    context->doctype = move(doctype);

    if (context->listener)
        context->listener->set_doctype(context->doctype.value());
}

static void structured_error_handler(void* ctx, xmlError const* error)
{
    auto* parser_ctx = static_cast<xmlParserCtxtPtr>(ctx);
    auto* context = static_cast<ParserContext*>(parser_ctx->_private);
    if (!context || !error)
        return;

    size_t offset = 0;
    if (parser_ctx->input && parser_ctx->input->cur && parser_ctx->input->base)
        offset = static_cast<size_t>(parser_ctx->input->cur - parser_ctx->input->base);

    ParseError parse_error {
        .position = LineTrackingLexer::Position {
            .offset = offset,
            .line = error->line > 0 ? static_cast<size_t>(error->line) : 0,
            .column = error->int2 > 0 ? static_cast<size_t>(error->int2) : 0,
        },
        .error = ByteString(error->message ? StringView(error->message, strlen(error->message)).trim_whitespace() : "Unknown error"sv),
    };

    context->parse_errors.append(parse_error);

    if (context->listener)
        context->listener->error(parse_error);

    if (!context->error.has_value())
        context->error = move(parse_error);
}

static xmlSAXHandler create_sax_handler(bool preserve_comments, bool resolve_html_entities)
{
    xmlSAXHandler handler = {};
    handler.initialized = XML_SAX2_MAGIC;
    handler.startDocument = start_document_handler;
    handler.endDocument = end_document_handler;
    handler.startElementNs = start_element_ns_handler;
    handler.endElementNs = end_element_ns_handler;
    handler.characters = characters_handler;
    handler.cdataBlock = cdata_block_handler;
    handler.processingInstruction = processing_instruction_handler;
    handler.internalSubset = internal_subset_handler;
    handler.serror = structured_error_handler;
    if (preserve_comments)
        handler.comment = comment_handler;
    if (resolve_html_entities) {
        handler.externalSubset = external_subset_handler;
        handler.getEntity = get_entity_handler;
    }
    return handler;
}

ErrorOr<void, ParseError> Parser::parse_with_listener(Listener& listener)
{
    auto source_result = listener.set_source(ByteString(m_source));
    if (source_result.is_error())
        return ParseError { {}, ByteString("Failed to set source") };

    ParserContext context;
    context.listener = &listener;
    context.options = &m_options;

    bool resolve_html_entities = static_cast<bool>(m_options.resolve_named_html_entity);
    auto sax_handler = create_sax_handler(m_options.preserve_comments, resolve_html_entities);

    int options = XML_PARSE_NONET | XML_PARSE_NOWARNING;
    if (!m_options.preserve_cdata)
        options |= XML_PARSE_NOCDATA;

    auto* parser_ctx = xmlCreatePushParserCtxt(&sax_handler, nullptr, nullptr, 0, nullptr);
    if (!parser_ctx)
        return ParseError { {}, ByteString("Failed to create parser context") };

    parser_ctx->_private = &context;
    xmlCtxtUseOptions(parser_ctx, options);

    xmlSwitchEncoding(parser_ctx, XML_CHAR_ENCODING_UTF8);

    auto result = xmlParseChunk(parser_ctx, m_source.characters_without_null_termination(), static_cast<int>(m_source.length()), 1);

    bool well_formed = parser_ctx->wellFormed;
    xmlFreeParserCtxt(parser_ctx);

    m_parse_errors = move(context.parse_errors);

    if (!context.document_ended)
        listener.document_end();

    if (context.error.has_value() && m_options.treat_errors_as_fatal)
        return context.error.release_value();

    if (result != 0 || !well_formed) {
        if (!m_parse_errors.is_empty())
            return m_parse_errors.first();
        return ParseError { {}, ByteString("XML parsing failed") };
    }

    return {};
}

ErrorOr<Document, ParseError> Parser::parse()
{
    ParserContext context;
    context.options = &m_options;

    bool resolve_html_entities = static_cast<bool>(m_options.resolve_named_html_entity);
    auto sax_handler = create_sax_handler(m_options.preserve_comments, resolve_html_entities);

    int options = XML_PARSE_NONET | XML_PARSE_NOWARNING;
    if (!m_options.preserve_cdata)
        options |= XML_PARSE_NOCDATA;

    auto* parser_ctx = xmlCreatePushParserCtxt(&sax_handler, nullptr, nullptr, 0, nullptr);
    if (!parser_ctx)
        return ParseError { {}, ByteString("Failed to create parser context") };

    parser_ctx->_private = &context;
    xmlCtxtUseOptions(parser_ctx, options);

    xmlSwitchEncoding(parser_ctx, XML_CHAR_ENCODING_UTF8);

    auto result = xmlParseChunk(parser_ctx, m_source.characters_without_null_termination(), static_cast<int>(m_source.length()), 1);

    bool well_formed = parser_ctx->wellFormed;
    xmlFreeParserCtxt(parser_ctx);

    m_parse_errors = move(context.parse_errors);

    if (context.error.has_value() && m_options.treat_errors_as_fatal)
        return context.error.release_value();

    if (result != 0 || !well_formed) {
        if (!m_parse_errors.is_empty())
            return m_parse_errors.first();
        return ParseError { {}, ByteString("XML parsing failed") };
    }

    if (!context.root_node)
        return ParseError { {}, ByteString("No root element") };

    return Document(context.root_node.release_nonnull(), move(context.doctype), move(context.processing_instructions), context.version);
}

}
