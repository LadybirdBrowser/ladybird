/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLParagraphElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLParagraphElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLParagraphElement);

public:
    virtual ~HTMLParagraphElement() override;

    virtual bool is_presentational_hint(FlyString const&) const override;
    virtual void apply_presentational_hints(GC::Ref<CSS::CascadedProperties>) const override;

    // https://www.w3.org/TR/html-aria/#el-p
    virtual Optional<ARIA::Role> default_role() const override { return ARIA::Role::paragraph; }

private:
    HTMLParagraphElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
};

}
