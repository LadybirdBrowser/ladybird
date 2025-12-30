/*
 * Copyright (c) 2025, mikiubo <michele.uboldi@gmail.com>
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
    // NB: The feed will be used to create the parser below
    StringBuilder feed;

    StringBuilder qualified_name_builder;
    if (auto const& prefix = context.prefix(); prefix.has_value() && !prefix->is_empty()) {
        qualified_name_builder.append(prefix.value());
        qualified_name_builder.append(':');
    }
    qualified_name_builder.append(context.local_name());
    auto const& qualified_name = qualified_name_builder.string_view();

    // 2. Feed the parser just created the string corresponding to the start tag of context,
    feed.append('<');
    feed.append(qualified_name);
    //  declaring all the namespace prefixes that are in scope on that element in the DOM,
    for (auto const& prefix : context.get_in_scope_prefixes()) {
        // NB: Skipping the empty prefix because it is handled specially
        // and the "xmlns" prefix because it is illegal to declare.
        if (prefix.is_empty() || prefix == "xmlns"_fly_string)
            continue;

        auto namespace_uri = context.lookup_namespace_uri(prefix.to_string()).value();
        VERIFY(!namespace_uri.is_empty());

        feed.append(" xmlns:"sv);
        feed.append(prefix);
        feed.append("=\""sv);
        feed.append(namespace_uri);
        feed.append('"');
    }
    //  as well as declaring the default namespace (if any) that is in scope on that element in the DOM.
    auto default_namespace = context.locate_a_namespace({});
    if (default_namespace.has_value() && !default_namespace->is_empty()) {
        feed.append(" xmlns=\""sv);
        feed.append(default_namespace.value());
        feed.append('"');
    }
    //  A namespace prefix is in scope if the DOM lookupNamespaceURI() method on the element would return a non-null value for that prefix.
    //  The default namespace is the namespace for which the DOM isDefaultNamespace() method on the element would return true.
    feed.append('>');

    // 3. Feed the parser just created the string input.
    feed.append(input);

    // 4. Feed the parser just created the string corresponding to the end tag of context.
    feed.append("</"sv);
    feed.append(qualified_name);
    feed.append(">"sv);

    GC::Ptr<DOM::Document> document = DOM::Document::create(context.realm());
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
