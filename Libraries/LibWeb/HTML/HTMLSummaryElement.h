/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLSummaryElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLSummaryElement, HTMLElement);
    JS_DECLARE_ALLOCATOR(HTMLSummaryElement);

public:
    virtual ~HTMLSummaryElement() override;

    // https://www.w3.org/TR/html-aria/#el-details
    virtual Optional<ARIA::Role> default_role() const override { return ARIA::Role::button; }

    bool is_summary_for_its_parent_details();

    virtual bool has_activation_behavior() const override;
    virtual void activation_behavior(DOM::Event const&) override;

private:
    HTMLSummaryElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
};

}
