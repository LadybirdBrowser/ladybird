/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLPictureElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLPictureElement, HTMLElement);
    JS_DECLARE_ALLOCATOR(HTMLPictureElement);

public:
    virtual ~HTMLPictureElement() override;

private:
    HTMLPictureElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
};

}
