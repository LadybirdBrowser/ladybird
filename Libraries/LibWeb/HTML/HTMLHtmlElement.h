/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/Export.h>
#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class WEB_API HTMLHtmlElement final : public HTMLElement {
    WEB_WRAPPABLE(HTMLHtmlElement, HTMLElement);
    friend struct DOM::RustElementObjectHooks;

public:
    // Being the document's body is one of the facts a layout row is built with, and which child is the body moves
    // with this element's children. Called wherever they move.
    void publish_body_construction_facts();

    // https://www.w3.org/TR/html-aria/#el-html
    virtual Optional<ARIA::Role> default_role() const override { return ARIA::Role::document; }

private:
    explicit HTMLHtmlElement(DOM::VtableProbe);
    virtual ~HTMLHtmlElement() override = default;
    virtual void children_changed(ChildrenChangedMetadata const&) override;
};

}
