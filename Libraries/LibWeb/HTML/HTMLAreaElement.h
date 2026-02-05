/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/HTMLHyperlinkElementUtils.h>

namespace Web::HTML {

class HTMLAreaElement final
    : public HTMLElement
    , public HTMLHyperlinkElementUtils {
    WEB_PLATFORM_OBJECT(HTMLAreaElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLAreaElement);

public:
    virtual ~HTMLAreaElement() override;
    GC::Ref<DOM::DOMTokenList> rel_list();

private:
    HTMLAreaElement(DOM::Document&, DOM::QualifiedName);

    virtual bool is_html_area_element() const override { return true; }

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    virtual Optional<URL::Origin> extract_an_origin() const final { return hyperlink_element_utils_extract_an_origin(); }

    // ^DOM::Element
    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;
    virtual i32 default_tab_index_value() const override;

    // ^HTML::HTMLHyperlinkElementUtils
    virtual DOM::Element& hyperlink_element_utils_element() override { return *this; }
    virtual DOM::Element const& hyperlink_element_utils_element() const override { return *this; }

    virtual Optional<ARIA::Role> default_role() const override;

    GC::Ptr<DOM::DOMTokenList> m_rel_list;
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<HTML::HTMLAreaElement>() const { return is_html_area_element(); }

}
