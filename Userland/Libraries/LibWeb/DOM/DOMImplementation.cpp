/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/DOMImplementationPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/DOM/DOMImplementation.h>
#include <LibWeb/DOM/DocumentType.h>
#include <LibWeb/DOM/ElementFactory.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/DOM/XMLDocument.h>
#include <LibWeb/HTML/HTMLDocument.h>
#include <LibWeb/HTML/Origin.h>
#include <LibWeb/Namespace.h>

namespace Web::DOM {

JS_DEFINE_ALLOCATOR(DOMImplementation);

JS::NonnullGCPtr<DOMImplementation> DOMImplementation::create(Document& document)
{
    auto& realm = document.realm();
    return realm.heap().allocate<DOMImplementation>(realm, document);
}

DOMImplementation::DOMImplementation(Document& document)
    : PlatformObject(document.realm())
    , m_document(document)
{
}

DOMImplementation::~DOMImplementation() = default;

void DOMImplementation::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(DOMImplementation);
}

void DOMImplementation::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
}

// https://dom.spec.whatwg.org/#dom-domimplementation-createdocument
WebIDL::ExceptionOr<JS::NonnullGCPtr<XMLDocument>> DOMImplementation::create_document(Optional<FlyString> const& namespace_, String const& qualified_name, JS::GCPtr<DocumentType> doctype) const
{
    // 1. Let document be a new XMLDocument
    auto xml_document = XMLDocument::create(realm());

    xml_document->set_ready_for_post_load_tasks(true);

    // 2. Let element be null.
    JS::GCPtr<Element> element;

    // 3. If qualifiedName is not the empty string, then set element to the result of running the internal createElementNS steps, given document, namespace, qualifiedName, and an empty dictionary.
    if (!qualified_name.is_empty())
        element = TRY(xml_document->create_element_ns(namespace_, qualified_name, ElementCreationOptions {}));

    // 4. If doctype is non-null, append doctype to document.
    if (doctype)
        TRY(xml_document->append_child(*doctype));

    // 5. If element is non-null, append element to document.
    if (element)
        TRY(xml_document->append_child(*element));

    // 6. document’s origin is this’s associated document’s origin.
    xml_document->set_origin(document().origin());

    // 7. document’s content type is determined by namespace:
    if (namespace_ == Namespace::HTML) {
        // -> HTML namespace
        xml_document->set_content_type("application/xhtml+xml"_string);
    } else if (namespace_ == Namespace::SVG) {
        // -> SVG namespace
        xml_document->set_content_type("image/svg+xml"_string);
    } else {
        // -> Any other namespace
        xml_document->set_content_type("application/xml"_string);
    }

    // 8. Return document.
    return xml_document;
}

// https://dom.spec.whatwg.org/#dom-domimplementation-createhtmldocument
JS::NonnullGCPtr<Document> DOMImplementation::create_html_document(Optional<String> const& title) const
{
    // 1. Let doc be a new document that is an HTML document.
    auto html_document = HTML::HTMLDocument::create(realm());

    // 2. Set doc’s content type to "text/html".
    html_document->set_content_type("text/html"_string);
    html_document->set_document_type(DOM::Document::Type::HTML);

    html_document->set_ready_for_post_load_tasks(true);

    // 3. Append a new doctype, with "html" as its name and with its node document set to doc, to doc.
    auto doctype = heap().allocate<DocumentType>(realm(), html_document);
    doctype->set_name("html"_string);
    MUST(html_document->append_child(*doctype));

    // 4. Append the result of creating an element given doc, html, and the HTML namespace, to doc.
    auto html_element = create_element(html_document, HTML::TagNames::html, Namespace::HTML).release_value_but_fixme_should_propagate_errors();
    MUST(html_document->append_child(html_element));

    // 5. Append the result of creating an element given doc, head, and the HTML namespace, to the html element created earlier.
    auto head_element = create_element(html_document, HTML::TagNames::head, Namespace::HTML).release_value_but_fixme_should_propagate_errors();
    MUST(html_element->append_child(head_element));

    // 6. If title is given:
    if (title.has_value()) {
        // 1. Append the result of creating an element given doc, title, and the HTML namespace, to the head element created earlier.
        auto title_element = create_element(html_document, HTML::TagNames::title, Namespace::HTML).release_value_but_fixme_should_propagate_errors();
        MUST(head_element->append_child(title_element));

        // 2. Append a new Text node, with its data set to title (which could be the empty string) and its node document set to doc, to the title element created earlier.
        auto text_node = heap().allocate<Text>(realm(), html_document, title.value());
        MUST(title_element->append_child(*text_node));
    }

    // 7. Append the result of creating an element given doc, body, and the HTML namespace, to the html element created earlier.
    auto body_element = create_element(html_document, HTML::TagNames::body, Namespace::HTML).release_value_but_fixme_should_propagate_errors();
    MUST(html_element->append_child(body_element));

    // 8. doc’s origin is this’s associated document’s origin.
    html_document->set_origin(document().origin());

    // 9. Return doc.
    return html_document;
}

// https://dom.spec.whatwg.org/#dom-domimplementation-createdocumenttype
WebIDL::ExceptionOr<JS::NonnullGCPtr<DocumentType>> DOMImplementation::create_document_type(String const& qualified_name, String const& public_id, String const& system_id)
{
    // 1. Validate qualifiedName.
    TRY(Document::validate_qualified_name(realm(), qualified_name));

    // 2. Return a new doctype, with qualifiedName as its name, publicId as its public ID, and systemId as its system ID, and with its node document set to the associated document of this.
    auto document_type = DocumentType::create(document());
    document_type->set_name(qualified_name);
    document_type->set_public_id(public_id);
    document_type->set_system_id(system_id);
    return document_type;
}

}
