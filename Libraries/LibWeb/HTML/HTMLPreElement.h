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
    GC_DECLARE_ALLOCATOR(HTMLPreElement);

public:
    virtual ~HTMLPreElement() override;

    // https://www.w3.org/TR/html-aria/#el-pre
    virtual Optional<ARIA::Role> default_role() const override { return ARIA::Role::generic; }

private:
    HTMLPreElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual bool is_presentational_hint(FlyString const&) const override;
    virtual void apply_presentational_hints(GC::Ref<CSS::CascadedProperties>) const override;
};

}
