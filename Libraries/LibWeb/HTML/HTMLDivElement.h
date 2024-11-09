/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLDivElement : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLDivElement, HTMLElement);
    JS_DECLARE_ALLOCATOR(HTMLDivElement);

public:
    virtual ~HTMLDivElement() override;

    // https://www.w3.org/TR/html-aria/#el-div
    virtual Optional<ARIA::Role> default_role() const override { return ARIA::Role::generic; }

protected:
    HTMLDivElement(DOM::Document&, DOM::QualifiedName);

private:
    virtual void initialize(JS::Realm&) override;
    virtual void apply_presentational_hints(CSS::StyleProperties&) const override;
};

}
