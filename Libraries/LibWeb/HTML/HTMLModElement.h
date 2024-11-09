/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLModElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLModElement, HTMLElement);
    JS_DECLARE_ALLOCATOR(HTMLModElement);

public:
    virtual ~HTMLModElement() override;

    virtual Optional<ARIA::Role> default_role() const override;

private:
    HTMLModElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
};

}
