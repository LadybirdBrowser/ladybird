/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Checked.h>
#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

class HTMLOListElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLOListElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLOListElement);

public:
    virtual ~HTMLOListElement() override;

    // https://www.w3.org/TR/html-aria/#el-ol
    virtual Optional<ARIA::Role> default_role() const override { return ARIA::Role::list; }

    WebIDL::Long start();
    void set_start(WebIDL::Long start)
    {
        MUST(set_attribute(AttributeNames::start, String::number(start)));
    }

    AK::Checked<i32> starting_value() const;

    virtual bool is_html_olist_element() const override { return true; }

private:
    HTMLOListElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void attribute_changed(FlyString const& local_name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;

    virtual bool is_presentational_hint(FlyString const&) const override;
    virtual void apply_presentational_hints(GC::Ref<CSS::CascadedProperties>) const override;
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<Web::HTML::HTMLOListElement>() const { return is_html_olist_element(); }

}
