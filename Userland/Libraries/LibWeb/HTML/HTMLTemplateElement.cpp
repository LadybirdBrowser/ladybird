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

JS_DEFINE_ALLOCATOR(HTMLTemplateElement);

HTMLTemplateElement::HTMLTemplateElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLTemplateElement::~HTMLTemplateElement() = default;

void HTMLTemplateElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLTemplateElement);

    m_content = heap().allocate<DOM::DocumentFragment>(realm, m_document->appropriate_template_contents_owner_document());
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
WebIDL::ExceptionOr<void> HTMLTemplateElement::cloned(Node& copy, bool clone_children)
{
    // 1. If the clone children flag is not set in the calling clone algorithm, return.
    if (!clone_children)
        return {};

    // 2. Let copied contents be the result of cloning all the children of node's template contents,
    //    with document set to copy's template contents's node document, and with the clone children flag set.
    // 3. Append copied contents to copy's template contents.
    auto& template_clone = verify_cast<HTMLTemplateElement>(copy);
    for (auto child = content()->first_child(); child; child = child->next_sibling()) {
        auto cloned_child = TRY(child->clone_node(&template_clone.content()->document(), true));
        TRY(template_clone.content()->append_child(cloned_child));
    }
    return {};
}

void HTMLTemplateElement::set_template_contents(JS::NonnullGCPtr<DOM::DocumentFragment> contents)
{
    m_content = contents;
}

}
