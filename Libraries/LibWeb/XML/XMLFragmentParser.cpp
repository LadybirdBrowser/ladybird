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
WebIDL::ExceptionOr<GC::Ref<DOM::DocumentFragment>> XMLFragmentParser::parse_xml_fragment(Variant<GC::Ref<DOM::Element>, GC::Ref<DOM::DocumentFragment>> target, Utf16View input)
{
    // 1. Let context be target if target is an Element; otherwise target's host.
    auto* context = target.has<GC::Ref<DOM::Element>>()
        ? target.get<GC::Ref<DOM::Element>>().ptr()
        : target.get<GC::Ref<DOM::DocumentFragment>>()->host();

    // 2. Assert: context is non-null.
    VERIFY(context);

    // 3. Create a new XML parser.
    // NB: The feed will be used to create the parser below
    StringBuilder feed;

    StringBuilder qualified_name_builder;
    if (auto const& prefix = context->prefix(); prefix.has_value() && !prefix->is_empty()) {
        qualified_name_builder.append(prefix.value());
        qualified_name_builder.append(':');
    }
    qualified_name_builder.append(context->local_name());
    auto const& qualified_name = qualified_name_builder.string_view();

    // 4. Feed the parser just created the string corresponding to the start tag of context,
    feed.append('<');
    feed.append(qualified_name);
    //  declaring all the namespace prefixes that are in scope on that element in the DOM,
    for (auto const& prefix : context->get_in_scope_prefixes()) {
        // NB: Skipping the empty prefix because it is handled specially
        // and the "xmlns" prefix because it is illegal to declare.
        if (prefix.is_empty() || prefix == "xmlns"sv)
            continue;

        auto namespace_uri = context->lookup_namespace_uri(prefix.view()).value();
        VERIFY(!namespace_uri.is_empty());

        feed.append(" xmlns:"sv);
        feed.append(prefix.view());
        feed.append("=\""sv);
        feed.append(namespace_uri);
        feed.append('"');
    }
    //  as well as declaring the default namespace (if any) that is in scope on that element in the DOM.
    auto default_namespace = context->locate_a_namespace({});
    if (default_namespace.has_value() && !default_namespace->is_empty()) {
        feed.append(" xmlns=\""sv);
        feed.append(default_namespace->utf16_view());
        feed.append('"');
    }
    //  A namespace prefix is in scope if the DOM lookupNamespaceURI() method on the element would return a non-null value for that prefix.
    //  The default namespace is the namespace for which the DOM isDefaultNamespace() method on the element would return true.
    feed.append('>');

    // 5. Feed the parser just created the string input.
    feed.append(input);

    // 6. Feed the parser just created the string corresponding to the end tag of context.
    feed.append("</"sv);
    feed.append(qualified_name);
    feed.append(">"sv);

    GC::Ptr<DOM::Document> document = DOM::Document::create(context->realm());
    document->set_document_type(DOM::Document::Type::XML);

    XML::Parser parser(feed.string_view(), { .resolve_named_html_entity = resolve_named_html_entity });
    XMLDocumentBuilder builder { *document, XMLScriptingSupport::Disabled };
    auto result = parser.parse_with_listener(builder);

    // 7. If there is an XML well-formedness or XML namespace well-formedness error, then throw a "SyntaxError" DOMException.
    if (result.is_error()) {
        return WebIDL::SyntaxError::create(context->realm(), Utf16String::formatted("{}", result.error()));
    }

    auto* doc_element = document->document_element();

    // 8. If the document element of the resulting Document has any sibling nodes, then throw a "SyntaxError" DOMException.
    if (doc_element->previous_sibling() || doc_element->next_sibling()) {
        return WebIDL::SyntaxError::create(context->realm(), "Document element has sibling nodes"_utf16);
    }

    // 9. Let newChildren be the resulting Document node's document element's children, in tree order.
    // 10. Let fragment be a new DocumentFragment whose node document is context's node document.
    auto fragment = context->realm().create<DOM::DocumentFragment>(context->document());

    // 11. For each node of newChildren, in tree order: append node to fragment.
    for (auto* child = doc_element->first_child(); child;) {
        // append_child() moves child out of doc_element, so keep the next sibling first.
        auto* next_child = child->next_sibling();
        TRY(fragment->append_child(*child));
        child = next_child;
    }

    // 12. Return fragment.
    return fragment;
}

}
