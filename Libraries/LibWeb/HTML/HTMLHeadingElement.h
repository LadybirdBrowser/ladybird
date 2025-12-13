/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLHeadingElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLHeadingElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLHeadingElement);

public:
    virtual ~HTMLHeadingElement() override;

    virtual bool is_presentational_hint(FlyString const&) const override;
    virtual void apply_presentational_hints(GC::Ref<CSS::CascadedProperties>) const override;

    // https://www.w3.org/TR/html-aria/#el-h1-h6
    virtual Optional<ARIA::Role> default_role() const override { return ARIA::Role::heading; }

    WebIDL::UnsignedLong heading_level() const;

    virtual Optional<String> aria_level() const override
    {
        if (auto const attr = get_attribute(ARIA::AttributeNames::aria_level); attr.has_value())
            return attr;

        // Implicit defaults to the number in the element's tag name.
        return MUST(local_name().to_string().substring_from_byte_offset(1));
    }

private:
    HTMLHeadingElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

    mutable WebIDL::UnsignedLong m_cached_heading_level { 0 };
    mutable u64 m_dom_tree_version_for_cached_heading_level { 0 };
};

}
