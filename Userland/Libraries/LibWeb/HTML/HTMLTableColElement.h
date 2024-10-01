/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLTableColElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLTableColElement, HTMLElement);
    JS_DECLARE_ALLOCATOR(HTMLTableColElement);

public:
    virtual ~HTMLTableColElement() override;

    unsigned span() const;
    WebIDL::ExceptionOr<void> set_span(unsigned);

private:
    HTMLTableColElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

    virtual void apply_presentational_hints(CSS::StyleProperties&) const override;
};

}
