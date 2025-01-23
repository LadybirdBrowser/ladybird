/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>

namespace Web::HTML {

class HTMLBaseElement final : public HTMLElement {
    WEB_PLATFORM_OBJECT(HTMLBaseElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLBaseElement);

public:
    virtual ~HTMLBaseElement() override;

    String href() const;
    WebIDL::ExceptionOr<void> set_href(String const& href);

    URL::URL const& frozen_base_url() const { return m_frozen_base_url; }

    virtual void inserted() override;
    virtual void removed_from(Node* old_parent, Node& old_root) override;
    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;

private:
    HTMLBaseElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual bool is_html_base_element() const override { return true; }

    // https://html.spec.whatwg.org/multipage/semantics.html#frozen-base-url
    // A base element that is the first base element with an href content attribute in a document tree has a frozen base URL.
    URL::URL m_frozen_base_url;

    void set_the_frozen_base_url();
};

}

namespace Web::DOM {
template<>
inline bool Node::fast_is<HTML::HTMLBaseElement>() const { return is_html_base_element(); }
}
