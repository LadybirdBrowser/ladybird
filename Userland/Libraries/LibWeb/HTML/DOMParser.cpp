/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/DOMParserPrototype.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/DOM/XMLDocument.h>
#include <LibWeb/HTML/DOMParser.h>
#include <LibWeb/HTML/HTMLDocument.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/XML/XMLDocumentBuilder.h>

namespace Web::HTML {

JS_DEFINE_ALLOCATOR(DOMParser);

WebIDL::ExceptionOr<JS::NonnullGCPtr<DOMParser>> DOMParser::construct_impl(JS::Realm& realm)
{
    return realm.heap().allocate<DOMParser>(realm, realm);
}

DOMParser::DOMParser(JS::Realm& realm)
    : PlatformObject(realm)
{
}

DOMParser::~DOMParser() = default;

void DOMParser::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(DOMParser);
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-domparser-parsefromstring
JS::NonnullGCPtr<DOM::Document> DOMParser::parse_from_string(StringView string, Bindings::DOMParserSupportedType type)
{
    // FIXME: 1. Let compliantString to the result of invoking the Get Trusted Type compliant string algorithm with TrustedHTML, this's relevant global object, string, "DOMParser parseFromString", and "script".

    // 2. Let document be a new Document, whose content type is type and url is this's relevant global object's associated Document's URL.
    JS::GCPtr<DOM::Document> document;

    // 3. Switch on type:
    if (type == Bindings::DOMParserSupportedType::Text_Html) {
        // -> "text/html"
        document = HTML::HTMLDocument::create(realm(), verify_cast<HTML::Window>(relevant_global_object(*this)).associated_document().url());
        document->set_content_type(Bindings::idl_enum_to_string(type));

        // 1. Parse HTML from a string given document and compliantString. FIXME: Use compliantString.
        document->parse_html_from_a_string(string);
    } else {
        // -> Otherwise
        document = DOM::XMLDocument::create(realm(), verify_cast<HTML::Window>(relevant_global_object(*this)).associated_document().url());
        document->set_content_type(Bindings::idl_enum_to_string(type));
        document->set_document_type(DOM::Document::Type::XML);

        // 1. Create an XML parser parse, associated with document, and with XML scripting support disabled.
        XML::Parser parser(string, { .resolve_external_resource = resolve_xml_resource });
        XMLDocumentBuilder builder { *document, XMLScriptingSupport::Disabled };
        // 2. Parse compliantString using parser. FIXME: Use compliantString.
        auto result = parser.parse_with_listener(builder);
        // 3. If the previous step resulted in an XML well-formedness or XML namespace well-formedness error, then:
        if (result.is_error() || builder.has_error()) {
            // NOTE: The XML parsing can produce nodes before it hits an error, just remove them.
            // 1. Assert: document has no child nodes.
            document->remove_all_children(true);
            // 2. Let root be the result of creating an element given document, "parsererror", and "http://www.mozilla.org/newlayout/xml/parsererror.xml".
            auto root = DOM::create_element(*document, "parsererror"_fly_string, "http://www.mozilla.org/newlayout/xml/parsererror.xml"_fly_string).release_value_but_fixme_should_propagate_errors();
            // FIXME: 3. Optionally, add attributes or children to root to describe the nature of the parsing error.
            // 4. Append root to document.
            MUST(document->append_child(*root));
        }
    }

    // 3. Return document.
    return *document;
}

}
