/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLHRElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLHRElement, HTMLElement);
    JS_DECLARE_ALLOCATOR(HTMLHRElement);

public:
    virtual ~HTMLHRElement() override;

    // https://www.w3.org/TR/html-aria/#el-hr
    virtual Optional<ARIA::Role> default_role() const override { return ARIA::Role::separator; }

private:
    HTMLHRElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

    virtual void apply_presentational_hints(CSS::StyleProperties&) const override;
};

}
