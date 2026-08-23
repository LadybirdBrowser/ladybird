/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::computed_value_types::{ComputedFilter, ComputedStyleValueHandle, MaskValues};
use crate::css::computed_value_views::ComputedValuesView;
use crate::css::css_enums::{
    backface_visibility, content_visibility, isolation, line_style, mix_blend_mode, outline_style, overflow,
    positioning, transform_style,
};
use crate::css::css_pixels::CssPixels;
use crate::css::retained_fly_string::RetainedUtf16FlyString;
use crate::css::serialize::{StringUnits, with_fly_string_units};
use crate::css::style_value::StyleValueData;
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::{NodeFlag, NodeKind, NodeSlotId};

const SEPARATOR_COMMA: u8 = 1;

fn has_flag(arena: &LayoutNodeArena, node: NodeSlotId, flag: NodeFlag) -> bool {
    arena.node_flags_if_live(node) & flag as u32 != 0
}

pub(crate) fn handle_value(handle: &ComputedStyleValueHandle) -> Option<&StyleValueData> {
    // SAFETY: A non-null handle points at the retained style value owned by the
    // node's style group payload, which outlives every reader of the pass.
    unsafe { handle.pointer.cast::<StyleValueData>().as_ref() }
}

fn for_each_comma_item(value: &StyleValueData, mut f: impl FnMut(&StyleValueData) -> bool) -> bool {
    if let StyleValueData::ValueList { values, separator, .. } = value
        && *separator == SEPARATOR_COMMA
    {
        return values.as_slice().iter().any(|item| f(item.data()));
    }
    f(value)
}

pub(crate) fn comma_items(value: &StyleValueData) -> Vec<&StyleValueData> {
    if let StyleValueData::ValueList { values, separator, .. } = value
        && *separator == SEPARATOR_COMMA
    {
        return values.as_slice().iter().map(|item| item.data()).collect();
    }
    vec![value]
}

pub(crate) fn is_abstract_image(value: &StyleValueData) -> bool {
    matches!(
        value,
        StyleValueData::Image { .. }
            | StyleValueData::ImageSet { .. }
            | StyleValueData::LinearGradient { .. }
            | StyleValueData::ConicGradient { .. }
            | StyleValueData::RadialGradient { .. }
    )
}

pub(crate) fn background_layers_have_image(style: ComputedValuesView<'_>) -> bool {
    let Some(value) = handle_value(&style.background().background_image) else {
        return false;
    };
    for_each_comma_item(value, is_abstract_image)
}

fn fly_string_equals_ascii(string: &RetainedUtf16FlyString, expected: &[u8]) -> bool {
    with_fly_string_units(string, |units| match units {
        StringUnits::Ascii(bytes) => bytes == expected,
        StringUnits::Utf16(_) => false,
    })
}

fn will_change_has_property(style: ComputedValuesView<'_>, name: &[u8]) -> bool {
    let Some(value) = handle_value(&style.misc_reset().will_change) else {
        return false;
    };
    let StyleValueData::ValueList { values, .. } = value else {
        return false;
    };
    values.as_slice().iter().any(|item| match item.data() {
        StyleValueData::CustomIdent { custom_ident } => fly_string_equals_ascii(custom_ident, name),
        _ => false,
    })
}

fn filter_has_filters(filter: &ComputedFilter) -> bool {
    filter.operations.length != 0
}

fn mask_references_url(mask: &MaskValues) -> bool {
    let Some(value) = handle_value(&mask.mask_image) else {
        return false;
    };
    let mut first = None;
    for_each_comma_item(value, |item| {
        first.get_or_insert(matches!(item, StyleValueData::Url { .. }));
        true
    });
    first.unwrap_or(false)
}

fn mask_image_is_image(mask: &MaskValues) -> bool {
    let Some(value) = handle_value(&mask.mask_image) else {
        return false;
    };
    let mut first = None;
    for_each_comma_item(value, |item| {
        first.get_or_insert(is_abstract_image(item));
        true
    });
    first.unwrap_or(false)
}

pub(crate) fn mask_layers_have_image(mask: &MaskValues) -> bool {
    let Some(value) = handle_value(&mask.mask_image) else {
        return false;
    };
    for_each_comma_item(value, is_abstract_image)
}

fn clip_path_has_value(mask: &MaskValues) -> bool {
    handle_value(&mask.clip_path)
        .is_some_and(|value| matches!(value, StyleValueData::Url { .. } | StyleValueData::BasicShape { .. }))
}

fn view_transition_name_has_value(style: ComputedValuesView<'_>) -> bool {
    handle_value(&style.misc_reset().view_transition_name)
        .is_some_and(|value| matches!(value, StyleValueData::CustomIdent { .. }))
}

fn containment_applies_to_display(arena: &LayoutNodeArena, node: NodeSlotId, style: ComputedValuesView<'_>) -> bool {
    let display = style.display();
    if display.is_internal_table() && !display.is_table_cell() {
        return false;
    }
    let is_replaced_box = arena
        .node_kind_if_live(node)
        .is_some_and(crate::layout::kind_is_replaced_box);
    if display.is_inline_outside() && display.is_flow_inside() && !is_replaced_box {
        return false;
    }
    true
}

pub(crate) fn has_layout_containment(arena: &LayoutNodeArena, node: NodeSlotId, style: ComputedValuesView<'_>) -> bool {
    let contained = style.box_values().layout_containment || style.content_visibility() == content_visibility::AUTO;
    contained && containment_applies_to_display(arena, node, style)
}

pub(crate) fn is_scroll_container(arena: &LayoutNodeArena, node: NodeSlotId) -> bool {
    if arena.node_kind_if_live(node) == Some(NodeKind::Viewport) {
        return true;
    }
    let Some(style) = arena.node_style_if_live(node) else {
        return false;
    };
    let overflow_value_makes_box_a_scroll_container =
        |overflow_keyword: u8| matches!(overflow_keyword, overflow::AUTO | overflow::HIDDEN | overflow::SCROLL);
    overflow_value_makes_box_a_scroll_container(style.overflow_x())
        || overflow_value_makes_box_a_scroll_container(style.overflow_y())
}

pub(crate) fn has_paint_containment(arena: &LayoutNodeArena, node: NodeSlotId, style: ComputedValuesView<'_>) -> bool {
    let contained = style.box_values().paint_containment || style.content_visibility() == content_visibility::AUTO;
    contained && containment_applies_to_display(arena, node, style)
}

fn kind_is_svg_box(kind: NodeKind) -> bool {
    matches!(
        kind,
        NodeKind::SVGBox
            | NodeKind::SVGClipBox
            | NodeKind::SVGGeometryBox
            | NodeKind::SVGGraphicsBox
            | NodeKind::SVGImageBox
            | NodeKind::SVGMaskBox
            | NodeKind::SVGPatternBox
            | NodeKind::SVGTextBox
            | NodeKind::SVGTextPathBox
    )
}

fn kind_is_svg_graphics_box(kind: NodeKind) -> bool {
    matches!(
        kind,
        NodeKind::SVGGeometryBox
            | NodeKind::SVGGraphicsBox
            | NodeKind::SVGImageBox
            | NodeKind::SVGMaskBox
            | NodeKind::SVGTextBox
            | NodeKind::SVGTextPathBox
    )
}

pub(crate) fn kind_is_svg_element_box(kind: NodeKind) -> bool {
    kind_is_svg_box(kind) || matches!(kind, NodeKind::SVGSVGBox | NodeKind::SVGForeignObjectBox)
}

pub(crate) fn node_is_root_element(arena: &LayoutNodeArena, node: NodeSlotId) -> bool {
    let flags = arena.node_flags_if_live(node);
    flags & NodeFlag::Anonymous as u32 == 0 && flags & NodeFlag::IsHtmlHtmlElement as u32 != 0
}

fn style_establishes_fixed_positioning_containing_block(
    arena: &LayoutNodeArena,
    node: NodeSlotId,
    style: ComputedValuesView<'_>,
) -> bool {
    use crate::css::css_enums::{backface_visibility, content_visibility};
    let will_change = |name: &[u8]| will_change_has_property(style, name);
    let transformable = is_transformable(arena, node);
    let transform = style.transform();

    if (!transform.transformations.pointer.is_null() || will_change(b"transform")) && transformable {
        return true;
    }
    if (!transform.translate.pointer.is_null() || will_change(b"translate")) && transformable {
        return true;
    }
    if (!transform.rotate.pointer.is_null() || will_change(b"rotate")) && transformable {
        return true;
    }
    if (!transform.scale.pointer.is_null() || will_change(b"scale")) && transformable {
        return true;
    }

    if (transform.has_perspective || will_change(b"perspective")) && transformable {
        return true;
    }

    let effects = style.effects();
    let is_root_element = node_is_root_element(arena, node);
    if (filter_has_filters(&effects.filter) || will_change(b"filter")) && !is_root_element {
        return true;
    }

    if (filter_has_filters(&effects.backdrop_filter) || will_change(b"backdrop-filter")) && !is_root_element {
        return true;
    }

    if will_change(b"contain") {
        return true;
    }
    let content_visibility_adds_containment = style.content_visibility() == content_visibility::AUTO;
    let box_values = style.box_values();
    if (box_values.layout_containment || content_visibility_adds_containment)
        && has_layout_containment(arena, node, style)
    {
        return true;
    }
    if (box_values.paint_containment || content_visibility_adds_containment)
        && has_paint_containment(arena, node, style)
    {
        return true;
    }

    if (transform.transform_style == transform_style::PRESERVE_3D || will_change(b"transform-style")) && transformable {
        return true;
    }

    if (transform.backface_visibility == backface_visibility::HIDDEN || will_change(b"backface-visibility"))
        && transformable
        && participates_in_a_3d_rendering_context(arena, node)
    {
        return true;
    }

    false
}

pub(crate) fn establishes_positioning_containing_blocks(arena: &LayoutNodeArena, node: NodeSlotId) -> (bool, bool) {
    let Some(kind) = arena.node_kind_if_live(node) else {
        return (false, false);
    };
    if !crate::layout::kind_is_box(kind) {
        return (false, false);
    }

    if kind == NodeKind::SVGForeignObjectBox {
        return (true, true);
    }

    let Some(style) = arena.node_style_if_live(node) else {
        return (false, false);
    };
    if style_establishes_fixed_positioning_containing_block(arena, node, style) {
        return (true, true);
    }

    if style.box_values().position != positioning::STATIC || will_change_has_property(style, b"position") {
        return (true, false);
    }

    if kind == NodeKind::Viewport {
        return (true, false);
    }

    (false, false)
}

pub(crate) fn has_css_transform(arena: &LayoutNodeArena, node: NodeSlotId, style: ComputedValuesView<'_>) -> bool {
    let transform = style.transform();
    let has_transform = !transform.transformations.pointer.is_null()
        || !transform.rotate.pointer.is_null()
        || !transform.translate.pointer.is_null()
        || !transform.scale.pointer.is_null();
    has_transform && is_transformable(arena, node)
}

pub(crate) fn is_transformable(arena: &LayoutNodeArena, node: NodeSlotId) -> bool {
    let Some(kind) = arena.node_kind_if_live(node) else {
        return false;
    };
    if kind_is_svg_element_box(kind) {
        if matches!(kind, NodeKind::SVGClipBox | NodeKind::SVGPatternBox) {
            return true;
        }
        let is_renderable = (kind_is_svg_graphics_box(kind) && kind != NodeKind::SVGMaskBox)
            || matches!(kind, NodeKind::SVGSVGBox | NodeKind::SVGForeignObjectBox);
        if !is_renderable {
            return false;
        }
        let mut ancestor = arena.node_parent_if_live(node);
        while let Some(current) = ancestor {
            if matches!(
                arena.node_kind_if_live(current),
                Some(NodeKind::SVGTextBox | NodeKind::SVGTextPathBox)
            ) {
                return false;
            }
            ancestor = arena.node_parent_if_live(current);
        }
        return true;
    }

    let is_dom_element =
        !has_flag(arena, node, NodeFlag::Anonymous) && !crate::layout::kind_is_text(kind) && kind != NodeKind::Viewport;
    let is_element_or_pseudo_element = is_dom_element || arena.node_is_generated_for_pseudo_element(node);
    if is_element_or_pseudo_element && crate::layout::kind_is_box(kind) {
        let Some(style) = arena.node_style_if_live(node) else {
            return false;
        };
        let display = style.display();
        if display.is_table_column() || display.is_table_column_group() {
            return false;
        }
        let is_atomic_inline = has_flag(arena, node, NodeFlag::IsReplacedElement)
            || kind == NodeKind::ListItemMarkerBox
            || (display.is_inline_outside() && !display.is_flow_inside());
        if display.is_inline_outside() && !is_atomic_inline {
            return false;
        }
        return true;
    }

    false
}

fn used_transform_style_is_preserve_3d(arena: &LayoutNodeArena, node: NodeSlotId) -> bool {
    let Some(style) = arena.node_style_if_live(node) else {
        return false;
    };
    if style.transform().transform_style != transform_style::PRESERVE_3D {
        return false;
    }
    let effects = style.effects();
    let box_values = style.box_values();
    let mask_values = style.mask();
    let grouping_overflow = |value: u8| value != overflow::VISIBLE && value != overflow::CLIP;
    let has_transform_style_grouping_property = grouping_overflow(box_values.overflow_x)
        || grouping_overflow(box_values.overflow_y)
        || effects.opacity < 1.0
        || filter_has_filters(&effects.filter)
        || effects.clip_is_rect
        || clip_path_has_value(mask_values)
        || effects.isolation == isolation::ISOLATE
        || mask_references_url(mask_values)
        || mask_layers_have_image(mask_values)
        || effects.mix_blend_mode != mix_blend_mode::NORMAL
        || filter_has_filters(&effects.backdrop_filter);
    if has_transform_style_grouping_property {
        return false;
    }
    if has_paint_containment(arena, node, style) {
        return false;
    }
    true
}

pub(crate) fn establishes_or_extends_a_3d_rendering_context(arena: &LayoutNodeArena, node: NodeSlotId) -> bool {
    if !has_flag(arena, node, NodeFlag::HasPreserve3dTransformStyle) {
        return false;
    }
    used_transform_style_is_preserve_3d(arena, node) && is_transformable(arena, node)
}

fn participates_in_a_3d_rendering_context(arena: &LayoutNodeArena, node: NodeSlotId) -> bool {
    let mut ancestor = arena.node_parent_if_live(node);
    while let Some(current) = ancestor {
        if has_flag(arena, current, NodeFlag::Anonymous) {
            ancestor = arena.node_parent_if_live(current);
            continue;
        }
        return establishes_or_extends_a_3d_rendering_context(arena, current);
    }
    false
}

#[derive(Clone, Copy)]
pub(crate) struct OutlineData {
    pub color: u32,
    pub line_style: u8,
    pub width: CssPixels,
}

fn outline_style_to_line_style(style: u8) -> u8 {
    match style {
        outline_style::AUTO => line_style::SOLID,
        outline_style::NONE => line_style::NONE,
        outline_style::DOTTED => line_style::DOTTED,
        outline_style::DASHED => line_style::DASHED,
        outline_style::SOLID => line_style::SOLID,
        outline_style::DOUBLE => line_style::DOUBLE,
        outline_style::GROOVE => line_style::GROOVE,
        outline_style::RIDGE => line_style::RIDGE,
        outline_style::INSET => line_style::INSET,
        outline_style::OUTSET => line_style::OUTSET,
        _ => unreachable!("computed outline-style holds an unknown value"),
    }
}

// Returns OptionalNone if there is no outline to paint.
pub(crate) fn outline_data(
    arena: &LayoutNodeArena,
    node: NodeSlotId,
    window_is_focused: bool,
    auto_outline_color: u32,
) -> Option<OutlineData> {
    let style = arena.node_style_if_live(node)?;
    let misc = style.misc_reset();
    if misc.outline_style == outline_style::AUTO && !window_is_focused {
        return None;
    }
    let (color, resolved_line_style, width) = if misc.outline_style == outline_style::AUTO {
        // `auto` lets us do whatever we want for the outline. 2px of the accent
        // colour seems reasonable.
        (auto_outline_color, line_style::SOLID, CssPixels::from_integer(2))
    } else {
        let resolved = outline_style_to_line_style(misc.outline_style);
        (misc.outline_color, resolved, misc.outline_width)
    };
    if color >> 24 == 0 || resolved_line_style == line_style::NONE || width.raw_value() == 0 {
        return None;
    }
    Some(OutlineData {
        color,
        line_style: resolved_line_style,
        width,
    })
}

pub(crate) fn outline_offset(arena: &LayoutNodeArena, node: NodeSlotId) -> CssPixels {
    arena
        .node_style_if_live(node)
        .map_or(CssPixels::from_raw(0), |style| style.misc_reset().outline_offset)
}

pub(crate) fn is_text_decoration_propagation_boundary(arena: &LayoutNodeArena, node: NodeSlotId) -> bool {
    let Some(kind) = arena.node_kind_if_live(node) else {
        return false;
    };
    // NB: Anonymous wrappers must stay transparent to propagation so an element's own decorations still reach
    //     its text. The principal box of a pseudo-element is not a wrapper and must be checked like any other
    //     element, and a table wrapper carries the float and position of the table it wraps, so it is the only
    //     box where an out-of-flow table is observable.
    if has_flag(arena, node, NodeFlag::Anonymous)
        && !arena.node_is_generated_for_pseudo_element(node)
        && kind != NodeKind::TableWrapper
    {
        return false;
    }

    // https://drafts.csswg.org/css-text-decor-4/#decorating-box
    // NOTE: Note that text decorations are not propagated to any out-of-flow descendants, nor to the contents
    //       of atomic inline-level descendants such as inline blocks and inline tables.
    if arena.node_is_out_of_flow_if_live(node) {
        return true;
    }
    if has_flag(arena, node, NodeFlag::IsReplacedElement) || kind == NodeKind::ListItemMarkerBox {
        return true;
    }
    arena.node_style_if_live(node).is_some_and(|style| {
        let display = style.display();
        display.is_inline_outside() && !display.is_flow_inside()
    })
}

pub(crate) fn z_index(arena: &LayoutNodeArena, node: NodeSlotId) -> Option<i32> {
    let style = arena.node_style_if_live(node)?;
    let box_values = style.box_values();
    box_values.has_z_index.then_some(box_values.z_index)
}

pub(crate) fn effective_z_index(arena: &LayoutNodeArena, paintable: NodeSlotId) -> Option<i32> {
    if !is_positioned(arena, paintable) {
        return None;
    }
    z_index(arena, paintable)
}

// Facts a paintable shares with its layout node, read live from the arena so
// style changes that do not relayout (z-index, for one) are never stale here.

pub(crate) fn is_positioned(arena: &LayoutNodeArena, node: NodeSlotId) -> bool {
    arena
        .node_data_if_live(node)
        .is_some_and(|data| crate::layout::node_is_positioned(data, arena.node_style_if_live(node)))
}

pub(crate) fn position(arena: &LayoutNodeArena, node: NodeSlotId) -> u8 {
    crate::layout::node_position(arena.node_style_if_live(node))
}

pub(crate) fn is_fixed_position(arena: &LayoutNodeArena, node: NodeSlotId) -> bool {
    position(arena, node) == positioning::FIXED
}

pub(crate) fn is_sticky_position(arena: &LayoutNodeArena, node: NodeSlotId) -> bool {
    position(arena, node) == positioning::STICKY
}

pub(crate) fn is_floating(arena: &LayoutNodeArena, node: NodeSlotId) -> bool {
    arena
        .node_data_if_live(node)
        .is_some_and(|data| crate::layout::node_is_floating(data, arena.node_style_if_live(node)))
}

pub(crate) fn is_inline(arena: &LayoutNodeArena, node: NodeSlotId) -> bool {
    crate::layout::node_is_inline_outside(arena.node_style_if_live(node))
}

pub(crate) fn display(arena: &LayoutNodeArena, node: NodeSlotId) -> crate::css::display::FfiDisplay {
    crate::layout::node_display(arena.node_style_if_live(node))
}

pub(crate) fn is_flex_or_grid_item(arena: &LayoutNodeArena, node: NodeSlotId) -> bool {
    has_flag(arena, node, NodeFlag::IsFlexItem) || has_flag(arena, node, NodeFlag::IsGridItem)
}

pub(crate) fn is_anonymous(arena: &LayoutNodeArena, node: NodeSlotId) -> bool {
    has_flag(arena, node, NodeFlag::Anonymous)
}

pub(crate) fn is_replaced_element(arena: &LayoutNodeArena, node: NodeSlotId) -> bool {
    has_flag(arena, node, NodeFlag::IsReplacedElement)
}

pub(crate) fn is_replaced_box(arena: &LayoutNodeArena, node: NodeSlotId) -> bool {
    arena
        .node_kind_if_live(node)
        .is_some_and(crate::layout::kind_is_replaced_box)
}

pub(crate) fn establishes_stacking_context(arena: &LayoutNodeArena, node: NodeSlotId) -> bool {
    let Some(kind) = arena.node_kind_if_live(node) else {
        return false;
    };

    if kind_is_svg_box(kind) {
        return false;
    }

    if kind == NodeKind::Viewport {
        return true;
    }

    let flags = arena.node_flags_if_live(node);
    if flags & NodeFlag::Anonymous as u32 == 0 && flags & NodeFlag::IsHtmlHtmlElement as u32 != 0 {
        return true;
    }

    let Some(style) = arena.node_style_if_live(node) else {
        return false;
    };
    let box_values = style.box_values();
    let will_change = |name: &[u8]| will_change_has_property(style, name);

    let position = box_values.position;
    let has_z_index = box_values.has_z_index || will_change(b"z-index");

    if (position == positioning::ABSOLUTE || position == positioning::RELATIVE) && has_z_index {
        return true;
    }

    if position == positioning::FIXED || position == positioning::STICKY || will_change(b"position") {
        return true;
    }

    let transformable = is_transformable(arena, node);
    let transform = style.transform();
    if transformable {
        if !transform.transformations.pointer.is_null() || will_change(b"transform") {
            return true;
        }
        if !transform.translate.pointer.is_null() || will_change(b"translate") {
            return true;
        }
        if !transform.rotate.pointer.is_null() || will_change(b"rotate") {
            return true;
        }
        if !transform.scale.pointer.is_null() || will_change(b"scale") {
            return true;
        }
    }

    if has_z_index
        && let Some(parent) = arena.node_parent_if_live(node)
        && let Some(parent_style) = arena.node_style_if_live(parent)
    {
        let parent_display = parent_style.display();
        if parent_display.is_flex_inside() || parent_display.is_grid_inside() {
            return true;
        }
    }

    let effects = style.effects();
    if filter_has_filters(&effects.backdrop_filter)
        || filter_has_filters(&effects.filter)
        || will_change(b"backdrop-filter")
        || will_change(b"filter")
    {
        return true;
    }

    let mask_values = style.mask();
    if mask_references_url(mask_values)
        || clip_path_has_value(mask_values)
        || mask_image_is_image(mask_values)
        || will_change(b"mask")
        || will_change(b"clip-path")
        || will_change(b"mask-image")
    {
        return true;
    }

    if kind == NodeKind::SVGForeignObjectBox {
        return true;
    }

    if effects.isolation == isolation::ISOLATE || will_change(b"isolation") {
        return true;
    }

    if has_layout_containment(arena, node, style)
        || has_paint_containment(arena, node, style)
        || will_change(b"contain")
    {
        return true;
    }

    if effects.mix_blend_mode != mix_blend_mode::NORMAL || will_change(b"mix-blend-mode") {
        return true;
    }

    if view_transition_name_has_value(style) || will_change(b"view-transition-name") {
        return true;
    }

    if transformable && (transform.has_perspective || will_change(b"perspective")) {
        return true;
    }

    if transformable && (transform.transform_style == transform_style::PRESERVE_3D || will_change(b"transform-style")) {
        return true;
    }

    if (transform.backface_visibility == backface_visibility::HIDDEN || will_change(b"backface-visibility"))
        && transformable
        && participates_in_a_3d_rendering_context(arena, node)
    {
        return true;
    }

    effects.opacity < 1.0 || will_change(b"opacity")
}
