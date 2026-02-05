/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/StyleElementBase.h>
#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLStyleElement final
    : public HTMLElement
    , public DOM::StyleElementBase {
    WEB_PLATFORM_OBJECT(HTMLStyleElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLStyleElement);

public:
    virtual ~HTMLStyleElement() override;

    virtual void children_changed(ChildrenChangedMetadata const*) override;
    virtual void inserted() override;
    virtual void removed_from(Node* old_parent, Node& old_root) override;
    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;

    bool disabled();
    void set_disabled(bool disabled);

    virtual bool contributes_a_script_blocking_style_sheet() const final;

private:
    HTMLStyleElement(DOM::Document&, DOM::QualifiedName);

    // ^DOM::Node
    virtual bool is_html_style_element() const override { return true; }

    // ^DOM::StyleElementBase
    virtual Element& as_element() override { return *this; }

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
};

}
