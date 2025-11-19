/*
 * Copyright (c) 2025, Feng Yu <f3n67u@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/form-elements.html#the-selectedcontent-element
class HTMLSelectedContentElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLSelectedContentElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLSelectedContentElement);

public:
    virtual ~HTMLSelectedContentElement() override;

    // https://www.w3.org/TR/html-aria/#el-selectedcontent
    virtual Optional<ARIA::Role> default_role() const override { return ARIA::Role::generic; }

    // https://html.spec.whatwg.org/multipage/form-elements.html#clear-a-selectedcontent
    void clear_selectedcontent();

    bool disabled() const { return m_disabled; }
    void set_disabled(bool disabled) { m_disabled = disabled; }

private:
    HTMLSelectedContentElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

    virtual void post_connection() override;
    virtual void removed_from(DOM::Node* old_parent, DOM::Node& old_root) override;

    // https://html.spec.whatwg.org/multipage/form-elements.html#selectedcontent-disabled
    bool m_disabled { false };
};

}
