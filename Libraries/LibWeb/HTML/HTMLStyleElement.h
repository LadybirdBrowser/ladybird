/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/StyleElementUtils.h>
#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLStyleElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLStyleElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLStyleElement);

public:
    virtual ~HTMLStyleElement() override;

    virtual void children_changed(ChildrenChangedMetadata const*) override;
    virtual void inserted() override;
    virtual void removed_from(Node* old_parent, Node& old_root) override;

    bool disabled();
    void set_disabled(bool disabled);

    [[nodiscard]] String media() const;
    void set_media(String);

    CSS::CSSStyleSheet* sheet();
    CSS::CSSStyleSheet const* sheet() const;

private:
    HTMLStyleElement(DOM::Document&, DOM::QualifiedName);

    // ^DOM::Node
    virtual bool is_html_style_element() const override { return true; }

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    DOM::StyleElementUtils m_style_element_utils;
};

}
