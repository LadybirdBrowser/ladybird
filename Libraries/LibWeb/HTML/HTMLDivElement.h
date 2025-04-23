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
    GC_DECLARE_ALLOCATOR(HTMLDivElement);

public:
    virtual ~HTMLDivElement() override;

    // https://www.w3.org/TR/html-aria/#el-div
    virtual Optional<ARIA::Role> default_role() const override { return ARIA::Role::generic; }

    virtual bool is_html_div_element() const override { return true; }

protected:
    HTMLDivElement(DOM::Document&, DOM::QualifiedName);

private:
    virtual void initialize(JS::Realm&) override;
    virtual bool is_presentational_hint(FlyString const&) const override;
    virtual void apply_presentational_hints(GC::Ref<CSS::CascadedProperties>) const override;
};

}

namespace Web::DOM {
template<>
inline bool Node::fast_is<HTML::HTMLDivElement>() const { return is_html_div_element(); }
}
    