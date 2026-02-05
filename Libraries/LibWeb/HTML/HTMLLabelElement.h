/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLLabelElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLLabelElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLLabelElement);

public:
    virtual ~HTMLLabelElement() override;

    Optional<String> for_() const { return attribute(HTML::AttributeNames::for_); }

    GC::Ptr<HTMLElement> control() const;
    GC::Ptr<HTMLFormElement> form() const;

private:
    HTMLLabelElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

    virtual bool has_activation_behavior() const override;
    virtual void activation_behavior(DOM::Event const&) override;

    bool m_click_in_progress { false };
};

}
