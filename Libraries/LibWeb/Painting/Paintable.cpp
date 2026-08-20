/*
 * Copyright (c) 2022-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025-2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/GenericShorthands.h>
#include <AK/StdLibExtras.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Font/Font.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/Invalidation/ContainerQueryInvalidator.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/StyleInvalidation.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/CSS/StyleValues/BorderImageSliceStyleValue.h>
#include <LibWeb/CSS/StyleValues/ColorSchemeStyleValue.h>
#include <LibWeb/CSS/StyleValues/ImageStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/URLStyleValue.h>
#include <LibWeb/CSS/SystemColor.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/DOM/Position.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLAreaElement.h>
#include <LibWeb/HTML/HTMLBodyElement.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLMapElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/LayoutRustBridge.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Layout/TextOffsetMapping.h>
#include <LibWeb/Page/EventHandler.h>
#include <LibWeb/Page/MiddleButtonScrollHandler.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Painting/ChromeMetrics.h>
#include <LibWeb/Painting/DisplayListRecordingContext.h>
#include <LibWeb/Painting/FlexboxInspectorOverlay.h>
#include <LibWeb/Painting/GridInspectorOverlay.h>
#include <LibWeb/Painting/HitTestDisplayList.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/Painting/PaintableWithLines.h>
#include <LibWeb/Painting/PaintingRustBridge.h>
#include <LibWeb/Painting/ResizeHandle.h>
#include <LibWeb/Painting/Scrollbar.h>
#include <LibWeb/Painting/ViewportPaintable.h>
#include <LibWeb/SVG/SVGFilterElement.h>
#include <LibWeb/SVG/SVGFitToViewBox.h>
#include <LibWeb/SVG/SVGSVGElement.h>

namespace Web::Painting {

static PixelBox pixel_box_from_ffi(Layout::RustFFI::FfiPixelBox const& box)
{
    return { CSSPixels::from_raw(box.top), CSSPixels::from_raw(box.right), CSSPixels::from_raw(box.bottom), CSSPixels::from_raw(box.left) };
}

BoxModelMetrics Paintable::box_model() const
{
    return {
        .margin = pixel_box_from_ffi(rust_data().margin),
        .padding = pixel_box_from_ffi(rust_data().padding),
        .border = pixel_box_from_ffi(rust_data().border),
        .inset = pixel_box_from_ffi(rust_data().inset),
    };
}

CSS::Display Paintable::display() const
{
    return CSS::display_from_ffi_display(CSS::decode_ffi_display(rust_data().display));
}

CSSPixelSize Paintable::content_size() const
{
    return { CSSPixels::from_raw(rust_data().content_size.width), CSSPixels::from_raw(rust_data().content_size.height) };
}

Optional<Paintable::OverflowData> Paintable::overflow_data() const
{
    if (!rust_data().has_overflow)
        return {};
    return OverflowData { from_ffi_css_pixel_rect(rust_data().overflow.rect), rust_data().overflow.has_scrollable_overflow };
}

Optional<Paintable::CachedOverflowData> Paintable::cached_overflow_data() const
{
    if (!rust_data().has_cached_overflow)
        return {};
    return CachedOverflowData { from_ffi_css_pixel_rect(rust_data().cached_overflow.rect), rust_data().cached_overflow.has_scrollable_overflow };
}

Paintable::StickyInsets Paintable::sticky_insets() const
{
    VERIFY(has_sticky_insets());
    auto const& insets = rust_data().sticky_insets;
    auto side = [](i32 raw, bool present) -> Optional<CSSPixels> {
        if (!present)
            return {};
        return CSSPixels::from_raw(raw);
    };
    return { side(insets.top, insets.has_top), side(insets.right, insets.has_right), side(insets.bottom, insets.has_bottom), side(insets.left, insets.has_left) };
}

String Paintable::debug_description() const
{
    return MUST(String::formatted("{}({})", class_name(), layout_node().debug_description()));
}

DOM::Document const& Paintable::document() const
{
    return layout_node().document();
}

DOM::Document& Paintable::document()
{
    return layout_node().document();
}

RefPtr<Paintable> Paintable::containing_block() const
{
    return const_cast<Paintable*>(containing_block_ptr());
}

Paintable const* Paintable::containing_block_ptr() const
{
    return shell_from_slot(rust_data().containing_block);
}

Paintable* Paintable::shell_from_slot(Layout::RustFFI::PaintableSlotId slot) const
{
    return static_cast<Paintable*>(Layout::RustFFI::layout_arena_paintable_shell(m_rust_arena->handle(), slot));
}

bool Paintable::is_visible() const
{
    return layout_node().visibility() == CSS::Visibility::Visible && layout_node().opacity() != 0;
}

CSS::StyleRecordID Paintable::style_record_identity() const
{
    return layout_node().style_record_identity();
}

bool Paintable::visible_for_hit_testing() const
{
    if (auto node = dom_node(); node && node->is_inert())
        return false;
    return layout_node().pointer_events() != CSS::PointerEvents::None;
}

void Paintable::set_dom_node(GC::Ptr<DOM::Node> dom_node)
{
    m_dom_node = dom_node.ptr();
}

GC::Ptr<DOM::Node> Paintable::dom_node()
{
    return m_dom_node.ptr();
}

GC::Ptr<DOM::Node const> Paintable::dom_node() const
{
    return m_dom_node.ptr();
}

GC::Ptr<HTML::LocalNavigable> Paintable::navigable() const
{
    return document().navigable();
}

bool Paintable::has_stacking_context() const
{
    return rust_data().stacking_context != Layout::RustFFI::NO_STACKING_CONTEXT;
}

DOM::Node* HitTestResult::dom_node()
{
    if (dom_node_override)
        return dom_node_override.ptr();

    for (auto* current = paintable.ptr(); current; current = current->parent()) {
        if (auto node = current->dom_node())
            return node.ptr();
    }
    return nullptr;
}

DOM::Node const* HitTestResult::dom_node() const
{
    if (dom_node_override)
        return dom_node_override.ptr();

    for (auto const* current = paintable.ptr(); current; current = current->parent()) {
        if (auto node = current->dom_node())
            return node.ptr();
    }
    return nullptr;
}

CSSPixelPoint Paintable::box_type_agnostic_position() const
{
    return absolute_position();
}

Painting::BorderRadiiData normalize_border_radii_data(CSSPixelRect const& border_rect, CSSPixelRect const& reference_rect, CSS::BorderRadiusData const& top_left_radius, CSS::BorderRadiusData const& top_right_radius, CSS::BorderRadiusData const& bottom_right_radius, CSS::BorderRadiusData const& bottom_left_radius)
{
    Painting::BorderRadiiData radii_px {
        .top_left = {
            top_left_radius.horizontal_radius.to_px(reference_rect.width()),
            top_left_radius.vertical_radius.to_px(reference_rect.height()) },
        .top_right = { top_right_radius.horizontal_radius.to_px(reference_rect.width()), top_right_radius.vertical_radius.to_px(reference_rect.height()) },
        .bottom_right = { bottom_right_radius.horizontal_radius.to_px(reference_rect.width()), bottom_right_radius.vertical_radius.to_px(reference_rect.height()) },
        .bottom_left = { bottom_left_radius.horizontal_radius.to_px(reference_rect.width()), bottom_left_radius.vertical_radius.to_px(reference_rect.height()) }
    };

    // Scale overlapping curves according to https://www.w3.org/TR/css-backgrounds-3/#corner-overlap
    // Let f = min(Li/Si), where i ∈ {top, right, bottom, left},
    // Si is the sum of the two corresponding radii of the corners on side i,
    // and Ltop = Lbottom = the width of the box, and Lleft = Lright = the height of the box.
    //
    // NOTE: We iterate twice as a form of iterative refinement. A single scaling pass using
    // fixed-point arithmetic can result in small rounding errors, causing the scaled radii to
    // still slightly overflow the box dimensions. A second pass corrects this remaining error.
    auto border_width = max(CSSPixels(0), border_rect.width());
    auto border_height = max(CSSPixels(0), border_rect.height());
    for (int iteration = 0; iteration < 2; ++iteration) {
        auto s_top = radii_px.top_left.horizontal_radius + radii_px.top_right.horizontal_radius;
        auto s_right = radii_px.top_right.vertical_radius + radii_px.bottom_right.vertical_radius;
        auto s_bottom = radii_px.bottom_right.horizontal_radius + radii_px.bottom_left.horizontal_radius;
        auto s_left = radii_px.bottom_left.vertical_radius + radii_px.top_left.vertical_radius;

        CSSPixelFraction f = 1;
        if (s_top > 0 && s_top > border_width)
            f = min(f, border_width / s_top);
        if (s_right > 0 && s_right > border_height)
            f = min(f, border_height / s_right);
        if (s_bottom > 0 && s_bottom > border_width)
            f = min(f, border_width / s_bottom);
        if (s_left > 0 && s_left > border_height)
            f = min(f, border_height / s_left);

        // If f is 1 or more, the radii fit perfectly and no more scaling is needed
        if (f >= 1)
            break;

        Painting::BorderRadiusData* corners[] = {
            &radii_px.top_left, &radii_px.top_right, &radii_px.bottom_right, &radii_px.bottom_left
        };

        for (auto* corner : corners) {
            corner->horizontal_radius *= f;
            corner->vertical_radius *= f;
        }
    }

    return radii_px;
}

// https://drafts.csswg.org/css-pseudo-4/#highlight-styling
// FIXME: Support additional ::selection properties: text-underline-offset, text-underline-position, stroke-color,
//        fill-color, stroke-width, and CSS custom properties.
Paintable::SelectionStyle Paintable::selection_style() const
{
    return selection_style_for_node(layout_node(), dom_node());
}

Paintable::SelectionStyle Paintable::selection_style_for_node(Layout::Node const& layout_node, GC::Ptr<DOM::Node const> node)
{
    // Selections render in a muted color while the window does not have focus.
    auto navigable = layout_node.document().navigable();
    auto window_is_active = navigable && navigable->is_focused();
    auto const* layout_node_with_style = as_if<Layout::NodeWithStyle>(layout_node);
    auto const& style_source = layout_node_with_style ? *layout_node_with_style : *layout_node.parent();

    auto default_style_for_color_scheme = [&](CSS::PreferredColorScheme color_scheme, bool use_palette_for_normal_color_scheme = true) {
        auto palette = layout_node.document().page().palette();
        auto palette_color_scheme = palette.is_dark() ? CSS::PreferredColorScheme::Dark : CSS::PreferredColorScheme::Light;
        if (color_scheme == palette_color_scheme || use_palette_for_normal_color_scheme) {
            auto background = window_is_active ? palette.selection() : palette.inactive_selection();
            return SelectionStyle { CSS::SystemColor::transform_selection_background_color(background) };
        }

        auto background = window_is_active ? CSS::SystemColor::highlight(color_scheme) : CSS::SystemColor::inactive_highlight(color_scheme);
        return SelectionStyle { CSS::SystemColor::transform_selection_background_color(background) };
    };

    // For text nodes, check the parent element since text nodes don't have computed properties.
    if (!node)
        return default_style_for_color_scheme(style_source.color_scheme());

    DOM::Element const* element = as_if<DOM::Element>(*node);
    if (!element)
        element = node->parent_element().ptr();
    if (!element)
        return default_style_for_color_scheme(style_source.color_scheme());

    auto color_scheme_is_normal = style_source.color_schemes().is_empty();
    auto use_palette_for_normal_color_scheme = color_scheme_is_normal && !layout_node.document().supported_color_schemes().has_value();
    auto default_style = default_style_for_color_scheme(style_source.color_scheme(), use_palette_for_normal_color_scheme);

    auto style_from_element = [&](DOM::Element const& element) -> Optional<SelectionStyle> {
        auto computed_selection_style = element.computed_style(CSS::PseudoElement::Selection);
        if (!computed_selection_style)
            return {};

        SelectionStyle style;
        style.background_color = computed_selection_style->background_color();

        // Only use text color if it was explicitly set in the ::selection rule, not inherited.
        if (!computed_selection_style->is_property_inherited(CSS::PropertyID::Color))
            style.text_color = computed_selection_style->color();

        // Only use text-shadow if it was explicitly set in the ::selection rule, not inherited.
        if (!computed_selection_style->is_property_inherited(CSS::PropertyID::TextShadow)) {
            auto const& css_shadows = computed_selection_style->text_shadow();
            Vector<ShadowData> shadows;
            shadows.ensure_capacity(css_shadows.size());
            for (auto const& shadow : css_shadows)
                shadows.unchecked_append(ShadowData::from_css(shadow));
            style.text_shadow = move(shadows);
        }

        // Only use text-decoration if it was explicitly set in the ::selection rule, not inherited.
        if (!computed_selection_style->is_property_inherited(CSS::PropertyID::TextDecorationLine)) {
            style.text_decoration = TextDecorationStyle {
                .line = Vector<CSS::TextDecorationLine> { computed_selection_style->text_decoration_line() },
                .style = computed_selection_style->text_decoration_style(),
                .color = computed_selection_style->text_decoration_color(),
            };
        }

        // Only return a style if there's a meaningful customization. This allows us to continue checking shadow hosts
        // when the current element only has UA default styles.
        if (!style.has_styling())
            return {};

        return style;
    };

    // Check the element itself.
    if (auto style = style_from_element(*element); style.has_value())
        return style.release_value();

    // If inside a shadow tree, check the shadow host. This enables ::selection styling on elements like <input> to
    // apply to text rendered inside their shadow DOM.
    if (auto shadow_root = element->containing_shadow_root(); shadow_root && shadow_root->is_user_agent_internal()) {
        if (auto const* host = shadow_root->host()) {
            if (auto style = style_from_element(*host); style.has_value())
                return style.release_value();
        }
    }

    return default_style;
}

void Paintable::clear_overflow_data()
{
    Layout::RustFFI::layout_arena_paintable_clear_overflow_data(m_rust_arena->handle(), m_rust_slot);
}

void Paintable::clear_cached_overflow_data()
{
    Layout::RustFFI::layout_arena_paintable_clear_cached_overflow_data(m_rust_arena->handle(), m_rust_slot);
}

void Paintable::set_sticky_insets(OwnPtr<StickyInsets> sticky_insets)
{
    Layout::RustFFI::FfiStickyInsets ffi_insets {};
    if (sticky_insets) {
        auto pack = [](Optional<CSSPixels> const& value, i32& raw, bool& present) {
            present = value.has_value();
            raw = value.has_value() ? value->raw_value() : 0;
        };
        pack(sticky_insets->top, ffi_insets.top, ffi_insets.has_top);
        pack(sticky_insets->right, ffi_insets.right, ffi_insets.has_right);
        pack(sticky_insets->bottom, ffi_insets.bottom, ffi_insets.has_bottom);
        pack(sticky_insets->left, ffi_insets.left, ffi_insets.has_left);
    }
    Layout::RustFFI::layout_arena_paintable_set_sticky_insets(m_rust_arena->handle(), m_rust_slot, ffi_insets, !!sticky_insets);
}

void Paintable::set_selection_state(SelectionState state)
{
    if (selection_state() == state)
        return;
    Layout::RustFFI::layout_arena_paintable_set_selection_state(m_rust_arena->handle(), m_rust_slot, to_underlying(state));
    invalidate_paint_cache();
}

bool Paintable::should_paint_cursor() const
{
    if (!document().cursor_blink_state() || !document().navigable()->is_focused())
        return false;

    auto cursor_position = document().cursor_position();
    if (!cursor_position)
        return false;

    if (auto const* text_control = as_if<HTML::FormAssociatedTextControlElement>(document().focused_area().ptr());
        text_control && text_control->text_control_to_html_element().is_mutable()) {
        return true;
    }

    // The editable element may sit anywhere between the cursor and this box (e.g. a
    // contenteditable inline box), so editability is the cursor node's, not this box's.
    auto const* editable_node = cursor_position->node().ptr();
    return editable_node && editable_node->is_editable_or_editing_host();
}

void Paintable::scroll_text_offset_into_view(DOM::Text const& text, size_t offset, TextAffinity affinity, ScrollBlockDirection scroll_block_direction)
{
    auto scroll_to_cursor = [&](PaintableFragment const& fragment) {
        auto cursor_rect = fragment.range_rect(SelectionState::StartAndEnd, offset, offset);
        auto const& style_source = fragment.style_source();
        if (style_source.writing_mode() == CSS::WritingMode::HorizontalTb) {
            if (style_source.inline_axis_is_reverse())
                cursor_rect.set_x(cursor_rect.x() - 1);
            cursor_rect.set_width(1);
        } else {
            if (style_source.inline_axis_is_reverse())
                cursor_rect.set_y(cursor_rect.y() - 1);
            cursor_rect.set_height(1);
        }
        for (auto ancestor = fragment.containing_block_paintable(); ancestor; ancestor = ancestor->containing_block()) {
            if (ancestor->has_scrollable_overflow()) {
                if (scroll_block_direction == ScrollBlockDirection::No) {
                    auto snapport = ancestor->scroll_snapport_rect();
                    if (style_source.writing_mode() == CSS::WritingMode::HorizontalTb) {
                        cursor_rect.set_y(snapport.y() + ancestor->scroll_offset().y());
                        cursor_rect.set_height(snapport.height());
                    } else {
                        cursor_rect.set_x(snapport.x() + ancestor->scroll_offset().x());
                        cursor_rect.set_width(snapport.width());
                    }
                }
                ancestor->scroll_into_view(cursor_rect);
                return;
            }
        }
    };

    PaintableFragment const* fallback_fragment = nullptr;
    Layout::TextOffsetMapping mapping { text };
    mapping.for_each_paintable_fragment([&](PaintableFragment const& fragment) {
        switch (fragment.caret_match(offset, affinity)) {
        case PaintableFragment::CaretMatch::None:
            return TraversalDecision::Continue;
        case PaintableFragment::CaretMatch::SoftWrapFallback:
            if (!fallback_fragment)
                fallback_fragment = &fragment;
            return TraversalDecision::Continue;
        case PaintableFragment::CaretMatch::Direct:
            fallback_fragment = nullptr;
            scroll_to_cursor(fragment);
            return TraversalDecision::Break;
        }
        VERIFY_NOT_REACHED();
    });
    if (fallback_fragment)
        scroll_to_cursor(*fallback_fragment);
}

void Paintable::scroll_ancestor_to_offset_into_view(size_t offset)
{
    if (auto const* text = as_if<DOM::Text>(dom_node().ptr()))
        scroll_text_offset_into_view(*text, offset);
}

static bool g_paint_viewport_scrollbars = true;

static bool content_size_change_affects_container_queries(Paintable const& paintable_box, CSSPixelSize old_size, CSSPixelSize new_size)
{
    auto container_type = paintable_box.layout_node().container_type();
    if (container_type.is_size_container)
        return old_size != new_size;

    if (!container_type.is_inline_size_container)
        return false;

    if (paintable_box.layout_node().writing_mode() == CSS::WritingMode::HorizontalTb)
        return old_size.width() != new_size.width();

    return old_size.height() != new_size.height();
}

static void invalidate_descendant_styles_for_container_query_size_change(Paintable& paintable_box, CSSPixelSize old_size, CSSPixelSize new_size)
{
    if (!content_size_change_affects_container_queries(paintable_box, old_size, new_size))
        return;

    if (auto* element = as_if<DOM::Element>(paintable_box.dom_node().ptr()))
        CSS::Invalidation::invalidate_descendant_styles_depending_on_size_container_query(*element);
}

void set_paint_viewport_scrollbars(bool const enabled)
{
    g_paint_viewport_scrollbars = enabled;
}

bool should_paint_viewport_scrollbars()
{
    return g_paint_viewport_scrollbars;
}

bool body_background_is_propagated_to_root(Layout::NodeWithStyle const& layout_node)
{
    if (!layout_node.is_body())
        return false;
    // Reachable at invalidation time, when the root element's layout node may already be detached.
    auto const* html_element = layout_node.document().html_element();
    return html_element && html_element->unsafe_layout_node() && html_element->should_use_body_background_properties();
}

static bool is_canvas_background_source(Layout::NodeWithStyle const& layout_node)
{
    return layout_node.is_root_element() || body_background_is_propagated_to_root(layout_node);
}

static Color effective_scrollbar_background_color(Paintable const& paintable_box)
{
    auto background_color = paintable_box.document().canvas_background_color();

    Vector<Layout::NodeWithStyle const*, 32> ancestors;
    for (Layout::NodeWithStyle const* layout_node = &paintable_box.layout_node(); layout_node; layout_node = layout_node->parent())
        ancestors.append(layout_node);

    for (auto const* layout_node : ancestors.in_reverse()) {
        auto const& layout_node_with_style = *layout_node;
        if (is_canvas_background_source(layout_node_with_style))
            continue;

        auto color = layout_node_with_style.background_color();
        if (color.alpha() == 0)
            continue;

        background_color = background_color.blend(color);
    }

    return background_color;
}

static CSS::ScrollbarColorData automatic_scrollbar_colors(Paintable const& paintable_box)
{
    auto background_color = effective_scrollbar_background_color(paintable_box);
    auto black_thumb = Color(Color::Black).with_alpha(128);
    auto white_thumb = Color(Color::White).with_alpha(128);

    auto black_thumb_contrast = background_color.contrast_ratio(background_color.blend(black_thumb));
    auto white_thumb_contrast = background_color.contrast_ratio(background_color.blend(white_thumb));
    auto thumb_color = black_thumb_contrast >= white_thumb_contrast ? black_thumb : white_thumb;

    return {
        .thumb_color = thumb_color,
        .track_color = thumb_color.with_alpha(25),
        .is_auto = true,
    };
}

CSS::ScrollbarColorData scrollbar_colors_for_paint(Paintable const& paintable_box)
{
    auto scrollbar_colors = paintable_box.layout_node().scrollbar_color();
    if (!scrollbar_colors.is_auto)
        return scrollbar_colors;

    return automatic_scrollbar_colors(paintable_box);
}

Paintable const* nearest_svg_viewport_paintable_of(Layout::Node const& layout_node)
{
    for (auto const* ancestor = layout_node.parent(); ancestor; ancestor = ancestor->parent()) {
        if (auto paintable = ancestor->paintable(); paintable && paintable->svg_viewport_transform().has_value())
            return paintable.ptr();
    }
    return nullptr;
}

// active_view_box covers <view> redirection and the svg-as-image fallback viewBox, which
// layout used to build the geometry these callers interpret.
static Gfx::FloatRect svg_svg_box_view_box_or_viewport_rect(Layout::Box const& svg_svg_box)
{
    if (auto view_box = as<SVG::SVGSVGElement>(*svg_svg_box.dom_node()).active_view_box(); view_box.has_value())
        return { view_box->min_x, view_box->min_y, view_box->width, view_box->height };
    if (auto const* paintable = svg_svg_box.paintable_box().ptr())
        return { {}, { paintable->svg_viewport_size().width().to_float(), paintable->svg_viewport_size().height().to_float() } };
    return {};
}

Gfx::FloatRect svg_viewport_user_rect(Paintable const& viewport_paintable)
{
    if (viewport_paintable.layout_node().is_svg_svg_box())
        return svg_svg_box_view_box_or_viewport_rect(static_cast<Layout::Box const&>(viewport_paintable.layout_node()));
    if (auto dom_node = viewport_paintable.dom_node()) {
        if (auto const* fit_to_view_box = as_if<SVG::SVGFitToViewBox>(*dom_node)) {
            if (auto view_box = fit_to_view_box->view_box(); view_box.has_value())
                return { static_cast<float>(view_box->min_x), static_cast<float>(view_box->min_y), static_cast<float>(view_box->width), static_cast<float>(view_box->height) };
        }
    }
    return { {}, viewport_paintable.absolute_rect().size().to_type<float>() };
}

ResolvedCSSFilter resolve_css_filter(CSS::ComputedFilterView computed_filter, Paintable const& paintable_box)
{
    auto const& layout_node = paintable_box.layout_node();

    ResolvedCSSFilter result;
    bool failed = false;
    computed_filter.for_each_operation([&](auto const& operation) {
        if (failed)
            return;
        if (auto const* url = operation.template get_pointer<CSS::Filter::Url>()) {
            if (url->fragment.is_empty()) {
                failed = true;
                return;
            }
            auto maybe_filter = paintable_box.document().get_element_by_id(url->fragment);
            if (!maybe_filter) {
                failed = true;
                return;
            }
            if (auto* filter_element = as_if<SVG::SVGFilterElement>(*maybe_filter)) {
                // Filter primitive lengths are specified in the filtered element's user coordinate system, but the
                // resulting filter operates in device pixels. Compute the user-unit-to-device-pixel scale so the
                // filter can convert its lengths accordingly.
                // The replay-time layer maps filter parameters through the accumulated transform,
                // so only the device pixel ratio — which lives in recorded coordinates, not in the
                // transform chain — converts here.
                auto device_pixels_per_css_pixel = paintable_box.document().page().client().device_pixels_per_css_pixel();
                auto filter_scale = Gfx::FloatPoint { device_pixels_per_css_pixel, device_pixels_per_css_pixel };
                result.svg_filter = filter_element->gfx_filter(layout_node, filter_scale);
                // The bounds live in the filtered element's user space; an element without
                // geometry of its own falls back to the whole enclosing viewport rect there.
                auto bounds = paintable_box.absolute_border_box_rect();
                if (bounds.is_empty()) {
                    if (auto const* viewport_paintable = nearest_svg_viewport_paintable_of(paintable_box.layout_node()))
                        result.svg_filter_bounds = svg_viewport_user_rect(*viewport_paintable).to_type<CSSPixels>();
                }
                if (!bounds.is_empty())
                    result.svg_filter_bounds = bounds;
            } else {
                failed = true;
            }
            return;
        }

        operation.visit(
            [&](CSS::Filter::Blur const& blur) {
                result.operations.empend(ResolvedCSSFilter::Blur {
                    .radius = CSSPixels::nearest_value_for(blur.resolved_radius),
                });
            },
            [&](CSS::Filter::DropShadow const& drop_shadow) {
                result.operations.empend(ResolvedCSSFilter::DropShadow {
                    .offset_x = drop_shadow.offset_x,
                    .offset_y = drop_shadow.offset_y,
                    .radius = drop_shadow.radius,
                    .color = drop_shadow.color,
                });
            },
            [&](CSS::Filter::ColorOperation const& color_operation) {
                result.operations.empend(ResolvedCSSFilter::Color {
                    .operation = color_operation.operation,
                    .amount = color_operation.resolved_amount,
                });
            },
            [&](CSS::Filter::HueRotate const& hue_rotate) {
                result.operations.empend(ResolvedCSSFilter::HueRotate {
                    .angle_degrees = hue_rotate.angle_degrees,
                });
            },
            [&](CSS::Filter::Url const&) {});
    });
    if (failed)
        return {};
    return result;
}

NonnullRefPtr<Paintable> Paintable::create(Layout::Box const& layout_box)
{
    return adopt_ref(*new Paintable(layout_box));
}

Paintable::Paintable(Layout::NodeWithStyle const& layout_node)
    : m_layout_node(layout_node)
    , m_rust_arena(layout_node.node_arena())
{
    auto allocation = m_rust_arena->paintable_row_for_node(Layout::Node::slot_id(&layout_node), this);
    m_rust_slot = allocation.slot;
    m_rust_slot_generation = allocation.generation;
    m_rust_data = allocation.data;
}

Paintable::Paintable(Layout::Box const& layout_box)
    : Paintable(static_cast<Layout::NodeWithStyle const&>(layout_box))
{
}

Paintable::~Paintable()
{
    m_rust_arena->paintable_shell_destroyed(m_rust_slot, m_rust_slot_generation, this);
}

void Paintable::detach_from_layout_node(Badge<Layout::Node>)
{
    m_layout_node.clear();
    detach_chrome_widgets();
}

void Paintable::detach_chrome_widgets()
{
    if (m_horizontal_scrollbar) {
        m_horizontal_scrollbar->detach_from_paintable({});
        m_horizontal_scrollbar = nullptr;
    }
    if (m_vertical_scrollbar) {
        m_vertical_scrollbar->detach_from_paintable({});
        m_vertical_scrollbar = nullptr;
    }
    if (m_resize_handle) {
        m_resize_handle->detach_from_paintable({});
        m_resize_handle = nullptr;
    }
}

bool Paintable::has_css_transform() const
{
    return layout_node().has_css_transform();
}

StringView Paintable::class_name() const
{
    switch (kind()) {
    case Layout::RustFFI::PaintableKind::None:
        return "Paintable"sv;
    case Layout::RustFFI::PaintableKind::Paintable:
        return "Paintable"sv;
    case Layout::RustFFI::PaintableKind::PaintableWithLines:
        return "PaintableWithLines"sv;
    case Layout::RustFFI::PaintableKind::InlinePaintable:
        return "InlinePaintable"sv;
    case Layout::RustFFI::PaintableKind::ViewportPaintable:
        return "ViewportPaintable"sv;
    case Layout::RustFFI::PaintableKind::ImagePaintable:
        return "ImagePaintable"sv;
    case Layout::RustFFI::PaintableKind::CanvasPaintable:
        return "CanvasPaintable"sv;
    case Layout::RustFFI::PaintableKind::VideoPaintable:
        return "VideoPaintable"sv;
    case Layout::RustFFI::PaintableKind::CheckBoxPaintable:
        return "CheckBoxPaintable"sv;
    case Layout::RustFFI::PaintableKind::RadioButtonPaintable:
        return "RadioButtonPaintable"sv;
    case Layout::RustFFI::PaintableKind::FieldSetPaintable:
        return "FieldSetPaintable"sv;
    case Layout::RustFFI::PaintableKind::NavigableContainerViewportPaintable:
        return "NavigableContainerViewportPaintable"sv;
    case Layout::RustFFI::PaintableKind::SVGSVGPaintable:
        return "SVGSVGPaintable"sv;
    case Layout::RustFFI::PaintableKind::SVGPathPaintable:
        return "SVGPathPaintable"sv;
    case Layout::RustFFI::PaintableKind::SVGGraphicsPaintable:
        return "SVGGraphicsPaintable"sv;
    case Layout::RustFFI::PaintableKind::SVGImagePaintable:
        return "SVGImagePaintable"sv;
    case Layout::RustFFI::PaintableKind::SVGMaskPaintable:
        return "SVGMaskPaintable"sv;
    case Layout::RustFFI::PaintableKind::SVGClipPaintable:
        return "SVGClipPaintable"sv;
    case Layout::RustFFI::PaintableKind::SVGPatternPaintable:
        return "SVGPatternPaintable"sv;
    case Layout::RustFFI::PaintableKind::SVGForeignObjectPaintable:
        return "SVGForeignObjectPaintable"sv;
    }
    VERIFY_NOT_REACHED();
}

Optional<Gfx::AffineTransform> Paintable::svg_viewport_transform() const
{
    if (!rust_data().has_svg_viewport_transform)
        return {};
    auto const& transform = rust_data().svg_viewport_transform;
    return Gfx::AffineTransform { transform.a, transform.b, transform.c, transform.d, transform.e, transform.f };
}

Gfx::Path const* Paintable::committed_svg_path() const
{
    return static_cast<Gfx::Path const*>(Layout::RustFFI::layout_arena_paintable_computed_svg_path(m_rust_arena->handle(), m_rust_slot));
}

void Paintable::invalidate_paint_cache() const
{
    mirror_rust_invalidate_paint_cache(*this);
}

void Paintable::repaint_after_style_change(CSS::RequiredInvalidationAfterStyleChange const& invalidation)
{
    if (invalidation.needs_repaint())
        set_needs_repaint();
    if (invalidation.repaint_propagated_text_decorations)
        rust_invalidate_propagated_text_decoration_caches(*this);
}

void Paintable::reset_for_relayout()
{
    // A reused paintable must shed its chrome widgets: whether the box still warrants them
    // (e.g. scrollbars on a scroll container) is only known after the new layout is painted.
    detach_chrome_widgets();

    invalidate_absolute_geometry_cache(InvalidateDescendantGeometry::No);

    invalidate_stacking_context();
}

CSSPixelPoint Paintable::scroll_offset() const
{
    if (is_viewport_paintable()) {
        auto navigable = document().navigable();
        VERIFY(navigable);
        return navigable->viewport_scroll_offset();
    }

    auto const& node = layout_node();
    if (auto pseudo_element = node.generated_for_pseudo_element(); pseudo_element.has_value())
        return node.pseudo_element_generator()->scroll_offset(*pseudo_element);

    if (auto const* element = as_if<DOM::Element>(dom_node().ptr()))
        return element->scroll_offset({});
    return {};
}

CSSPixelPoint Paintable::minimum_scroll_offset() const
{
    auto scrollable_overflow_rect = this->scrollable_overflow_rect();
    if (!scrollable_overflow_rect.has_value())
        return {};

    auto scrollport_rect = absolute_padding_box_rect();
    return {
        min(scrollable_overflow_rect->left() - scrollport_rect.left(), CSSPixels(0)),
        min(scrollable_overflow_rect->top() - scrollport_rect.top(), CSSPixels(0)),
    };
}

bool Paintable::has_scrollable_overflow() const
{
    if (auto const* box = as_if<Layout::Box>(layout_node()))
        document().ensure_scrollable_overflow_is_measured(*box);
    if (rust_data().has_overflow)
        return rust_data().overflow.has_scrollable_overflow;
    return rust_data().has_cached_overflow && rust_data().cached_overflow.has_scrollable_overflow;
}

Optional<CSSPixelRect> Paintable::scrollable_overflow_rect() const
{
    if (auto const* box = as_if<Layout::Box>(layout_node()))
        document().ensure_scrollable_overflow_is_measured(*box);
    if (rust_data().has_overflow)
        return from_ffi_css_pixel_rect(rust_data().overflow.rect);
    if (!rust_data().has_cached_overflow)
        return {};
    auto scrollable_overflow_rect = from_ffi_css_pixel_rect(rust_data().cached_overflow.rect);
    scrollable_overflow_rect.translate_by(absolute_padding_box_rect().location());
    return scrollable_overflow_rect;
}

CSSPixelPoint Paintable::maximum_scroll_offset() const
{
    auto scrollable_overflow_rect = this->scrollable_overflow_rect();
    if (!scrollable_overflow_rect.has_value())
        return {};

    auto scrollport_rect = absolute_padding_box_rect();
    return {
        max(scrollable_overflow_rect->right() - scrollport_rect.right(), CSSPixels(0)),
        max(scrollable_overflow_rect->bottom() - scrollport_rect.bottom(), CSSPixels(0)),
    };
}

CSSPixelPoint Paintable::clamp_scroll_offset(CSSPixelPoint offset) const
{
    if (!scrollable_overflow_rect().has_value())
        return offset;

    auto minimum_offset = minimum_scroll_offset();
    auto maximum_offset = maximum_scroll_offset();
    return {
        clamp(offset.x(), minimum_offset.x(), maximum_offset.x()),
        clamp(offset.y(), minimum_offset.y(), maximum_offset.y()),
    };
}

Paintable::ScrollHandled Paintable::set_scroll_offset(CSSPixelPoint offset)
{
    if (!scrollable_overflow_rect().has_value())
        return ScrollHandled::No;

    offset = clamp_scroll_offset(offset);

    if (scroll_offset() == offset)
        return ScrollHandled::No;

    if (is_viewport_paintable()) {
        auto navigable = document().navigable();
        VERIFY(navigable);
        navigable->perform_scroll_of_viewport_scrolling_box(offset);
        return ScrollHandled::Yes;
    }

    document().set_needs_to_refresh_scroll_state(true);

    auto& node = layout_node();
    if (auto pseudo_element = node.generated_for_pseudo_element(); pseudo_element.has_value()) {
        node.pseudo_element_generator()->set_scroll_offset(*pseudo_element, offset);
    } else if (auto* element = as_if<DOM::Element>(*dom_node())) {
        element->set_scroll_offset({}, offset);
    } else {
        return ScrollHandled::No;
    }

    // https://drafts.csswg.org/cssom-view-1/#scrolling-events
    // Whenever an element gets scrolled (whether in response to user interaction or by an API),
    // the user agent must run these steps:

    // 1. Let doc be the element’s node document.
    auto& document = layout_node().document();

    // FIXME: 2. If the element is a snap container, run the steps to update snapchanging targets for the element with
    //           the element’s eventual snap target in the block axis as newBlockTarget and the element’s eventual snap
    //           target in the inline axis as newInlineTarget.

    auto event_target = scroll_event_target();
    if (!event_target)
        return ScrollHandled::Yes;

    // 3. If (element, "scroll") is already in doc’s pending scroll events, abort these steps.
    // 4. Append (element, "scroll") to doc’s pending scroll events.
    if (!document.append_pending_scroll_event({ *event_target, HTML::EventNames::scroll }))
        return ScrollHandled::Yes;

    set_needs_repaint(InvalidateDisplayList::No);
    return ScrollHandled::Yes;
}

Paintable::ScrollHandled Paintable::scroll_by(double delta_x, double delta_y)
{
    return set_scroll_offset_from_user_input(scroll_offset().translated(CSSPixels::nearest_value_for(delta_x), CSSPixels::nearest_value_for(delta_y)));
}

Paintable::ScrollHandled Paintable::set_scroll_offset_from_user_input(CSSPixelPoint offset)
{
    auto scroll_handled = set_scroll_offset(offset);
    auto navigable = document().navigable();
    if (!navigable)
        return scroll_handled;

    if (scroll_handled == ScrollHandled::Yes) {
        if (auto event_target = scroll_event_target())
            navigable->queue_scrollend_event_after_user_scroll(*event_target);
    } else {
        // User input keeps the scroll gesture in progress even when it does not move the scrolling box.
        navigable->defer_user_scroll_settlement();
    }
    return scroll_handled;
}

GC::Ptr<DOM::EventTarget> Paintable::scroll_event_target()
{
    auto& node = layout_node();
    if (node.generated_for_pseudo_element().has_value())
        return node.pseudo_element_generator();
    return dom_node();
}

CSSPixelRect Paintable::scroll_snapport_rect() const
{
    return scroll_snapport_rect(absolute_padding_box_rect());
}

CSSPixelRect Paintable::scroll_snapport_rect(CSSPixelRect scrollport) const
{
    Layout::NodeWithStyle const* scroll_padding_source = &layout_node();

    if (is_viewport_paintable()) {
        auto const* document_element = document().document_element();
        auto const* document_element_layout_node = document_element ? document_element->unsafe_layout_node() : nullptr;
        if (!document_element_layout_node)
            return scrollport;
        scroll_padding_source = document_element_layout_node;
    }

    // Percentages refer to the corresponding dimension of the scroll container’s scrollport.
    auto const& scroll_padding = scroll_padding_source->scroll_padding();
    scrollport.shrink(
        scroll_padding.top().to_px_or_zero(scrollport.height()),
        scroll_padding.right().to_px_or_zero(scrollport.width()),
        scroll_padding.bottom().to_px_or_zero(scrollport.height()),
        scroll_padding.left().to_px_or_zero(scrollport.width()));
    return scrollport;
}

void Paintable::scroll_into_view(CSSPixelRect rect)
{
    auto snapport = scroll_snapport_rect();
    auto current_offset = scroll_offset();

    // Both rect and snapport are in layout coordinate space (not scroll-adjusted).
    auto content_rect = rect.translated(-snapport.x(), -snapport.y());
    auto new_offset = current_offset;

    if (content_rect.right() > current_offset.x() + snapport.width())
        new_offset.set_x(content_rect.right() - snapport.width());
    else if (content_rect.left() < current_offset.x())
        new_offset.set_x(content_rect.left());

    if (content_rect.bottom() > current_offset.y() + snapport.height())
        new_offset.set_y(content_rect.bottom() - snapport.height());
    else if (content_rect.top() < current_offset.y())
        new_offset.set_y(content_rect.top());

    set_scroll_offset(new_offset);
}

void Paintable::set_offset(CSSPixelPoint offset)
{
    if (this->offset() == offset)
        return;

    rust_data().offset = { offset.x().raw_value(), offset.y().raw_value() };
    invalidate_absolute_geometry_cache(InvalidateDescendantGeometry::Yes);
}

void Paintable::set_content_size(CSSPixelSize size)
{
    auto old_size = content_size();
    rust_data().content_size = { size.width().raw_value(), size.height().raw_value() };
    invalidate_absolute_geometry_cache(InvalidateDescendantGeometry::No);
    if (auto layout_box = as_if<Layout::Box>(layout_node()))
        layout_box->did_set_content_size();
    invalidate_descendant_styles_for_container_query_size_change(*this, old_size, size);
}

void Paintable::invalidate_absolute_geometry_cache(InvalidateDescendantGeometry invalidate_descendants)
{
    m_absolute_rect.clear();
    m_absolute_padding_box_rect.clear();
    m_absolute_border_box_rect.clear();

    if (invalidate_descendants == InvalidateDescendantGeometry::No)
        return;

    for_each_child_of_type<Paintable>([](auto& child) {
        child.invalidate_absolute_geometry_cache(InvalidateDescendantGeometry::Yes);
        return IterationDecision::Continue;
    });
}

void Paintable::translate_reused_subtree_absolute_geometry(CSSPixelPoint delta)
{
    for_each_in_inclusive_subtree([&](Paintable& paintable) {
        paintable.invalidate_absolute_geometry_cache(InvalidateDescendantGeometry::No);
        Layout::RustFFI::layout_arena_paintable_translate_scrollable_overflow(paintable.m_rust_arena->handle(), paintable.m_rust_slot, { delta.x().raw_value(), delta.y().raw_value() });
        // Recorded paint commands bake absolute coordinates.
        paintable.invalidate_paint_cache();
        return TraversalDecision::Continue;
    });
}

CSSPixelPoint Paintable::offset() const
{
    return { CSSPixels::from_raw(rust_data().offset.x), CSSPixels::from_raw(rust_data().offset.y) };
}

CSSPixelRect Paintable::compute_absolute_rect() const
{
    if (is_svg_paintable()) {
        // SVG content geometry lives in the user space of the nearest ancestor viewport, and layout
        // places every box viewport-relative already, so no ancestor offsets accumulate.
        for (auto const* ancestor = layout_node().parent(); ancestor; ancestor = ancestor->parent()) {
            if (ancestor->is_svg_svg_box())
                return { offset(), content_size() };
        }
    }

    CSSPixelRect rect { offset(), content_size() };
    for (auto block = containing_block(); block; block = block->containing_block()) {
        // SVG content offsets are viewport-relative: accumulation never crosses into an enclosing
        // SVG coordinate space, and a foreignObject's own offset is the last one that applies to
        // the CSS content inside it.
        if (block->is_svg_svg_paintable() || block->is_svg_paintable())
            break;
        rect.translate_by(block->offset());
        if (block->is_svg_foreign_object_paintable())
            break;
    }
    return rect;
}

CSSPixelRect Paintable::absolute_rect() const
{
    if (!m_absolute_rect.has_value())
        m_absolute_rect = compute_absolute_rect();
    return *m_absolute_rect;
}

CSSPixelRect Paintable::absolute_padding_box_rect() const
{
    if (!m_absolute_padding_box_rect.has_value())
        m_absolute_padding_box_rect = compute_absolute_padding_box_rect();
    return *m_absolute_padding_box_rect;
}

CSSPixelRect Paintable::compute_absolute_padding_box_rect() const
{
    auto absolute_rect = this->absolute_rect();
    CSSPixelRect rect;
    rect.set_x(absolute_rect.x() - box_model().padding.left);
    rect.set_width(content_width() + box_model().padding.left + box_model().padding.right);
    rect.set_y(absolute_rect.y() - box_model().padding.top);
    rect.set_height(content_height() + box_model().padding.top + box_model().padding.bottom);
    return rect;
}

Optional<CSSPixelRect> Paintable::absolute_resizer_rect(ChromeMetrics const& metrics) const
{
    if (!has_resizer())
        return {};
    auto padding_rect = absolute_padding_box_rect();
    CSSPixels x = is_chrome_mirrored() ? padding_rect.x() : padding_rect.right() - metrics.resize_gripper_size;
    CSSPixels y = padding_rect.bottom() - metrics.resize_gripper_size;
    return CSSPixelRect { x, y, metrics.resize_gripper_size, metrics.resize_gripper_size };
}

CSSPixelRect Paintable::absolute_border_box_rect() const
{
    if (!m_absolute_border_box_rect.has_value())
        m_absolute_border_box_rect = compute_absolute_border_box_rect();
    return *m_absolute_border_box_rect;
}

CSSPixelRect Paintable::compute_absolute_border_box_rect() const
{
    auto padded_rect = this->absolute_padding_box_rect();
    CSSPixelRect rect;
    auto use_collapsing_borders_model = uses_collapsing_borders_model();
    // Implement the collapsing border model https://www.w3.org/TR/CSS22/tables.html#collapsing-borders.
    auto border_top = box_model().border.top;
    auto border_bottom = box_model().border.bottom;
    auto border_left = box_model().border.left;
    auto border_right = box_model().border.right;
    if (use_collapsing_borders_model) {
        border_top = round(border_top / 2);
        border_bottom = round(border_bottom / 2);
        border_left = round(border_left / 2);
        border_right = round(border_right / 2);
    }
    rect.set_x(padded_rect.x() - border_left);
    rect.set_width(padded_rect.width() + border_left + border_right);
    rect.set_y(padded_rect.y() - border_top);
    rect.set_height(padded_rect.height() + border_top + border_bottom);
    return rect;
}

RefPtr<Scrollbar> Paintable::scrollbar(ScrollDirection direction) const
{
    return direction == ScrollDirection::Horizontal ? m_horizontal_scrollbar : m_vertical_scrollbar;
}

NonnullRefPtr<Scrollbar> Paintable::ensure_scrollbar(ScrollDirection direction)
{
    auto& slot = direction == ScrollDirection::Horizontal ? m_horizontal_scrollbar : m_vertical_scrollbar;
    if (!slot)
        slot = Scrollbar::create(const_cast<Paintable&>(*this), direction);
    return *slot;
}

static CSS::Overflow overflow_value_applied_to_viewport_for_wheel_scrolling(DOM::Document const& document, Paintable::ScrollDirection direction)
{
    auto overflow_for_direction = [direction](CSS::ComputedValues::BoxValues const& style) {
        return direction == Paintable::ScrollDirection::Horizontal
            ? static_cast<CSS::Overflow>(style.overflow_x)
            : static_cast<CSS::Overflow>(style.overflow_y);
    };
    auto has_containment = [](CSS::ComputedValues::BoxValues const& style) {
        return style.size_containment || style.inline_size_containment || style.layout_containment || style.style_containment || style.paint_containment;
    };

    auto* root_element = document.document_element();
    auto const* root_style = root_element ? root_element->style_group<CSS::ComputedValues::BoxValues>() : nullptr;
    if (!root_style)
        return CSS::Overflow::Auto;

    auto const* overflow_origin = root_style;
    if (root_element->is_html_html_element() && !has_containment(*root_style)) {
        auto root_overflow_x = static_cast<CSS::Overflow>(root_style->overflow_x);
        auto root_overflow_y = static_cast<CSS::Overflow>(root_style->overflow_y);
        if (root_overflow_x == CSS::Overflow::Visible && root_overflow_y == CSS::Overflow::Visible) {
            auto* body_element = root_element->first_child_of_type<HTML::HTMLBodyElement>();
            auto const* body_style = body_element ? body_element->style_group<CSS::ComputedValues::BoxValues>() : nullptr;
            if (body_style && !has_containment(*body_style))
                overflow_origin = body_style;
        }
    }

    auto overflow = overflow_for_direction(*overflow_origin);
    if (overflow == CSS::Overflow::Visible)
        return CSS::Overflow::Auto;
    if (overflow == CSS::Overflow::Clip)
        return CSS::Overflow::Hidden;
    return overflow;
}

bool Paintable::could_be_scrolled_by_wheel_event(ScrollDirection direction) const
{
    bool is_horizontal = direction == ScrollDirection::Horizontal;
    Gfx::Orientation orientation = is_horizontal ? Gfx::Orientation::Horizontal : Gfx::Orientation::Vertical;
    auto overflow = is_horizontal ? layout_node().overflow_x() : layout_node().overflow_y();
    if (is_viewport_paintable())
        overflow = overflow_value_applied_to_viewport_for_wheel_scrolling(document(), direction);

    if (overflow != CSS::Overflow::Auto && overflow != CSS::Overflow::Scroll)
        return false;

    auto scrollable_overflow_rect = this->scrollable_overflow_rect();
    if (!scrollable_overflow_rect.has_value())
        return false;

    CSSPixels scrollable_overflow_size = scrollable_overflow_rect->primary_size_for_orientation(orientation);
    CSSPixels scrollport_size = absolute_padding_box_rect().primary_size_for_orientation(orientation);

    return scrollable_overflow_size > scrollport_size;
}

bool Paintable::could_be_scrolled_by_wheel_event() const
{
    return could_be_scrolled_by_wheel_event(ScrollDirection::Horizontal) || could_be_scrolled_by_wheel_event(ScrollDirection::Vertical);
}

CSSPixels Paintable::available_scrollbar_length(ScrollDirection direction, ChromeMetrics const& metrics) const
{
    bool is_horizontal = direction == ScrollDirection::Horizontal;
    auto padding_rect = absolute_padding_box_rect();
    CSSPixels full_scrollport_length = is_horizontal ? padding_rect.width() : padding_rect.height();
    if (has_resizer())
        full_scrollport_length -= metrics.resize_gripper_size;
    else {
        if (is_horizontal && could_be_scrolled_by_wheel_event(ScrollDirection::Vertical))
            full_scrollport_length -= metrics.scroll_gutter_thickness;
        if (!is_horizontal && could_be_scrolled_by_wheel_event(ScrollDirection::Horizontal))
            full_scrollport_length -= metrics.scroll_gutter_thickness;
    }
    return full_scrollport_length;
}

Optional<CSSPixelRect> Paintable::absolute_scrollbar_rect(ScrollDirection direction, bool with_gutter, ChromeMetrics const& metrics) const
{
    if (!could_be_scrolled_by_wheel_event(direction))
        return {};

    if (layout_node().scrollbar_width() == CSS::ScrollbarWidth::None)
        return {};

    bool is_horizontal = direction == ScrollDirection::Horizontal;
    bool adjusting_for_resizer = has_resizer();

    CSSPixels rect_thickness = with_gutter
        ? metrics.scroll_gutter_thickness
        : metrics.scroll_thumb_thickness_thin + metrics.scroll_thumb_padding_thin;
    CSSPixelRect scrollbar_rect = absolute_padding_box_rect();

    if (is_horizontal) {
        if (!adjusting_for_resizer && could_be_scrolled_by_wheel_event(ScrollDirection::Vertical)) {
            scrollbar_rect.set_width(max(CSSPixels { 0 }, scrollbar_rect.width() - metrics.scroll_gutter_thickness));
            if (is_chrome_mirrored())
                scrollbar_rect.set_x(scrollbar_rect.x() + metrics.scroll_gutter_thickness);
        } else if (adjusting_for_resizer) {
            scrollbar_rect.set_width(available_scrollbar_length(ScrollDirection::Horizontal, metrics));
            if (is_chrome_mirrored())
                scrollbar_rect.set_x(scrollbar_rect.x() + metrics.resize_gripper_size);
        }
        scrollbar_rect.set_y(max(CSSPixels { 0 }, scrollbar_rect.bottom() - rect_thickness));
        scrollbar_rect.set_height(rect_thickness);
    } else {
        if (adjusting_for_resizer)
            scrollbar_rect.set_height(available_scrollbar_length(ScrollDirection::Vertical, metrics));
        if (!is_chrome_mirrored())
            scrollbar_rect.set_x(max(CSSPixels { 0 }, scrollbar_rect.right() - rect_thickness));
        scrollbar_rect.set_width(rect_thickness);
    }
    return scrollbar_rect;
}

Optional<Paintable::ScrollbarData> Paintable::compute_scrollbar_data(ScrollDirection direction, ChromeMetrics const& metrics, ScrollStateSnapshot const* scroll_state_snapshot, ScrollbarSizing scrollbar_sizing) const
{
    bool is_horizontal = direction == ScrollDirection::Horizontal;
    auto orientation = is_horizontal ? Gfx::Orientation::Horizontal : Gfx::Orientation::Vertical;
    auto overflow = is_horizontal ? layout_node().overflow_x() : layout_node().overflow_y();

    if (overflow != CSS::Overflow::Scroll && !could_be_scrolled_by_wheel_event(direction))
        return {};

    if (!own_scroll_node_index().value())
        return {};

    auto scrollable_overflow_rect = this->scrollable_overflow_rect();
    if (!scrollable_overflow_rect.has_value())
        return {};

    CSSPixels scrollable_overflow_length = scrollable_overflow_rect->primary_size_for_orientation(orientation);
    if (scrollable_overflow_length == 0)
        return {};

    auto const& scrollbar = is_horizontal ? m_horizontal_scrollbar : m_vertical_scrollbar;
    bool with_gutter = [&] {
        switch (scrollbar_sizing) {
        case ScrollbarSizing::Current:
            return scrollbar && scrollbar->is_enlarged();
        case ScrollbarSizing::Regular:
            return false;
        case ScrollbarSizing::Enlarged:
            return true;
        }
        VERIFY_NOT_REACHED();
    }();
    auto scrollbar_rect = absolute_scrollbar_rect(direction, with_gutter, metrics);
    if (!scrollbar_rect.has_value())
        return {};

    CSSPixels thumb_thickness = metrics.scroll_thumb_thickness_thin;
    CSSPixels thumb_margin = metrics.scroll_thumb_padding_thin;
    if (with_gutter) {
        thumb_thickness = metrics.scroll_thumb_thickness;
        thumb_margin = CSSPixels { (metrics.scroll_gutter_thickness - metrics.scroll_thumb_thickness) / 2.0 };
    }
    CSSPixels scrollbar_length = scrollbar_rect->primary_size_for_orientation(orientation);
    CSSPixels usable_scrollbar_length = max(CSSPixels { 0 }, scrollbar_length - (2 * thumb_margin));
    CSSPixels scrollport_size = absolute_padding_box_rect().primary_size_for_orientation(orientation);
    CSSPixels min_thumb_length = min(usable_scrollbar_length, metrics.scroll_thumb_min_length);
    CSSPixels thumb_length = max(usable_scrollbar_length * (scrollport_size / scrollable_overflow_length), min_thumb_length);

    ScrollbarData scrollbar_data = { .gutter_rect = {}, .thumb_rect = scrollbar_rect.value(), .track_rect = scrollbar_rect.value(), .thumb_travel_to_scroll_ratio = 0 };

    if (scrollable_overflow_length > scrollport_size)
        scrollbar_data.thumb_travel_to_scroll_ratio = (usable_scrollbar_length - thumb_length) / (scrollable_overflow_length - scrollport_size);

    scrollbar_data.thumb_rect.set_primary_size_for_orientation(orientation, thumb_length);
    scrollbar_data.thumb_rect.set_secondary_size_for_orientation(orientation, thumb_thickness);
    auto minimum_offset = minimum_scroll_offset().primary_offset_for_orientation(orientation);
    scrollbar_data.thumb_rect.translate_primary_offset_for_orientation(orientation, thumb_margin - minimum_offset * scrollbar_data.thumb_travel_to_scroll_ratio);
    if (with_gutter || (!is_horizontal && is_chrome_mirrored()))
        scrollbar_data.thumb_rect.translate_secondary_offset_for_orientation(orientation, thumb_margin);
    if (with_gutter)
        scrollbar_data.gutter_rect = scrollbar_rect.value();

    if (scroll_state_snapshot) {
        auto own_offset = scroll_state_snapshot->device_offset_for_index(own_scroll_node_index());
        auto device_scroll_offset = is_horizontal ? -own_offset.x() : -own_offset.y();
        auto device_pixels_per_css_pixel = static_cast<float>(document().page().client().device_pixels_per_css_pixel());
        CSSPixels thumb_offset = CSSPixels::nearest_value_for(device_scroll_offset / device_pixels_per_css_pixel) * scrollbar_data.thumb_travel_to_scroll_ratio;
        scrollbar_data.thumb_rect.translate_primary_offset_for_orientation(orientation, thumb_offset);
    }

    return scrollbar_data;
}

Optional<UsedGridTrackList> Paintable::used_values_for_grid_template_columns() const
{
    Optional<UsedGridTrackList> result;
    Layout::RustFFI::layout_arena_paintable_used_grid_tracks(rust_arena().handle(), rust_slot(), &result,
        [](void* context, Layout::RustFFI::FfiUsedGridTrackList const* columns, Layout::RustFFI::FfiUsedGridTrackList const*) {
            *static_cast<Optional<UsedGridTrackList>*>(context) = Layout::build_used_grid_track_list(*columns);
        });
    return result;
}

Optional<UsedGridTrackList> Paintable::used_values_for_grid_template_rows() const
{
    Optional<UsedGridTrackList> result;
    Layout::RustFFI::layout_arena_paintable_used_grid_tracks(rust_arena().handle(), rust_slot(), &result,
        [](void* context, Layout::RustFFI::FfiUsedGridTrackList const*, Layout::RustFFI::FfiUsedGridTrackList const* rows) {
            *static_cast<Optional<UsedGridTrackList>*>(context) = Layout::build_used_grid_track_list(*rows);
        });
    return result;
}

Optional<String> Paintable::grid_layout_json(UniqueNodeID container_node_id) const
{
    Optional<String> result;
    Layout::RustFFI::layout_arena_paintable_grid_layout_json(rust_arena().handle(), rust_slot(), container_node_id.value(), &result,
        [](void* context, u8 const* bytes, size_t length) {
            *static_cast<Optional<String>*>(context) = MUST(String::from_utf8(StringView { bytes, length }));
        });
    return result;
}

Optional<String> Paintable::flex_layout_json(UniqueNodeID container_node_id) const
{
    Optional<String> result;
    Layout::RustFFI::layout_arena_paintable_flex_layout_json(rust_arena().handle(), rust_slot(), container_node_id.value(), &result,
        [](void* context, u8 const* bytes, size_t length) {
            *static_cast<Optional<String>*>(context) = MUST(String::from_utf8(StringView { bytes, length }));
        });
    return result;
}

void Paintable::invalidate_stacking_context()
{
    if (!has_layout_node())
        return;
    if (auto viewport_paintable = document().unsafe_paintable())
        viewport_paintable->invalidate_stacking_context_tree();
}

Optional<int> Paintable::effective_z_index() const
{
    // https://drafts.csswg.org/css2/#z-index
    // Applies to: positioned elements
    if (is_positioned())
        return layout_node().z_index();

    return {};
}

Optional<CSSPixelPoint> Paintable::transform_point_to_local(CSSPixelPoint screen_position) const
{
    auto viewport_paintable = document().paintable();
    if (!viewport_paintable)
        return screen_position;
    auto pixel_ratio = static_cast<float>(document().page().client().device_pixels_per_css_pixel());
    auto const& scroll_state = viewport_paintable->scroll_state_snapshot();
    auto const& visual_context_tree = viewport_paintable->visual_context_tree();
    auto result = visual_context_tree.transform_point_for_hit_test(accumulated_visual_context_index(), screen_position.to_type<float>() * pixel_ratio, scroll_state);
    if (!result.has_value())
        return {};
    return (*result / pixel_ratio).to_type<CSSPixels>();
}

Optional<CSSPixelPoint> Paintable::transform_point_to_local_for_descendants(CSSPixelPoint screen_position) const
{
    auto viewport_paintable = document().paintable();
    if (!viewport_paintable)
        return screen_position;
    auto pixel_ratio = static_cast<float>(document().page().client().device_pixels_per_css_pixel());
    auto const& scroll_state = viewport_paintable->scroll_state_snapshot();
    auto const& visual_context_tree = viewport_paintable->visual_context_tree();
    auto result = visual_context_tree.transform_point_for_hit_test(accumulated_visual_context_for_descendants_index(), screen_position.to_type<float>() * pixel_ratio, scroll_state);
    if (!result.has_value())
        return {};
    return (*result / pixel_ratio).to_type<CSSPixels>();
}

CSSPixelRect Paintable::transform_rect_to_viewport(CSSPixelRect const& rect, AccumulatedVisualContextTree::IncludeVisualViewportTransform include_visual_viewport_transform) const
{
    auto viewport_paintable = document().paintable();
    if (!viewport_paintable)
        return rect;
    auto pixel_ratio = static_cast<float>(document().page().client().device_pixels_per_css_pixel());
    auto const& scroll_state = viewport_paintable->scroll_state_snapshot();
    auto const& visual_context_tree = viewport_paintable->visual_context_tree();
    auto result = visual_context_tree.transform_rect_to_viewport(accumulated_visual_context_index(), rect.to_type<float>() * pixel_ratio, scroll_state, include_visual_viewport_transform);
    return (result * (1.f / pixel_ratio)).to_type<CSSPixels>();
}

CSSPixelPoint Paintable::inverse_transform_point(CSSPixelPoint screen_position) const
{
    auto viewport_paintable = document().paintable();
    if (!viewport_paintable)
        return screen_position;
    auto pixel_ratio = static_cast<float>(document().page().client().device_pixels_per_css_pixel());
    auto const& visual_context_tree = viewport_paintable->visual_context_tree();
    auto result = visual_context_tree.inverse_transform_point(accumulated_visual_context_index(), screen_position.to_type<float>() * pixel_ratio);
    return (result / pixel_ratio).to_type<CSSPixels>();
}

CSSPixelPoint Paintable::transform_to_local_coordinates(CSSPixelPoint screen_position) const
{
    return transform_point_to_local(screen_position).value_or(screen_position);
}

bool Paintable::has_resizer() const
{
    // https://drafts.csswg.org/css-ui#resize
    if (is_viewport_paintable())
        return false;

    // The effect of the resize property on generated content is undefined.
    // Implementations should not apply the resize property to generated content.

    if (layout_node().generated_for_pseudo_element().has_value())
        return false;

    auto axes = physical_resize_axes();
    return axes.horizontal || axes.vertical;
}

bool Paintable::is_chrome_mirrored() const
{
    auto writing_mode = layout_node().writing_mode();
    return (writing_mode == CSS::WritingMode::HorizontalTb && layout_node().direction() == CSS::Direction::Rtl)
        || writing_mode == CSS::WritingMode::VerticalRl
        || writing_mode == CSS::WritingMode::SidewaysRl;
}

RefPtr<ResizeHandle> Paintable::resize_handle() const
{
    return m_resize_handle;
}

NonnullRefPtr<ResizeHandle> Paintable::ensure_resize_handle()
{
    if (!m_resize_handle)
        m_resize_handle = ResizeHandle::create(*this);
    return *m_resize_handle;
}

bool Paintable::handle_mousewheel(Badge<EventHandler>, CSSPixelPoint, unsigned, unsigned, double wheel_delta_x, double wheel_delta_y)
{
    auto can_scroll_horizontally = could_be_scrolled_by_wheel_event(ScrollDirection::Horizontal);
    auto can_scroll_vertically = could_be_scrolled_by_wheel_event(ScrollDirection::Vertical);
    if (!can_scroll_horizontally)
        wheel_delta_x = 0;
    if (!can_scroll_vertically)
        wheel_delta_y = 0;

    // if none of the axes we scrolled with can be accepted by this element, don't handle scroll.
    if (wheel_delta_x == 0 && wheel_delta_y == 0)
        return false;

    return scroll_by(wheel_delta_x, wheel_delta_y) == ScrollHandled::Yes;
}

bool Paintable::resizer_contains(CSSPixelPoint adjusted_position, ChromeMetrics const& metrics) const
{
    auto handle_rect = absolute_resizer_rect(metrics);
    if (!handle_rect.has_value())
        return false;
    bool bottom_left_resizer = is_chrome_mirrored();
    handle_rect->inflate(0, bottom_left_resizer ? 0 : box_model().border.right, box_model().border.bottom, bottom_left_resizer ? box_model().border.left : 0);

    return handle_rect->contains(adjusted_position);
}

void Paintable::set_needs_repaint(InvalidateDisplayList should_invalidate_display_list)
{
    if (should_invalidate_display_list == InvalidateDisplayList::Yes) {
        invalidate_paint_cache();

        // Recurse into anonymous child nodes so we properly invalidate nested contents of e.g. <button>s.
        for_each_child_of_type<Paintable>([&](auto& child) {
            if (child.layout_node().is_anonymous())
                child.set_needs_repaint(should_invalidate_display_list);
            return IterationDecision::Continue;
        });

        // The root element paints the body's propagated background, so a body repaint must also refresh the
        // root's cached background. A root repaint can conversely flip whether propagation applies, changing
        // what the body itself paints.
        if (body_background_is_propagated_to_root(layout_node())) {
            if (auto const* document_element = document().document_element()) {
                if (auto document_element_paintable = document_element->unsafe_paintable())
                    document_element_paintable->invalidate_paint_cache();
            }
        } else if (layout_node().is_root_element()) {
            if (auto const* body = document().body()) {
                if (auto body_paintable = body->unsafe_paintable())
                    body_paintable->invalidate_paint_cache();
            }
        }
    }
    document().set_needs_repaint(Badge<Painting::Paintable> {}, should_invalidate_display_list);
}

// https://www.w3.org/TR/css-transforms-1/#reference-box
CSSPixelRect Paintable::transform_reference_box() const
{
    auto transform_box = layout_node().transform_box();
    // For SVG elements without associated CSS layout box, the used value for content-box is fill-box and for
    // border-box is stroke-box.
    // FIXME: This currently detects any SVG element except the <svg> one. Is that correct?
    //        And is it correct to use `else` below?
    if (is_svg_paintable()) {
        switch (transform_box) {
        case CSS::TransformBox::ContentBox:
            transform_box = CSS::TransformBox::FillBox;
            break;
        case CSS::TransformBox::BorderBox:
            transform_box = CSS::TransformBox::StrokeBox;
            break;
        default:
            break;
        }
    }
    // For elements with associated CSS layout box, the used value for fill-box is content-box and for
    // stroke-box and view-box is border-box.
    else {
        switch (transform_box) {
        case CSS::TransformBox::FillBox:
            transform_box = CSS::TransformBox::ContentBox;
            break;
        case CSS::TransformBox::StrokeBox:
        case CSS::TransformBox::ViewBox:
            transform_box = CSS::TransformBox::BorderBox;
            break;
        default:
            break;
        }
    }

    switch (transform_box) {
    case CSS::TransformBox::ContentBox:
        // Uses the content box as reference box.
        // FIXME: The reference box of a table is the border box of its table wrapper box, not its table box.
        return absolute_rect();
    case CSS::TransformBox::BorderBox:
        // Uses the border box as reference box.
        // FIXME: The reference box of a table is the border box of its table wrapper box, not its table box.
        return absolute_border_box_rect();
    case CSS::TransformBox::FillBox:
        // Uses the object bounding box as reference box.
        // FIXME: For now we're using the content rect as an approximation.
        return absolute_rect();
    case CSS::TransformBox::StrokeBox:
        // Uses the stroke bounding box as reference box.
        // FIXME: For now we're using the border rect as an approximation.
        return absolute_border_box_rect();
    case CSS::TransformBox::ViewBox: {
        // Uses the nearest SVG viewport as reference box.
        // FIXME: If a viewBox attribute is specified for the SVG viewport creating element:
        //  - The reference box is positioned at the origin of the coordinate system established by the viewBox attribute.
        //  - The dimension of the reference box is set to the width and height values of the viewBox attribute.
        auto const* viewport_paintable = nearest_svg_viewport_paintable_of(layout_node());
        if (!viewport_paintable)
            return absolute_border_box_rect();
        return svg_viewport_user_rect(*viewport_paintable).to_type<CSSPixels>();
    }
    }
    VERIFY_NOT_REACHED();
}

BorderRadiiData Paintable::border_radii_data() const
{
    if (!layout_node().has_noninitial_border_radii())
        return {};
    CSSPixelRect const border_rect { 0, 0, border_box_width(), border_box_height() };
    return normalize_border_radii_data(border_rect, border_rect,
        layout_node().border_top_left_radius(), layout_node().border_top_right_radius(),
        layout_node().border_bottom_right_radius(), layout_node().border_bottom_left_radius());
}

static Optional<BordersData> borders_data_for_outline(Layout::Node const& layout_node, Color outline_color, CSS::OutlineStyle outline_style, CSSPixels outline_width)
{
    CSS::LineStyle line_style;
    if (outline_style == CSS::OutlineStyle::Auto) {
        line_style = CSS::LineStyle::Solid;
        outline_color = CSS::KeywordStyleValue::create(CSS::Keyword::Accentcolor)->to_color(CSS::ColorResolutionContext::for_layout_node_with_style(*static_cast<Layout::NodeWithStyle const*>(&layout_node))).value();
        outline_width = 2;
    } else {
        line_style = CSS::keyword_to_line_style(CSS::to_keyword(outline_style)).value_or(CSS::LineStyle::None);
    }

    if (outline_color.alpha() == 0 || line_style == CSS::LineStyle::None || outline_width == 0)
        return {};

    CSS::BorderData border_data {
        .color = outline_color,
        .line_style = line_style,
        .width = outline_width,
    };
    return BordersData { border_data, border_data, border_data, border_data };
}

Optional<BordersData> Paintable::outline_data() const
{
    // The `auto` outline is the UA focus ring; like native controls, it is only shown while the window has focus.
    if (layout_node().outline_style() == CSS::OutlineStyle::Auto && (!navigable() || !navigable()->is_focused()))
        return {};

    return borders_data_for_outline(layout_node(), layout_node().outline_color(), layout_node().outline_style(), layout_node().outline_width());
}

Optional<BordersData> Paintable::outline_data(CSS::ComputedValues const& computed_values) const
{
    // The `auto` outline is the UA focus ring; like native controls, it is only shown while the window has focus.
    if (computed_values.outline_style() == CSS::OutlineStyle::Auto && (!navigable() || !navigable()->is_focused()))
        return {};

    return borders_data_for_outline(layout_node(), computed_values.outline_color(), computed_values.outline_style(), computed_values.outline_width());
}

CSSPixels Paintable::outline_offset() const
{
    return layout_node().outline_offset();
}

RefPtr<Paintable const> Paintable::nearest_scrollable_ancestor() const
{
    auto paintable = this->containing_block();
    while (paintable) {
        if (paintable->could_be_scrolled_by_wheel_event())
            return paintable;
        if (paintable->is_fixed_position())
            return nullptr;
        paintable = paintable->containing_block();
    }
    return nullptr;
}

Paintable::PhysicalResizeAxes Paintable::physical_resize_axes() const
{
    // https://drafts.csswg.org/css-ui/#resize
    if (layout_node().resize() == CSS::Resize::None)
        return {};

    // 4.1. ... The resize property applies to elements that are scroll containers. UAs may also apply it,
    // regardless of the value of the overflow property, to:
    // - Replaced elements representing images or videos, such as img, video, picture, svg, object, or canvas.
    // - The <iframe> element.
    if (layout_node().display().is_inline_outside() && layout_node().display().is_flow_inside())
        return {};

    bool horizontal_writing_mode = layout_node().writing_mode() == CSS::WritingMode::HorizontalTb;

    return {
        .horizontal = layout_node().overflow_x() != CSS::Overflow::Visible
            && layout_node().overflow_x() != CSS::Overflow::Clip
            && (layout_node().resize() == CSS::Resize::Both
                || layout_node().resize() == CSS::Resize::Horizontal
                || (layout_node().resize() == CSS::Resize::Inline && horizontal_writing_mode)
                || (layout_node().resize() == CSS::Resize::Block && !horizontal_writing_mode)),
        .vertical = layout_node().overflow_y() != CSS::Overflow::Visible
            && layout_node().overflow_y() != CSS::Overflow::Clip
            && (layout_node().resize() == CSS::Resize::Both
                || layout_node().resize() == CSS::Resize::Vertical
                || (layout_node().resize() == CSS::Resize::Inline && !horizontal_writing_mode)
                || (layout_node().resize() == CSS::Resize::Block && horizontal_writing_mode))
    };
}

}
