/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLUListElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLUListElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLUListElement);

public:
    virtual ~HTMLUListElement() override;

    // https://www.w3.org/TR/html-aria/#el-ul
    virtual Optional<ARIA::Role> default_role() const override { return ARIA::Role::list; }

private:
    HTMLUListElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;

    virtual bool is_presentational_hint(FlyString const&) const override;
    virtual void apply_presentational_hints(GC::Ref<CSS::CascadedProperties>) const override;
};

}
