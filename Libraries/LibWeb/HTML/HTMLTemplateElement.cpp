/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLTemplateElementPrototype.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLTemplateElement.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLTemplateElement);

HTMLTemplateElement::HTMLTemplateElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLTemplateElement::~HTMLTemplateElement() = default;

void HTMLTemplateElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLTemplateElement);

    m_content = realm.create<DOM::DocumentFragment>(m_document->appropriate_template_contents_owner_document());
    m_content->set_host(this);
}

void HTMLTemplateElement::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_content);
}

// https://html.spec.whatwg.org/multipage/scripting.html#the-template-element:concept-node-adopt-ext
void HTMLTemplateElement::adopted_from(DOM::Document&)
{
    // 1. Let doc be node's node document's appropriate template contents owner document.
    auto doc = document().appropriate_template_contents_owner_document();

    // 2. Adopt node's template contents (a DocumentFragment object) into doc.
    doc->adopt_node(content());
}

// https://html.spec.whatwg.org/multipage/scripting.html#the-template-element:concept-node-clone-ext
WebIDL::ExceptionOr<void> HTMLTemplateElement::cloned(Node& copy, bool subtree) const
{
    TRY(Base::cloned(copy, subtree));

    // The cloning steps for template elements given node, copy, and subtree are:

    // 1. If subtree is false, then return.
    if (!subtree)
        return {};

    // 2. For each child of node's template contents's children, in tree order:
    //    clone a node given child with document set to copy's template contents's node document,
    //    subtree set to true, and parent set to copy's template contents.
    auto& template_copy = as<HTMLTemplateElement>(copy);
    for (auto child = content()->first_child(); child; child = child->next_sibling()) {
        TRY(child->clone_node(&template_copy.content()->document(), true, template_copy.content()));
    }

    return {};
}

void HTMLTemplateElement::set_template_contents(GC::Ref<DOM::DocumentFragment> contents)
{
    m_content = contents;
}

}
