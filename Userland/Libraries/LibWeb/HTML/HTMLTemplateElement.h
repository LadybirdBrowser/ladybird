/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/DocumentFragment.h>
#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLTemplateElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLTemplateElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLTemplateElement);

public:
    virtual ~HTMLTemplateElement() override;

    GC::Ref<DOM::DocumentFragment> content() { return *m_content; }
    GC::Ref<DOM::DocumentFragment> const content() const { return *m_content; }

    void set_template_contents(GC::Ref<DOM::DocumentFragment>);

    virtual void adopted_from(DOM::Document&) override;
    virtual WebIDL::ExceptionOr<void> cloned(Node& copy, bool clone_children) override;

private:
    HTMLTemplateElement(DOM::Document&, DOM::QualifiedName);

    virtual bool is_html_template_element() const final { return true; }

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ptr<DOM::DocumentFragment> m_content;
};

}

namespace Web::DOM {
template<>
inline bool Node::fast_is<HTML::HTMLTemplateElement>() const { return is_html_template_element(); }
}
