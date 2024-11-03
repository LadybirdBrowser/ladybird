/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLLIElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLLIElement, HTMLElement);
    JS_DECLARE_ALLOCATOR(HTMLLIElement);

public:
    virtual ~HTMLLIElement() override;

    // https://www.w3.org/TR/html-aria/#el-li
    virtual Optional<ARIA::Role> default_role() const override { return ARIA::Role::listitem; }

    i32 value() { return get_attribute(AttributeNames::value).value_or("0"_string).to_number<i32>().value_or(0); }
    void set_value(i32 value)
    {
        set_attribute(AttributeNames::value, String::number(value)).release_value_but_fixme_should_propagate_errors();
    }

private:
    HTMLLIElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
};

}
