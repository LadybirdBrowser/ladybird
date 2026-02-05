/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class WEB_API HTMLOptGroupElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLOptGroupElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLOptGroupElement);

public:
    virtual ~HTMLOptGroupElement() override;

    // https://www.w3.org/TR/html-aria/#el-optgroup
    virtual Optional<ARIA::Role> default_role() const override { return ARIA::Role::group; }

private:
    HTMLOptGroupElement(DOM::Document&, DOM::QualifiedName);

    virtual bool is_html_optgroup_element() const final { return true; }

    virtual void initialize(JS::Realm&) override;
    virtual void removed_from(Node* old_parent, Node& old_root) override;
    virtual void inserted() override;
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<HTML::HTMLOptGroupElement>() const { return is_html_optgroup_element(); }

}
