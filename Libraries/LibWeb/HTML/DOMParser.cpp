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
#include <LibWeb/TrustedTypes/RequireTrustedTypesForDirective.h>
#include <LibWeb/TrustedTypes/TrustedTypePolicy.h>
#include <LibWeb/XML/XMLDocumentBuilder.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(DOMParser);

WebIDL::ExceptionOr<GC::Ref<DOMParser>> DOMParser::construct_impl(JS::Realm& realm)
{
    return realm.create<DOMParser>(realm);
}

DOMParser::DOMParser(JS::Realm& realm)
    : PlatformObject(realm)
{
}

DOMParser::~DOMParser() = default;

void DOMParser::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(DOMParser);
    Base::initialize(realm);
}

// https://html.spec.whatwg.org/multipage/dynamic-markup-insertion.html#dom-domparser-parsefromstring
WebIDL::ExceptionOr<GC::Root<DOM::Document>> DOMParser::parse_from_string(Utf16String string, Bindings::DOMParserSupportedType type)
{
    // 1. Let compliantString to the result of invoking the Get Trusted Type compliant string algorithm with
    //    TrustedHTML, this's relevant global object, string, "DOMParser parseFromString", and "script".
    auto const compliant_string = TRY(TrustedTypes::get_trusted_type_compliant_string(
        TrustedTypes::TrustedTypeName::TrustedHTML,
        relevant_global_object(*this),
        move(string),
        TrustedTypes::InjectionSink::DOMParser_parseFromString,
        TrustedTypes::Script.to_string()));

    // 2. Let document be a new Document, whose content type is type and url is this's relevant global object's associated Document's URL.
    GC::Ptr<DOM::Document> document;
    auto& associated_document = as<HTML::Window>(relevant_global_object(*this)).associated_document();

    // 3. Switch on type:
    if (type == Bindings::DOMParserSupportedType::Text_Html) {
        // -> "text/html"
        document = HTML::HTMLDocument::create(realm(), associated_document.url());
        document->set_content_type(Bindings::idl_enum_to_string(type));
        document->set_document_type(DOM::Document::Type::HTML);

        // 1. Parse HTML from a string given document and compliantString.
        document->parse_html_from_a_string(compliant_string.to_utf8_but_should_be_ported_to_utf16());
    } else {
        // -> Otherwise
        document = DOM::Document::create(realm(), associated_document.url());
        document->set_content_type(Bindings::idl_enum_to_string(type));
        document->set_document_type(DOM::Document::Type::XML);

        // 1. Create an XML parser parse, associated with document, and with XML scripting support disabled.
        auto const utf8_complaint_string = compliant_string.to_utf8_but_should_be_ported_to_utf16();
        XML::Parser parser(utf8_complaint_string, { .resolve_external_resource = resolve_xml_resource });
        XMLDocumentBuilder builder { *document, XMLScriptingSupport::Disabled };
        // 2. Parse compliantString using parser.
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

    // AD-HOC: Setting the origin to match that of the associated document matches the behavior of existing browsers
    //         and avoids a crash, since we expect the origin to always be set.
    // Spec issue: https://github.com/whatwg/html/issues/11429
    document->set_origin(associated_document.origin());

    // 3. Return document.
    return document;
}

}
