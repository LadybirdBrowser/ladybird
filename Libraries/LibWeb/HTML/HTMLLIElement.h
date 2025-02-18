/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/GenericShorthands.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/HTMLOListElement.h>
#include <LibWeb/HTML/HTMLUListElement.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

class HTMLLIElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLLIElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLLIElement);

public:
    virtual ~HTMLLIElement() override;

    // https://www.w3.org/TR/html-aria/#el-li
    virtual Optional<ARIA::Role> default_role() const override
    {
        for (auto const* ancestor = parent_element(); ancestor; ancestor = ancestor->parent_element()) {
            if (ancestor->role_or_default() == ARIA::Role::list)
                return ARIA::Role::listitem;
        }
        // https://w3c.github.io/core-aam/#roleMappingComputedRole
        // When an element has a role but is not contained in the required context (for example, an orphaned listitem
        // without the required accessible parent of role list), User Agents MUST ignore the role token, and return the
        // computedrole as if the ignored role token had not been included.
        return ARIA::Role::none;
    }

    WebIDL::Long value();
    void set_value(WebIDL::Long value)
    {
        MUST(set_attribute(AttributeNames::value, String::number(value)));
    }

private:
    HTMLLIElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

    virtual bool is_presentational_hint(FlyString const&) const override;
    virtual void apply_presentational_hints(GC::Ref<CSS::CascadedProperties>) const override;
};

}
