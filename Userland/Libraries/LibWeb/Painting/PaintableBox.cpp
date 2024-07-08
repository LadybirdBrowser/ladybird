/*
 * Copyright (c) 2022-2023, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2022-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericShorthands.h>
#include <LibGfx/Font/ScaledFont.h>
#include <LibGfx/Painter.h>
#include <LibUnicode/CharacterTypes.h>
#include <LibWeb/CSS/SystemColor.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLHtmlElement.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/BackgroundPainting.h>
#include <LibWeb/Painting/FilterPainting.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/StackingContext.h>
#include <LibWeb/Painting/TextPaintable.h>
#include <LibWeb/Painting/ViewportPaintable.h>
#include <LibWeb/Platform/FontPlugin.h>

namespace Web::Painting {

JS::NonnullGCPtr<PaintableWithLines> PaintableWithLines::create(Layout::BlockContainer const& block_container)
{
    return block_container.heap().allocate_without_realm<PaintableWithLines>(block_container);
}

JS::NonnullGCPtr<PaintableBox> PaintableBox::create(Layout::Box const& layout_box)
{
    return layout_box.heap().allocate_without_realm<PaintableBox>(layout_box);
}

PaintableBox::PaintableBox(Layout::Box const& layout_box)
    : Paintable(layout_box)
{
}

PaintableBox::~PaintableBox()
{
}

PaintableWithLines::PaintableWithLines(Layout::BlockContainer const& layout_box)
    : PaintableBox(layout_box)
{
}

PaintableWithLines::~PaintableWithLines()
{
}

CSSPixelPoint PaintableBox::scroll_offset() const
{
    if (is_viewport()) {
        auto navigable = document().navigable();
        VERIFY(navigable);
        return navigable->viewport_scroll_offset();
    }

    auto const& node = layout_node();
    if (node.is_generated_for_before_pseudo_element())
        return node.pseudo_element_generator()->scroll_offset(DOM::Element::ScrollOffsetFor::PseudoBefore);
    if (node.is_generated_for_after_pseudo_element())
        return node.pseudo_element_generator()->scroll_offset(DOM::Element::ScrollOffsetFor::PseudoAfter);

    if (!(dom_node() && is<DOM::Element>(*dom_node())))
        return {};

    return static_cast<DOM::Element const*>(dom_node())->scroll_offset(DOM::Element::ScrollOffsetFor::Self);
}

void PaintableBox::set_scroll_offset(CSSPixelPoint offset)
{
    auto scrollable_overflow_rect = this->scrollable_overflow_rect();
    if (!scrollable_overflow_rect.has_value())
        return;

    document().set_needs_to_refresh_clip_state(true);
    document().set_needs_to_refresh_scroll_state(true);

    auto max_x_offset = scrollable_overflow_rect->width() - content_size().width();
    auto max_y_offset = scrollable_overflow_rect->height() - content_size().height();
    offset.set_x(clamp(offset.x(), 0, max_x_offset));
    offset.set_y(clamp(offset.y(), 0, max_y_offset));

    // FIXME: If there is horizontal and vertical scroll ignore only part of the new offset
    if (offset.y() < 0 || scroll_offset() == offset)
        return;

    auto& node = layout_node();
    if (node.is_generated_for_before_pseudo_element()) {
        node.pseudo_element_generator()->set_scroll_offset(DOM::Element::ScrollOffsetFor::PseudoBefore, offset);
    } else if (node.is_generated_for_after_pseudo_element()) {
        node.pseudo_element_generator()->set_scroll_offset(DOM::Element::ScrollOffsetFor::PseudoAfter, offset);
    } else if (is<DOM::Element>(*dom_node())) {
        static_cast<DOM::Element*>(dom_node())->set_scroll_offset(DOM::Element::ScrollOffsetFor::Self, offset);
    } else {
        return;
    }

    // https://drafts.csswg.org/cssom-view-1/#scrolling-events
    // Whenever an element gets scrolled (whether in response to user interaction or by an API),
    // the user agent must run these steps:

    // 1. Let doc be the element’s node document.
    auto& document = layout_box().document();

    // FIXME: 2. If the element is a snap container, run the steps to update snapchanging targets for the element with
    //           the element’s eventual snap target in the block axis as newBlockTarget and the element’s eventual snap
    //           target in the inline axis as newInlineTarget.

    JS::NonnullGCPtr<DOM::EventTarget> const event_target = *dom_node();

    // 3. If the element is already in doc’s pending scroll event targets, abort these steps.
    if (document.pending_scroll_event_targets().contains_slow(event_target))
        return;

    // 4. Append the element to doc’s pending scroll event targets.
    document.pending_scroll_event_targets().append(*layout_box().dom_node());

    set_needs_display();
}

void PaintableBox::scroll_by(int delta_x, int delta_y)
{
    set_scroll_offset(scroll_offset().translated(delta_x, delta_y));
}

void PaintableBox::set_offset(CSSPixelPoint offset)
{
    m_offset = offset;
}

void PaintableBox::set_content_size(CSSPixelSize size)
{
    m_content_size = size;
    layout_box().did_set_content_size();
}

CSSPixelPoint PaintableBox::offset() const
{
    return m_offset;
}

CSSPixelRect PaintableBox::compute_absolute_rect() const
{
    CSSPixelRect rect { offset(), content_size() };
    for (auto const* block = containing_block(); block; block = block->containing_block())
        rect.translate_by(block->offset());
    return rect;
}

CSSPixelRect PaintableBox::compute_absolute_padding_rect_with_css_transform_applied() const
{
    auto rect = absolute_rect();
    auto scroll_offset = this->enclosing_scroll_frame_offset();
    if (scroll_offset.has_value())
        rect.translate_by(scroll_offset.value());
    rect.translate_by(combined_css_transform().translation().to_type<CSSPixels>());

    CSSPixelRect padding_rect;
    padding_rect.set_x(rect.x() - box_model().padding.left);
    padding_rect.set_width(content_width() + box_model().padding.left + box_model().padding.right);
    padding_rect.set_y(rect.y() - box_model().padding.top);
    padding_rect.set_height(content_height() + box_model().padding.top + box_model().padding.bottom);
    return padding_rect;
}

CSSPixelRect PaintableBox::absolute_rect() const
{
    if (!m_absolute_rect.has_value())
        m_absolute_rect = compute_absolute_rect();
    return *m_absolute_rect;
}

CSSPixelRect PaintableBox::compute_absolute_paint_rect() const
{
    // FIXME: This likely incomplete:
    auto rect = absolute_border_box_rect();
    if (has_scrollable_overflow()) {
        auto scrollable_overflow_rect = this->scrollable_overflow_rect().value();
        if (computed_values().overflow_x() == CSS::Overflow::Visible)
            rect.unite_horizontally(scrollable_overflow_rect);
        if (computed_values().overflow_y() == CSS::Overflow::Visible)
            rect.unite_vertically(scrollable_overflow_rect);
    }
    for (auto const& shadow : box_shadow_data()) {
        if (shadow.placement == ShadowPlacement::Inner)
            continue;
        auto inflate = shadow.spread_distance + shadow.blur_radius;
        auto shadow_rect = rect.inflated(inflate, inflate, inflate, inflate).translated(shadow.offset_x, shadow.offset_y);
        rect = rect.united(shadow_rect);
    }
    return rect;
}

CSSPixelRect PaintableBox::absolute_paint_rect() const
{
    if (!m_absolute_paint_rect.has_value())
        m_absolute_paint_rect = compute_absolute_paint_rect();
    return *m_absolute_paint_rect;
}

Optional<CSSPixelRect> PaintableBox::get_clip_rect() const
{
    auto clip = computed_values().clip();
    if (clip.is_rect() && layout_box().is_absolutely_positioned()) {
        auto border_box = absolute_border_box_rect();
        return clip.to_rect().resolved(layout_node(), border_box);
    }
    return {};
}

bool PaintableBox::wants_mouse_events() const
{
    if (scroll_thumb_rect(ScrollDirection::Vertical).has_value())
        return true;
    if (scroll_thumb_rect(ScrollDirection::Horizontal).has_value())
        return true;
    return false;
}

void PaintableBox::before_paint(PaintContext& context, [[maybe_unused]] PaintPhase phase) const
{
    if (!is_visible())
        return;

    apply_clip_overflow_rect(context, phase);
    apply_scroll_offset(context, phase);
}

void PaintableBox::after_paint(PaintContext& context, [[maybe_unused]] PaintPhase phase) const
{
    if (!is_visible())
        return;

    reset_scroll_offset(context, phase);
    clear_clip_overflow_rect(context, phase);
}

bool PaintableBox::is_scrollable(ScrollDirection direction) const
{
    auto overflow = direction == ScrollDirection::Horizontal ? computed_values().overflow_x() : computed_values().overflow_y();
    auto scrollable_overflow_size = direction == ScrollDirection::Horizontal ? scrollable_overflow_rect()->width() : scrollable_overflow_rect()->height();
    auto scrollport_size = direction == ScrollDirection::Horizontal ? absolute_padding_box_rect().width() : absolute_padding_box_rect().height();
    if (is_viewport() || overflow == CSS::Overflow::Auto)
        return scrollable_overflow_size > scrollport_size;
    return overflow == CSS::Overflow::Scroll;
}

static constexpr CSSPixels scrollbar_thumb_thickness = 8;

Optional<CSSPixelRect> PaintableBox::scroll_thumb_rect(ScrollDirection direction) const
{
    if (!is_scrollable(direction))
        return {};

    auto padding_rect = absolute_padding_box_rect();
    auto scrollable_overflow_rect = this->scrollable_overflow_rect().value();
    auto scroll_overflow_size = direction == ScrollDirection::Horizontal ? scrollable_overflow_rect.width() : scrollable_overflow_rect.height();
    auto scrollport_size = direction == ScrollDirection::Horizontal ? padding_rect.width() : padding_rect.height();
    auto scroll_offset = direction == ScrollDirection::Horizontal ? this->scroll_offset().x() : this->scroll_offset().y();
    if (scroll_overflow_size == 0)
        return {};

    auto thumb_size = scrollport_size * (scrollport_size / scroll_overflow_size);
    CSSPixels thumb_position = 0;
    if (scroll_overflow_size > scrollport_size)
        thumb_position = scroll_offset * (scrollport_size - thumb_size) / (scroll_overflow_size - scrollport_size);

    CSSPixelRect thumb_rect;
    if (direction == ScrollDirection::Horizontal) {
        thumb_rect = {
            padding_rect.left() + thumb_position,
            padding_rect.bottom() - scrollbar_thumb_thickness,
            thumb_size,
            scrollbar_thumb_thickness
        };
    } else {
        thumb_rect = {
            padding_rect.right() - scrollbar_thumb_thickness,
            padding_rect.top() + thumb_position,
            scrollbar_thumb_thickness,
            thumb_size
        };
    }

    if (is_viewport())
        thumb_rect.translate_by(this->scroll_offset());

    return thumb_rect;
}

void PaintableBox::paint(PaintContext& context, PaintPhase phase) const
{
    if (!is_visible())
        return;

    if (phase == PaintPhase::Background) {
        paint_backdrop_filter(context);
        paint_background(context);
        paint_box_shadow(context);
    }

    if (phase == PaintPhase::Border) {
        paint_border(context);
    }

    if (phase == PaintPhase::Outline) {
        auto const& outline_data = this->outline_data();
        if (outline_data.has_value()) {
            auto outline_offset = this->outline_offset();
            auto border_radius_data = normalized_border_radii_data(ShrinkRadiiForBorders::No);
            auto borders_rect = absolute_border_box_rect();

            auto outline_offset_x = outline_offset;
            auto outline_offset_y = outline_offset;
            // "Both the height and the width of the outside of the shape drawn by the outline should not
            // become smaller than twice the computed value of the outline-width property to make sure
            // that an outline can be rendered even with large negative values."
            // https://www.w3.org/TR/css-ui-4/#outline-offset
            // So, if the horizontal outline offset is > half the borders_rect's width then we set it to that.
            // (And the same for y)
            if ((borders_rect.width() / 2) + outline_offset_x < 0)
                outline_offset_x = -borders_rect.width() / 2;
            if ((borders_rect.height() / 2) + outline_offset_y < 0)
                outline_offset_y = -borders_rect.height() / 2;

            border_radius_data.inflate(outline_data->top.width + outline_offset_y, outline_data->right.width + outline_offset_x, outline_data->bottom.width + outline_offset_y, outline_data->left.width + outline_offset_x);
            borders_rect.inflate(outline_data->top.width + outline_offset_y, outline_data->right.width + outline_offset_x, outline_data->bottom.width + outline_offset_y, outline_data->left.width + outline_offset_x);

            paint_all_borders(context.display_list_recorder(), context.rounded_device_rect(borders_rect), border_radius_data.as_corners(context), outline_data->to_device_pixels(context));
        }
    }

    auto scrollbar_width = computed_values().scrollbar_width();
    if (phase == PaintPhase::Overlay && scrollbar_width != CSS::ScrollbarWidth::None) {
        auto color = Color(Color::NamedColor::DarkGray).with_alpha(128);
        int thumb_corner_radius = static_cast<int>(context.rounded_device_pixels(scrollbar_thumb_thickness / 2));
        if (auto thumb_rect = scroll_thumb_rect(ScrollDirection::Horizontal); thumb_rect.has_value()) {
            auto thumb_device_rect = context.enclosing_device_rect(thumb_rect.value());
            context.display_list_recorder().fill_rect_with_rounded_corners(thumb_device_rect.to_type<int>(), color, thumb_corner_radius, thumb_corner_radius, thumb_corner_radius, thumb_corner_radius);
        }
        if (auto thumb_rect = scroll_thumb_rect(ScrollDirection::Vertical); thumb_rect.has_value()) {
            auto thumb_device_rect = context.enclosing_device_rect(thumb_rect.value());
            context.display_list_recorder().fill_rect_with_rounded_corners(thumb_device_rect.to_type<int>(), color, thumb_corner_radius, thumb_corner_radius, thumb_corner_radius, thumb_corner_radius);
        }
    }

    if (phase == PaintPhase::Overlay && layout_box().document().inspected_layout_node() == &layout_box()) {
        auto content_rect = absolute_rect();

        auto margin_box = box_model().margin_box();
        CSSPixelRect margin_rect;
        margin_rect.set_x(absolute_x() - margin_box.left);
        margin_rect.set_width(content_width() + margin_box.left + margin_box.right);
        margin_rect.set_y(absolute_y() - margin_box.top);
        margin_rect.set_height(content_height() + margin_box.top + margin_box.bottom);

        auto border_rect = absolute_border_box_rect();
        auto padding_rect = absolute_padding_box_rect();

        auto paint_inspector_rect = [&](CSSPixelRect const& rect, Color color) {
            auto device_rect = context.enclosing_device_rect(rect).to_type<int>();
            context.display_list_recorder().fill_rect(device_rect, Color(color).with_alpha(100));
            context.display_list_recorder().draw_rect(device_rect, Color(color));
        };

        paint_inspector_rect(margin_rect, Color::Yellow);
        paint_inspector_rect(padding_rect, Color::Cyan);
        paint_inspector_rect(border_rect, Color::Green);
        paint_inspector_rect(content_rect, Color::Magenta);

        auto& font = Platform::FontPlugin::the().default_font();

        StringBuilder builder;
        if (layout_box().dom_node())
            builder.append(layout_box().dom_node()->debug_description());
        else
            builder.append(layout_box().debug_description());
        builder.appendff(" {}x{} @ {},{}", border_rect.width(), border_rect.height(), border_rect.x(), border_rect.y());
        auto size_text = MUST(builder.to_string());
        auto size_text_rect = border_rect;
        size_text_rect.set_y(border_rect.y() + border_rect.height());
        size_text_rect.set_top(size_text_rect.top());
        size_text_rect.set_width(CSSPixels::nearest_value_for(font.width(size_text)) + 4);
        size_text_rect.set_height(CSSPixels::nearest_value_for(font.pixel_size()) + 4);
        auto size_text_device_rect = context.enclosing_device_rect(size_text_rect).to_type<int>();
        context.display_list_recorder().fill_rect(size_text_device_rect, context.palette().color(Gfx::ColorRole::Tooltip));
        context.display_list_recorder().draw_rect(size_text_device_rect, context.palette().threed_shadow1());
        context.display_list_recorder().draw_text(size_text_device_rect, size_text, font, Gfx::TextAlignment::Center, context.palette().color(Gfx::ColorRole::TooltipText));
    }
}

BordersData PaintableBox::remove_element_kind_from_borders_data(PaintableBox::BordersDataWithElementKind borders_data)
{
    return {
        .top = borders_data.top.border_data,
        .right = borders_data.right.border_data,
        .bottom = borders_data.bottom.border_data,
        .left = borders_data.left.border_data,
    };
}

void PaintableBox::paint_border(PaintContext& context) const
{
    auto borders_data = m_override_borders_data.has_value() ? remove_element_kind_from_borders_data(m_override_borders_data.value()) : BordersData {
        .top = box_model().border.top == 0 ? CSS::BorderData() : computed_values().border_top(),
        .right = box_model().border.right == 0 ? CSS::BorderData() : computed_values().border_right(),
        .bottom = box_model().border.bottom == 0 ? CSS::BorderData() : computed_values().border_bottom(),
        .left = box_model().border.left == 0 ? CSS::BorderData() : computed_values().border_left(),
    };
    paint_all_borders(context.display_list_recorder(), context.rounded_device_rect(absolute_border_box_rect()), normalized_border_radii_data().as_corners(context), borders_data.to_device_pixels(context));
}

void PaintableBox::paint_backdrop_filter(PaintContext& context) const
{
    auto& backdrop_filter = computed_values().backdrop_filter();
    if (!backdrop_filter.is_none())
        apply_backdrop_filter(context, absolute_border_box_rect(), normalized_border_radii_data(), backdrop_filter);
}

void PaintableBox::paint_background(PaintContext& context) const
{
    // If the body's background properties were propagated to the root element, do no re-paint the body's background.
    if (layout_box().is_body() && document().html_element()->should_use_body_background_properties())
        return;

    CSSPixelRect background_rect;
    Color background_color = computed_values().background_color();
    auto* background_layers = &computed_values().background_layers();

    if (layout_box().is_root_element()) {
        // CSS 2.1 Appendix E.2: If the element is a root element, paint the background over the entire canvas.
        background_rect = context.css_viewport_rect();

        // Section 2.11.2: If the computed value of background-image on the root element is none and its background-color is transparent,
        // user agents must instead propagate the computed values of the background properties from that element’s first HTML BODY child element.
        if (document().html_element()->should_use_body_background_properties()) {
            background_layers = document().background_layers();
            background_color = document().background_color();
        }
    } else {
        background_rect = absolute_padding_box_rect();
    }

    // HACK: If the Box has a border, use the bordered_rect to paint the background.
    //       This way if we have a border-radius there will be no gap between the filling and actual border.
    if (computed_values().border_top().width != 0 || computed_values().border_right().width != 0 || computed_values().border_bottom().width != 0 || computed_values().border_left().width != 0)
        background_rect = absolute_border_box_rect();

    Painting::paint_background(context, layout_box(), background_rect, background_color, computed_values().image_rendering(), background_layers, normalized_border_radii_data());
}

void PaintableBox::paint_box_shadow(PaintContext& context) const
{
    auto const& resolved_box_shadow_data = box_shadow_data();
    if (resolved_box_shadow_data.is_empty())
        return;
    auto borders_data = BordersData {
        .top = computed_values().border_top(),
        .right = computed_values().border_right(),
        .bottom = computed_values().border_bottom(),
        .left = computed_values().border_left(),
    };
    Painting::paint_box_shadow(context, absolute_border_box_rect(), absolute_padding_box_rect(),
        borders_data, normalized_border_radii_data(), resolved_box_shadow_data);
}

BorderRadiiData PaintableBox::normalized_border_radii_data(ShrinkRadiiForBorders shrink) const
{
    auto border_radii_data = this->border_radii_data();
    if (shrink == ShrinkRadiiForBorders::Yes)
        border_radii_data.shrink(computed_values().border_top().width, computed_values().border_right().width, computed_values().border_bottom().width, computed_values().border_left().width);
    return border_radii_data;
}

void PaintableBox::apply_scroll_offset(PaintContext& context, PaintPhase) const
{
    if (scroll_frame_id().has_value()) {
        context.display_list_recorder().save();
        context.display_list_recorder().set_scroll_frame_id(scroll_frame_id().value());
    }
}

void PaintableBox::reset_scroll_offset(PaintContext& context, PaintPhase) const
{
    if (scroll_frame_id().has_value())
        context.display_list_recorder().restore();
}

void PaintableBox::apply_clip_overflow_rect(PaintContext& context, PaintPhase phase) const
{
    if (!AK::first_is_one_of(phase, PaintPhase::Background, PaintPhase::Border, PaintPhase::Foreground, PaintPhase::Outline))
        return;

    if (clip_rect().has_value()) {
        auto overflow_clip_rect = clip_rect().value();
        m_clipping_overflow = true;
        context.display_list_recorder().save();
        context.display_list_recorder().add_clip_rect(context.enclosing_device_rect(overflow_clip_rect).to_type<int>());
        auto const& border_radii_clips = this->border_radii_clips();
        m_corner_clipper_ids.resize(border_radii_clips.size());
        auto const& combined_transform = combined_css_transform();
        for (size_t corner_clip_index = 0; corner_clip_index < border_radii_clips.size(); ++corner_clip_index) {
            auto const& corner_clip = border_radii_clips[corner_clip_index];
            auto corners = corner_clip.radii.as_corners(context);
            if (!corners.has_any_radius())
                continue;
            auto corner_clipper_id = context.allocate_corner_clipper_id();
            m_corner_clipper_ids[corner_clip_index] = corner_clipper_id;
            auto rect = corner_clip.rect.translated(-combined_transform.translation().to_type<CSSPixels>());
            context.display_list_recorder().sample_under_corners(corner_clipper_id, corner_clip.radii.as_corners(context), context.rounded_device_rect(rect).to_type<int>(), CornerClip::Outside);
        }
    }
}

void PaintableBox::clear_clip_overflow_rect(PaintContext& context, PaintPhase phase) const
{
    if (!AK::first_is_one_of(phase, PaintPhase::Background, PaintPhase::Border, PaintPhase::Foreground, PaintPhase::Outline))
        return;

    if (m_clipping_overflow) {
        m_clipping_overflow = false;
        auto const& border_radii_clips = this->border_radii_clips();
        for (int corner_clip_index = border_radii_clips.size() - 1; corner_clip_index >= 0; --corner_clip_index) {
            auto const& corner_clip = border_radii_clips[corner_clip_index];
            auto corners = corner_clip.radii.as_corners(context);
            if (!corners.has_any_radius())
                continue;
            auto corner_clipper_id = m_corner_clipper_ids[corner_clip_index];
            m_corner_clipper_ids[corner_clip_index] = corner_clipper_id;
            context.display_list_recorder().blit_corner_clipping(corner_clipper_id);
        }
        context.display_list_recorder().restore();
    }
}

void paint_cursor_if_needed(PaintContext& context, TextPaintable const& paintable, PaintableFragment const& fragment)
{
    auto const& navigable = *paintable.navigable();

    if (!navigable.is_focused())
        return;

    if (!navigable.cursor_blink_state())
        return;

    if (navigable.cursor_position()->node() != paintable.dom_node())
        return;

    // NOTE: This checks if the cursor is before the start or after the end of the fragment. If it is at the end, after all text, it should still be painted.
    if (navigable.cursor_position()->offset() < (unsigned)fragment.start() || navigable.cursor_position()->offset() > (unsigned)(fragment.start() + fragment.length()))
        return;

    if (!fragment.layout_node().dom_node() || !fragment.layout_node().dom_node()->is_editable())
        return;

    auto fragment_rect = fragment.absolute_rect();

    auto text = fragment.string_view();
    CSSPixelRect cursor_rect {
        fragment_rect.x() + CSSPixels::nearest_value_for(paintable.layout_node().first_available_font().width(text.substring_view(0, navigable.cursor_position()->offset() - fragment.start()))),
        fragment_rect.top(),
        1,
        fragment_rect.height()
    };

    auto cursor_device_rect = context.rounded_device_rect(cursor_rect).to_type<int>();

    context.display_list_recorder().draw_rect(cursor_device_rect, paintable.computed_values().color());
}

void paint_text_decoration(PaintContext& context, TextPaintable const& paintable, PaintableFragment const& fragment)
{
    auto& painter = context.display_list_recorder();
    auto& font = fragment.layout_node().first_available_font();
    auto fragment_box = fragment.absolute_rect();
    CSSPixels glyph_height = CSSPixels::nearest_value_for(font.pixel_size());
    auto baseline = fragment.baseline();

    auto line_color = paintable.computed_values().text_decoration_color();
    auto const& text_paintable = static_cast<TextPaintable const&>(fragment.paintable());
    auto device_line_thickness = context.rounded_device_pixels(text_paintable.text_decoration_thickness());

    auto text_decoration_lines = paintable.computed_values().text_decoration_line();
    for (auto line : text_decoration_lines) {
        DevicePixelPoint line_start_point {};
        DevicePixelPoint line_end_point {};

        switch (line) {
        case CSS::TextDecorationLine::None:
            return;
        case CSS::TextDecorationLine::Underline:
            line_start_point = context.rounded_device_point(fragment_box.top_left().translated(0, baseline + 2));
            line_end_point = context.rounded_device_point(fragment_box.top_right().translated(-1, baseline + 2));
            break;
        case CSS::TextDecorationLine::Overline:
            line_start_point = context.rounded_device_point(fragment_box.top_left().translated(0, baseline - glyph_height));
            line_end_point = context.rounded_device_point(fragment_box.top_right().translated(-1, baseline - glyph_height));
            break;
        case CSS::TextDecorationLine::LineThrough: {
            auto x_height = font.x_height();
            line_start_point = context.rounded_device_point(fragment_box.top_left().translated(0, baseline - x_height * CSSPixels(0.5f)));
            line_end_point = context.rounded_device_point(fragment_box.top_right().translated(-1, baseline - x_height * CSSPixels(0.5f)));
            break;
        }
        case CSS::TextDecorationLine::Blink:
            // Conforming user agents may simply not blink the text
            return;
        }

        switch (paintable.computed_values().text_decoration_style()) {
        case CSS::TextDecorationStyle::Solid:
            painter.draw_line(line_start_point.to_type<int>(), line_end_point.to_type<int>(), line_color, device_line_thickness.value(), Gfx::LineStyle::Solid);
            break;
        case CSS::TextDecorationStyle::Double:
            switch (line) {
            case CSS::TextDecorationLine::Underline:
                break;
            case CSS::TextDecorationLine::Overline:
                line_start_point.translate_by(0, -device_line_thickness - context.rounded_device_pixels(1));
                line_end_point.translate_by(0, -device_line_thickness - context.rounded_device_pixels(1));
                break;
            case CSS::TextDecorationLine::LineThrough:
                line_start_point.translate_by(0, -device_line_thickness / 2);
                line_end_point.translate_by(0, -device_line_thickness / 2);
                break;
            default:
                VERIFY_NOT_REACHED();
            }

            painter.draw_line(line_start_point.to_type<int>(), line_end_point.to_type<int>(), line_color, device_line_thickness.value());
            painter.draw_line(line_start_point.translated(0, device_line_thickness + 1).to_type<int>(), line_end_point.translated(0, device_line_thickness + 1).to_type<int>(), line_color, device_line_thickness.value());
            break;
        case CSS::TextDecorationStyle::Dashed:
            painter.draw_line(line_start_point.to_type<int>(), line_end_point.to_type<int>(), line_color, device_line_thickness.value(), Gfx::LineStyle::Dashed);
            break;
        case CSS::TextDecorationStyle::Dotted:
            painter.draw_line(line_start_point.to_type<int>(), line_end_point.to_type<int>(), line_color, device_line_thickness.value(), Gfx::LineStyle::Dotted);
            break;
        case CSS::TextDecorationStyle::Wavy:
            painter.draw_triangle_wave(line_start_point.to_type<int>(), line_end_point.to_type<int>(), line_color, device_line_thickness.value() + 1, device_line_thickness.value());
            break;
        }
    }
}

void paint_text_fragment(PaintContext& context, TextPaintable const& paintable, PaintableFragment const& fragment, PaintPhase phase)
{
    auto& painter = context.display_list_recorder();

    if (phase == PaintPhase::Foreground) {
        auto fragment_absolute_rect = fragment.absolute_rect();
        auto fragment_absolute_device_rect = context.enclosing_device_rect(fragment_absolute_rect);

        if (paintable.document().inspected_layout_node() == &paintable.layout_node())
            context.display_list_recorder().draw_rect(fragment_absolute_device_rect.to_type<int>(), Color::Magenta);

        auto text = paintable.text_for_rendering();

        auto glyph_run = fragment.glyph_run();
        if (!glyph_run)
            return;

        DevicePixelPoint baseline_start { fragment_absolute_device_rect.x(), fragment_absolute_device_rect.y() + context.rounded_device_pixels(fragment.baseline()) };
        auto scale = context.device_pixels_per_css_pixel();
        painter.draw_text_run(baseline_start.to_type<int>(), *glyph_run, paintable.computed_values().webkit_text_fill_color(), fragment_absolute_device_rect.to_type<int>(), scale);

        auto selection_rect = context.enclosing_device_rect(fragment.selection_rect(paintable.layout_node().first_available_font())).to_type<int>();
        if (!selection_rect.is_empty()) {
            painter.fill_rect(selection_rect, CSS::SystemColor::highlight());
            DisplayListRecorderStateSaver saver(painter);
            painter.add_clip_rect(selection_rect);
            painter.draw_text_run(baseline_start.to_type<int>(), *glyph_run, CSS::SystemColor::highlight_text(), fragment_absolute_device_rect.to_type<int>(), scale);
        }

        paint_text_decoration(context, paintable, fragment);
        paint_cursor_if_needed(context, paintable, fragment);
    }
}

void PaintableWithLines::paint(PaintContext& context, PaintPhase phase) const
{
    if (!is_visible())
        return;

    PaintableBox::paint(context, phase);

    if (fragments().is_empty())
        return;

    bool should_clip_overflow = computed_values().overflow_x() != CSS::Overflow::Visible && computed_values().overflow_y() != CSS::Overflow::Visible;
    Optional<u32> corner_clip_id;

    auto clip_box = absolute_padding_box_rect();
    if (get_clip_rect().has_value()) {
        clip_box.intersect(get_clip_rect().value());
        should_clip_overflow = true;
    }
    if (should_clip_overflow) {
        context.display_list_recorder().save();
        // FIXME: Handle overflow-x and overflow-y being different values.
        auto clip_box_with_enclosing_scroll_frame_offset = clip_box;
        if (enclosing_scroll_frame_offset().has_value())
            clip_box_with_enclosing_scroll_frame_offset.translate_by(enclosing_scroll_frame_offset().value());
        context.display_list_recorder().add_clip_rect(context.rounded_device_rect(clip_box_with_enclosing_scroll_frame_offset).to_type<int>());

        auto border_radii = normalized_border_radii_data(ShrinkRadiiForBorders::Yes);
        CornerRadii corner_radii {
            .top_left = border_radii.top_left.as_corner(context),
            .top_right = border_radii.top_right.as_corner(context),
            .bottom_right = border_radii.bottom_right.as_corner(context),
            .bottom_left = border_radii.bottom_left.as_corner(context)
        };
        if (corner_radii.has_any_radius()) {
            corner_clip_id = context.allocate_corner_clipper_id();
            context.display_list_recorder().sample_under_corners(*corner_clip_id, corner_radii, context.rounded_device_rect(clip_box).to_type<int>(), CornerClip::Outside);
        }

        context.display_list_recorder().save();
        auto scroll_offset = context.rounded_device_point(this->scroll_offset());
        context.display_list_recorder().translate(-scroll_offset.to_type<int>());
    }

    // Text shadows
    // This is yet another loop, but done here because all shadows should appear under all text.
    // So, we paint the shadows before painting any text.
    // FIXME: Find a smarter way to do this?
    if (phase == PaintPhase::Foreground) {
        for (auto& fragment : fragments()) {
            paint_text_shadow(context, fragment, fragment.shadows());
        }
    }

    for (auto const& fragment : m_fragments) {
        auto fragment_absolute_rect = fragment.absolute_rect();
        auto fragment_absolute_device_rect = context.enclosing_device_rect(fragment_absolute_rect);
        if (context.should_show_line_box_borders()) {
            context.display_list_recorder().draw_rect(fragment_absolute_device_rect.to_type<int>(), Color::Green);
            context.display_list_recorder().draw_line(
                context.rounded_device_point(fragment_absolute_rect.top_left().translated(0, fragment.baseline())).to_type<int>(),
                context.rounded_device_point(fragment_absolute_rect.top_right().translated(-1, fragment.baseline())).to_type<int>(), Color::Red);
        }
        if (is<TextPaintable>(fragment.paintable()))
            paint_text_fragment(context, static_cast<TextPaintable const&>(fragment.paintable()), fragment, phase);
    }

    if (should_clip_overflow) {
        context.display_list_recorder().restore();
        if (corner_clip_id.has_value()) {
            context.display_list_recorder().blit_corner_clipping(*corner_clip_id);
            corner_clip_id = {};
        }
        context.display_list_recorder().restore();
    }
}

Paintable::DispatchEventOfSameName PaintableBox::handle_mousedown(Badge<EventHandler>, CSSPixelPoint position, unsigned, unsigned)
{
    auto vertical_scroll_thumb_rect = scroll_thumb_rect(ScrollDirection::Vertical);
    auto horizontal_scroll_thumb_rect = scroll_thumb_rect(ScrollDirection::Horizontal);
    if (vertical_scroll_thumb_rect.has_value() && vertical_scroll_thumb_rect.value().contains(position)) {
        if (is_viewport())
            position.translate_by(-scroll_offset());
        m_last_mouse_tracking_position = position;
        m_scroll_thumb_dragging_direction = ScrollDirection::Vertical;
        const_cast<HTML::Navigable&>(*navigable()).event_handler().set_mouse_event_tracking_paintable(this);
    } else if (horizontal_scroll_thumb_rect.has_value() && horizontal_scroll_thumb_rect.value().contains(position)) {
        if (is_viewport())
            position.translate_by(-scroll_offset());
        m_last_mouse_tracking_position = position;
        m_scroll_thumb_dragging_direction = ScrollDirection::Horizontal;
        const_cast<HTML::Navigable&>(*navigable()).event_handler().set_mouse_event_tracking_paintable(this);
    }
    return Paintable::DispatchEventOfSameName::Yes;
}

Paintable::DispatchEventOfSameName PaintableBox::handle_mouseup(Badge<EventHandler>, CSSPixelPoint, unsigned, unsigned)
{
    if (m_last_mouse_tracking_position.has_value()) {
        m_last_mouse_tracking_position.clear();
        m_scroll_thumb_dragging_direction.clear();
        const_cast<HTML::Navigable&>(*navigable()).event_handler().set_mouse_event_tracking_paintable(nullptr);
    }
    return Paintable::DispatchEventOfSameName::Yes;
}

Paintable::DispatchEventOfSameName PaintableBox::handle_mousemove(Badge<EventHandler>, CSSPixelPoint position, unsigned, unsigned)
{
    if (m_last_mouse_tracking_position.has_value()) {
        if (is_viewport())
            position.translate_by(-scroll_offset());

        Gfx::Point<double> scroll_delta;
        if (m_scroll_thumb_dragging_direction == ScrollDirection::Horizontal)
            scroll_delta.set_x((position.x() - m_last_mouse_tracking_position->x()).to_double());
        else
            scroll_delta.set_y((position.y() - m_last_mouse_tracking_position->y()).to_double());

        auto padding_rect = absolute_padding_box_rect();
        auto scrollable_overflow_rect = this->scrollable_overflow_rect().value();
        auto scroll_overflow_size = m_scroll_thumb_dragging_direction == ScrollDirection::Horizontal ? scrollable_overflow_rect.width() : scrollable_overflow_rect.height();
        auto scrollport_size = m_scroll_thumb_dragging_direction == ScrollDirection::Horizontal ? padding_rect.width() : padding_rect.height();
        auto scroll_px_per_mouse_position_delta_px = scroll_overflow_size.to_double() / scrollport_size.to_double();
        scroll_delta *= scroll_px_per_mouse_position_delta_px;

        if (is_viewport()) {
            document().window()->scroll_by(scroll_delta.x(), scroll_delta.y());
        } else {
            scroll_by(scroll_delta.x(), scroll_delta.y());
        }

        m_last_mouse_tracking_position = position;
        return Paintable::DispatchEventOfSameName::No;
    }
    return Paintable::DispatchEventOfSameName::Yes;
}

bool PaintableBox::handle_mousewheel(Badge<EventHandler>, CSSPixelPoint, unsigned, unsigned, int wheel_delta_x, int wheel_delta_y)
{
    if (!layout_box().is_user_scrollable())
        return false;

    // TODO: Vertical and horizontal scroll overflow should be handled seperately.
    if (!has_scrollable_overflow())
        return false;

    scroll_by(wheel_delta_x, wheel_delta_y);
    return true;
}

Layout::BlockContainer const& PaintableWithLines::layout_box() const
{
    return static_cast<Layout::BlockContainer const&>(PaintableBox::layout_box());
}

Layout::BlockContainer& PaintableWithLines::layout_box()
{
    return static_cast<Layout::BlockContainer&>(PaintableBox::layout_box());
}

TraversalDecision PaintableBox::hit_test_scrollbars(CSSPixelPoint position, Function<TraversalDecision(HitTestResult)> const& callback) const
{
    auto vertical_scroll_thumb_rect = scroll_thumb_rect(ScrollDirection::Vertical);
    if (vertical_scroll_thumb_rect.has_value() && vertical_scroll_thumb_rect.value().contains(position))
        return callback(HitTestResult { const_cast<PaintableBox&>(*this) });
    auto horizontal_scroll_thumb_rect = scroll_thumb_rect(ScrollDirection::Horizontal);
    if (horizontal_scroll_thumb_rect.has_value() && horizontal_scroll_thumb_rect.value().contains(position))
        return callback(HitTestResult { const_cast<PaintableBox&>(*this) });
    return TraversalDecision::Continue;
}

TraversalDecision PaintableBox::hit_test(CSSPixelPoint position, HitTestType type, Function<TraversalDecision(HitTestResult)> const& callback) const
{
    if (clip_rect().has_value() && !clip_rect()->contains(position))
        return TraversalDecision::Continue;

    auto position_adjusted_by_scroll_offset = position;
    if (enclosing_scroll_frame_offset().has_value())
        position_adjusted_by_scroll_offset.translate_by(-enclosing_scroll_frame_offset().value());

    if (!is_visible())
        return TraversalDecision::Continue;

    if (hit_test_scrollbars(position_adjusted_by_scroll_offset, callback) == TraversalDecision::Break)
        return TraversalDecision::Break;

    if (layout_box().is_viewport()) {
        auto& viewport_paintable = const_cast<ViewportPaintable&>(static_cast<ViewportPaintable const&>(*this));
        viewport_paintable.build_stacking_context_tree_if_needed();
        viewport_paintable.document().update_paint_and_hit_testing_properties_if_needed();
        viewport_paintable.refresh_scroll_state();
        viewport_paintable.refresh_clip_state();
        return stacking_context()->hit_test(position, type, callback);
    }

    for (auto const* child = last_child(); child; child = child->previous_sibling()) {
        auto z_index = child->computed_values().z_index();
        if (child->layout_node().is_positioned() && z_index.value_or(0) == 0)
            continue;
        if (child->hit_test(position, type, callback) == TraversalDecision::Break)
            return TraversalDecision::Break;
    }

    if (!absolute_border_box_rect().contains(position_adjusted_by_scroll_offset.x(), position_adjusted_by_scroll_offset.y()))
        return TraversalDecision::Continue;

    if (!visible_for_hit_testing())
        return TraversalDecision::Continue;

    return callback(HitTestResult { const_cast<PaintableBox&>(*this) });
}

Optional<HitTestResult> PaintableBox::hit_test(CSSPixelPoint position, HitTestType type) const
{
    Optional<HitTestResult> result;
    (void)PaintableBox::hit_test(position, type, [&](HitTestResult candidate) {
        VERIFY(!result.has_value());
        if (!candidate.paintable->visible_for_hit_testing())
            return TraversalDecision::Continue;
        result = move(candidate);
        return TraversalDecision::Break;
    });
    return result;
}

TraversalDecision PaintableWithLines::hit_test(CSSPixelPoint position, HitTestType type, Function<TraversalDecision(HitTestResult)> const& callback) const
{
    if (clip_rect().has_value() && !clip_rect()->contains(position))
        return TraversalDecision::Continue;

    auto position_adjusted_by_scroll_offset = position;
    if (enclosing_scroll_frame_offset().has_value())
        position_adjusted_by_scroll_offset.translate_by(-enclosing_scroll_frame_offset().value());

    if (!layout_box().children_are_inline() || m_fragments.is_empty()) {
        return PaintableBox::hit_test(position, type, callback);
    }

    if (hit_test_scrollbars(position_adjusted_by_scroll_offset, callback) == TraversalDecision::Break)
        return TraversalDecision::Break;

    for (auto const* child = last_child(); child; child = child->previous_sibling()) {
        if (child->hit_test(position, type, callback) == TraversalDecision::Break)
            return TraversalDecision::Break;
    }

    Optional<HitTestResult> last_good_candidate;
    for (auto const& fragment : fragments()) {
        if (fragment.paintable().stacking_context())
            continue;
        auto fragment_absolute_rect = fragment.absolute_rect();
        if (fragment_absolute_rect.contains(position_adjusted_by_scroll_offset)) {
            if (fragment.paintable().hit_test(position, type, callback) == TraversalDecision::Break)
                return TraversalDecision::Break;
            HitTestResult hit_test_result { const_cast<Paintable&>(fragment.paintable()), fragment.text_index_at(position_adjusted_by_scroll_offset.x()) };
            if (callback(hit_test_result) == TraversalDecision::Break)
                return TraversalDecision::Break;
        }

        // If we reached this point, the position is not within the fragment. However, the fragment start or end might be the place to place the cursor.
        // This determines whether the fragment is a good candidate for the position. The last such good fragment is chosen.
        // The best candidate is either the end of the line above, the beginning of the line below, or the beginning or end of the current line.
        // We arbitrarily choose to consider the end of the line above and ignore the beginning of the line below.
        // If we knew the direction of selection, we could make a better choice.
        if (fragment_absolute_rect.bottom() - 1 <= position_adjusted_by_scroll_offset.y()) { // fully below the fragment
            last_good_candidate = HitTestResult { const_cast<Paintable&>(fragment.paintable()), fragment.start() + fragment.length() };
        } else if (fragment_absolute_rect.top() <= position_adjusted_by_scroll_offset.y()) { // vertically within the fragment
            if (position_adjusted_by_scroll_offset.x() < fragment_absolute_rect.left()) {    // left of the fragment
                if (!last_good_candidate.has_value()) {                                      // first fragment of the line
                    last_good_candidate = HitTestResult { const_cast<Paintable&>(fragment.paintable()), fragment.start() };
                }
            } else { // right of the fragment
                last_good_candidate = HitTestResult { const_cast<Paintable&>(fragment.paintable()), fragment.start() + fragment.length() };
            }
        }
    }

    if (type == HitTestType::TextCursor && last_good_candidate.has_value()) {
        if (callback(last_good_candidate.value()) == TraversalDecision::Break)
            return TraversalDecision::Break;
    }
    if (!stacking_context() && is_visible() && absolute_border_box_rect().contains(position_adjusted_by_scroll_offset.x(), position_adjusted_by_scroll_offset.y())) {
        if (callback(HitTestResult { const_cast<PaintableWithLines&>(*this) }) == TraversalDecision::Break)
            return TraversalDecision::Break;
    }

    return TraversalDecision::Continue;
}

void PaintableBox::set_needs_display() const
{
    if (auto navigable = this->navigable())
        navigable->set_needs_display(absolute_rect());
}

Optional<CSSPixelRect> PaintableBox::get_masking_area() const
{
    // FIXME: Support clip-paths with transforms.
    if (!combined_css_transform().is_identity_or_translation())
        return {};
    auto clip_path = computed_values().clip_path();
    // FIXME: Support other clip sources.
    if (!clip_path.has_value() || !clip_path->is_basic_shape())
        return {};
    // FIXME: Support other geometry boxes. See: https://drafts.fxtf.org/css-masking/#typedef-geometry-box
    return absolute_border_box_rect();
}

Optional<Gfx::Bitmap::MaskKind> PaintableBox::get_mask_type() const
{
    // Always an alpha mask as only basic shapes are supported right now.
    return Gfx::Bitmap::MaskKind::Alpha;
}

RefPtr<Gfx::Bitmap> PaintableBox::calculate_mask(PaintContext& context, CSSPixelRect const& masking_area) const
{
    VERIFY(computed_values().clip_path()->is_basic_shape());
    auto const& basic_shape = computed_values().clip_path()->basic_shape();
    auto path = basic_shape.to_path(masking_area, layout_node());
    auto device_pixel_scale = context.device_pixels_per_css_pixel();
    path = path.copy_transformed(Gfx::AffineTransform {}.set_scale(device_pixel_scale, device_pixel_scale));
    auto mask_rect = context.enclosing_device_rect(masking_area);
    auto maybe_bitmap = Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, mask_rect.size().to_type<int>());
    if (maybe_bitmap.is_error())
        return {};
    auto bitmap = maybe_bitmap.release_value();
    Gfx::Painter painter(*bitmap);
    Gfx::AntiAliasingPainter aa_painter(painter);
    aa_painter.fill_path(path, Color::Black);
    return bitmap;
}

}
