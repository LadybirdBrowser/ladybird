/*
 * Copyright (c) 2025, mikiubo
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "XMLFragmentParser.h"

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/XML/XMLDocumentBuilder.h>
#include <LibXML/Parser/Parser.h>

namespace Web {

// https://html.spec.whatwg.org/multipage/xhtml.html#parsing-xhtml-fragments
WebIDL::ExceptionOr<Vector<GC::Root<DOM::Node>>> XMLFragmentParser::parse_xml_fragment(DOM::Element& context, StringView input, HTML::HTMLParser::AllowDeclarativeShadowRoots allow_declarative_shadow_roots)
{
    // 1. Create a new XML parser.
    // The feed will be used to create the parser below
    StringBuilder feed;

    // 2. Feed the parser just created the string corresponding to the start tag of context,
    StringBuilder start_tag_builder;
    auto const& context_local_name = context.local_name();
    auto const& context_prefix = context.prefix();

    start_tag_builder.append('<');
    if (context_prefix.has_value() && !context_prefix->is_empty()) {
        start_tag_builder.append(context_prefix.value());
        start_tag_builder.append(':');
    }
    start_tag_builder.append(context_local_name);
    start_tag_builder.append(">"sv);
    feed.append(start_tag_builder.string_view());

    // FIXME
    //  declaring all the namespace prefixes that are in scope on that element in the DOM,

    // FIXME
    //  as well as declaring the default namespace (if any) that is in scope on that element in the DOM.
    //  A namespace prefix is in scope if the DOM lookupNamespaceURI() method on the element would return a non-null value for that prefix.
    //  The default namespace is the namespace for which the DOM isDefaultNamespace() method on the element would return true.

    // 3. Feed the parser just created the string input.
    feed.append(input);

    // 4. Feed the parser just created the string corresponding to the end tag of context.
    StringBuilder end_tag_builder;
    end_tag_builder.append("</"sv);
    if (context_prefix.has_value() && !context_prefix->is_empty()) {
        end_tag_builder.append(context_prefix.value());
        end_tag_builder.append(":"sv);
    }
    end_tag_builder.append(context_local_name);
    end_tag_builder.append(">"sv);
    feed.append(end_tag_builder.string_view());

    GC::Ptr<DOM::Document> document;
    document = DOM::Document::create(context.realm());
    document->set_document_type(DOM::Document::Type::XML);
    if (allow_declarative_shadow_roots == HTML::HTMLParser::AllowDeclarativeShadowRoots::Yes)
        document->set_allow_declarative_shadow_roots(true);

    XML::Parser parser(feed.string_view());
    XMLDocumentBuilder builder { *document, XMLScriptingSupport::Disabled };
    auto result = parser.parse_with_listener(builder);

    // 5. If there is an XML well-formedness or XML namespace well-formedness error, then throw a "SyntaxError" DOMException.
    if (result.is_error()) {
        return WebIDL::SyntaxError::create(context.realm(), Utf16String::formatted("{}", result.error()));
    }

    auto* doc_element = document->document_element();

    // 6. If the document element of the resulting Document has any sibling nodes, then throw a "SyntaxError" DOMException.
    if (doc_element->previous_sibling() || doc_element->next_sibling()) {
        return WebIDL::SyntaxError::create(context.realm(), "Document element has sibling nodes"_utf16);
    }

    // 7. Return the resulting Document node's document element's children, in tree order.
    Vector<GC::Root<DOM::Node>> result_nodes;
    for (auto* child = doc_element->first_child(); child; child = child->next_sibling()) {
        result_nodes.append(*child);
    }

    return result_nodes;
}

}
