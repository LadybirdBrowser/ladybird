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

static xmlSAXHandler create_sax_handler(bool preserve_comments)
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
    return handler;
}

ErrorOr<void, ParseError> Parser::parse_with_listener(Listener& listener)
{
    auto source_result = listener.set_source(ByteString(m_source));
    if (source_result.is_error())
        return ParseError { {}, ByteString("Failed to set source") };

    ParserContext context;
    context.listener = &listener;

    auto sax_handler = create_sax_handler(m_options.preserve_comments);

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

    auto sax_handler = create_sax_handler(m_options.preserve_comments);

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
