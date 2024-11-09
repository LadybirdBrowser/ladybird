/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLOptGroupElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLOptGroupElement, HTMLElement);
    JS_DECLARE_ALLOCATOR(HTMLOptGroupElement);

public:
    virtual ~HTMLOptGroupElement() override;

    // https://www.w3.org/TR/html-aria/#el-optgroup
    virtual Optional<ARIA::Role> default_role() const override { return ARIA::Role::group; }

private:
    HTMLOptGroupElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
};

}
