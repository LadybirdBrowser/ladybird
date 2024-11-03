/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLOListElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLOListElement, HTMLElement);
    JS_DECLARE_ALLOCATOR(HTMLOListElement);

public:
    virtual ~HTMLOListElement() override;

    // https://www.w3.org/TR/html-aria/#el-ol
    virtual Optional<ARIA::Role> default_role() const override { return ARIA::Role::list; }

    i32 start() { return get_attribute(AttributeNames::start).value_or("1"_string).to_number<i32>().value_or(1); }
    void set_start(i32 start)
    {
        set_attribute(AttributeNames::start, String::number(start)).release_value_but_fixme_should_propagate_errors();
    }

private:
    HTMLOListElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
};

}
