/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

class HTMLPreElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLPreElement, HTMLElement);
    JS_DECLARE_ALLOCATOR(HTMLPreElement);

public:
    virtual ~HTMLPreElement() override;

    // https://www.w3.org/TR/html-aria/#el-pre
    virtual Optional<ARIA::Role> default_role() const override { return ARIA::Role::generic; }

private:
    HTMLPreElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void apply_presentational_hints(CSS::StyleProperties&) const override;
};

}
