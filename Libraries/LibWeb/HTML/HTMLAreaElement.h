/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Path.h>
#include <LibWeb/HTML/HTMLElement.h>
#include <LibWeb/HTML/HTMLHyperlinkElementUtils.h>
#include <LibWeb/PixelUnits.h>

namespace Web::HTML {

class HTMLAreaElement final
    : public HTMLElement
    , public HTMLHyperlinkElementUtils {
    WEB_WRAPPABLE(HTMLAreaElement, HTMLElement);
    GC_DECLARE_ALLOCATOR(HTMLAreaElement);

public:
    virtual ~HTMLAreaElement() override;
    GC::Ref<DOM::DOMTokenList> rel_list();

    enum class ShapeState : u8 {
        Circle,
        Default,
        Polygon,
        Rectangle,
    };
    ShapeState shape_state() const;

    Optional<Gfx::Path> shape_path(CSSPixelSize image_size) const;
    bool shape_contains_point(CSSPixelPoint, CSSPixelSize image_size) const;

private:
    HTMLAreaElement(DOM::Document&, DOM::QualifiedName);

    virtual bool is_html_area_element() const override { return true; }
    virtual void visit_edges(Cell::Visitor&) override;

    virtual Optional<URL::Origin> extract_an_origin() const final { return hyperlink_element_utils_extract_an_origin(); }

    // ^DOM::EventTarget
    virtual bool has_activation_behavior() const override;
    virtual void activation_behavior(Web::DOM::Event const&) override;

    // ^DOM::Element
    virtual void attribute_changed(Utf16FlyString const& name, Optional<Utf16String> const& old_value, Optional<Utf16String> const& value, Optional<Utf16FlyString> const& namespace_) override;
    virtual i32 default_tab_index_value() const override;
    virtual bool is_focusable() const override;
    virtual void did_receive_focus() override;
    virtual void did_lose_focus() override;

    void repaint_associated_images();

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
