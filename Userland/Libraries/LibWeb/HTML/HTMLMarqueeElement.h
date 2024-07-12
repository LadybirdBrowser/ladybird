/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

// NOTE: This element is marked as obsolete, but is still listed as required by the specification.
class HTMLMarqueeElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLMarqueeElement, HTMLElement);
    JS_DECLARE_ALLOCATOR(HTMLMarqueeElement);

public:
    virtual ~HTMLMarqueeElement() override;

    WebIDL::UnsignedLong scroll_amount();
    WebIDL::ExceptionOr<void> set_scroll_amount(WebIDL::UnsignedLong);

    WebIDL::UnsignedLong scroll_delay();
    WebIDL::ExceptionOr<void> set_scroll_delay(WebIDL::UnsignedLong);

private:
    HTMLMarqueeElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void apply_presentational_hints(CSS::StyleProperties&) const override;
};

}
