/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLQuoteElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLQuoteElement, HTMLElement);
    JS_DECLARE_ALLOCATOR(HTMLQuoteElement);

public:
    virtual ~HTMLQuoteElement() override;

    virtual Optional<ARIA::Role> default_role() const override;

private:
    HTMLQuoteElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
};

}
