/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/HTMLHyperlinkElementUtils.h>

namespace Web::HTML {

class HTMLAnchorElement final
    : public HTMLElement
    , public HTMLHyperlinkElementUtils {
    WEB_PLATFORM_OBJECT(HTMLAnchorElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLAnchorElement);

public:
    virtual ~HTMLAnchorElement() override;

    virtual Optional<URL::Origin> extract_an_origin() const override { return hyperlink_element_utils_extract_an_origin(); }

    String rel() const { return get_attribute_value(HTML::AttributeNames::rel); }
    String target() const { return get_attribute_value(HTML::AttributeNames::target); }
    String download() const { return get_attribute_value(HTML::AttributeNames::download); }

    GC::Ref<DOM::DOMTokenList> rel_list();

    Utf16String text() const;
    void set_text(Utf16String const&);

    // ^EventTarget
    // https://html.spec.whatwg.org/multipage/interaction.html#the-tabindex-attribute:the-a-element
    virtual bool is_focusable() const override { return Base::is_focusable() || has_attribute(HTML::AttributeNames::href); }

    virtual bool is_html_anchor_element() const override { return true; }

private:
    HTMLAnchorElement(DOM::Document&, DOM::QualifiedName);

    bool has_download_preference() const;

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    // ^DOM::EventTarget
    virtual bool has_activation_behavior() const override;
    virtual void activation_behavior(Web::DOM::Event const&) override;

    // ^DOM::Element
    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;
    virtual i32 default_tab_index_value() const override;

    // ^HTML::HTMLHyperlinkElementUtils
    virtual DOM::Element& hyperlink_element_utils_element() override { return *this; }
    virtual DOM::Element const& hyperlink_element_utils_element() const override { return *this; }
    virtual String hyperlink_element_utils_get_an_elements_target(Optional<String> target) const override { return get_an_elements_target(move(target)); }
    virtual TokenizedFeature::NoOpener hyperlink_element_utils_get_an_elements_noopener(URL::URL const& url, StringView target) const override { return get_an_elements_noopener(url, target); }

    virtual Optional<ARIA::Role> default_role() const override;

    GC::Ptr<DOM::DOMTokenList> m_rel_list;
};

}

namespace Web::DOM {

template<>
inline bool Node::fast_is<HTML::HTMLAnchorElement>() const { return is_html_anchor_element(); }

}
