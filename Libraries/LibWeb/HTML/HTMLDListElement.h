/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLDListElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLDListElement, HTMLElement);
    JS_DECLARE_ALLOCATOR(HTMLDListElement);

public:
    virtual ~HTMLDListElement() override;

private:
    HTMLDListElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
};

}
