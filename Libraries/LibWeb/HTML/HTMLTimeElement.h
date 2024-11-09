/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLTimeElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLTimeElement, HTMLElement);
    JS_DECLARE_ALLOCATOR(HTMLTimeElement);

public:
    virtual ~HTMLTimeElement() override;

    // https://www.w3.org/TR/html-aria/#el-time
    virtual Optional<ARIA::Role> default_role() const override { return ARIA::Role::time; }

private:
    HTMLTimeElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
};

}
