/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use crate::css::style::{NaturalSize, ReplacedContentInput};

pub(crate) fn node_may_have_replaced_content_facts(data: &NodeData) -> bool {
    kind_is_replaced_box(data.kind.get())
        || matches!(
            data.kind.get(),
            NodeKind::RangeInputBox | NodeKind::SVGImageBox | NodeKind::TextAreaBox | NodeKind::TextInputBox
        )
        || has_flag(data, NodeFlag::IsHtmlInputElement)
}

// Size containment gives any box an auto content box size of zero, so
// size-contained boxes join the replaced kinds in publishing real facts.
// C++ enrollment and the Rust sync assertion both evaluate this predicate.
pub(crate) fn node_may_have_replaced_content_facts_including_size_containment(data: &NodeData) -> bool {
    if node_may_have_replaced_content_facts(data) {
        return true;
    }
    if !kind_is_box(data.kind.get()) {
        return false;
    }
    let Some(style) = node_style_view(data) else {
        return false;
    };
    style.has_size_containment() || style.is_size_container()
}

// https://drafts.csswg.org/css-contain-2/#containment-size
fn style_has_size_containment(style: ComputedValuesView<'_>) -> bool {
    // Giving an element size containment has no effect if its inner display type is 'table', or if its principal box
    // is an internal table box.
    let display = style.display();
    if display.is_table_inside() || display.is_internal_table() {
        return false;
    }
    style.has_size_containment() || style.is_size_container()
}

/// The replaced content facts of an enrolled node, from its kind, its computed style and what its
/// element published as the input of its replaced content.
pub(crate) fn derived_replaced_content_facts(data: &NodeData, input: ReplacedContentInput) -> FfiReplacedContentFacts {
    if data.kind.get() == NodeKind::SVGImageBox {
        return svg_image_facts(input);
    }
    node_style_view(data).map_or_else(Default::default, |style| {
        replaced_content_facts(data.kind.get(), ReplacedContentStyle::of(style), input)
    })
}

/// What a replaced box's facts read of its computed style.
#[derive(Clone, Copy)]
struct ReplacedContentStyle {
    /// The explicit intrinsic inner size that stands for the natural size of a size-contained box,
    /// zero in an axis that has none.
    size_contained: Option<(CssPixels, CssPixels)>,
    /// The `ch` unit.
    zero_advance: CssPixels,
    line_height: CssPixels,
    is_horizontal: bool,
    appearance_is_none: bool,
}

impl ReplacedContentStyle {
    fn of(style: ComputedValuesView<'_>) -> Self {
        // https://drafts.csswg.org/css-contain-2/#containment-size
        // Replaced elements must be treated as having a natural width and height of 0 and no natural aspect ratio.
        // https://drafts.csswg.org/css-sizing-4/#intrinsic-size-override
        // If an element has an explicit intrinsic inner size in an axis, [...] the size of the contents in that axis
        // are instead treated as being the explicit intrinsic inner size.
        let explicit_size = |has_length: bool, length_px: f64| {
            if has_length {
                CssPixels::nearest_value_for(length_px)
            } else {
                CssPixels::default()
            }
        };
        Self {
            size_contained: style_has_size_containment(style).then(|| {
                (
                    explicit_size(
                        style.contain_intrinsic_width_has_length(),
                        style.contain_intrinsic_width_px(),
                    ),
                    explicit_size(
                        style.contain_intrinsic_height_has_length(),
                        style.contain_intrinsic_height_px(),
                    ),
                )
            }),
            zero_advance: CssPixels::nearest_value_for_f32(style.font_zero_advance()),
            line_height: style.line_height(),
            is_horizontal: style.writing_mode() == crate::css::css_enums::writing_mode::HORIZONTAL_TB,
            appearance_is_none: style.appearance() == crate::css::css_enums::appearance::NONE,
        }
    }

    /// `count` characters, the `ch` unit.
    fn characters(self, count: u32) -> CssPixels {
        CssPixels::nearest_value_for(f64::from(count) * self.zero_advance.to_double())
    }

    /// An inline size and a block size as a width and a height.
    fn in_writing_mode(self, inline_size: CssPixels, block_size: CssPixels) -> (CssPixels, CssPixels) {
        if self.is_horizontal {
            (inline_size, block_size)
        } else {
            (block_size, inline_size)
        }
    }

    // https://html.spec.whatwg.org/multipage/rendering.html#the-input-element-as-a-text-entry-widget
    fn text_control_default_preferred_size(self, size: u32) -> (CssPixels, CssPixels) {
        // [...] If the element has a size attribute, and parsing that attribute's value using the rules for parsing
        // non-negative integers doesn't generate an error, return the value obtained from applying the converting a
        // character width to pixels algorithm to the value of the attribute. Otherwise, return the value obtained from
        // applying the converting a character width to pixels algorithm to the number 20.
        // FIXME: Implement the specified "converting a character width to pixels" algorithm.
        // FIXME: HTML does not yet detail the primitive appearance of text inputs. Use one line for the default
        //        preferred block size, matching the native appearance described by HTML and the behavior of other
        //        engines.
        self.in_writing_mode(self.characters(size), self.line_height)
    }
}

// An SVG <image> runs the default sizing algorithm over its own geometry, so it takes the natural size exactly as its
// image reports it, absent rather than zero while nothing has decoded, together with the default object size that
// applies once something has. Size containment does not apply to it.
fn svg_image_facts(input: ReplacedContentInput) -> FfiReplacedContentFacts {
    let (ReplacedContentInput::NaturalSize(natural_size) | ReplacedContentInput::DecodedSvgImage(natural_size)) = input
    else {
        panic!("an SVG image publishes its natural size as it arrives");
    };
    let mut facts = FfiReplacedContentFacts::default();
    set_auto_content_size(&mut facts, AutoContentSize::natural(natural_size));
    // The SVG formatting context reads the default object size as it is. It is no preferred size the
    // sizing of an ordinary replaced box would take, so the flags that offer it as one stay clear.
    if matches!(input, ReplacedContentInput::DecodedSvgImage(_)) {
        facts.default_preferred_width = CssPixels::from_integer(300);
        facts.default_preferred_height = CssPixels::from_integer(150);
    }
    facts
}

fn set_auto_content_size(facts: &mut FfiReplacedContentFacts, auto_content_size: AutoContentSize) {
    if let Some(width) = auto_content_size.width {
        facts.has_auto_content_width = true;
        facts.auto_content_width = width;
    }
    if let Some(height) = auto_content_size.height {
        facts.has_auto_content_height = true;
        facts.auto_content_height = height;
    }
    if let Some((numerator, denominator)) = auto_content_size.aspect_ratio {
        facts.auto_content_aspect_ratio_numerator = numerator;
        facts.auto_content_aspect_ratio_denominator = denominator;
    }
}

fn replaced_content_facts(
    kind: NodeKind,
    style: ReplacedContentStyle,
    input: ReplacedContentInput,
) -> FfiReplacedContentFacts {
    let mut facts = FfiReplacedContentFacts::default();
    set_auto_content_size(&mut facts, auto_content_size(kind, style, input));
    if style.appearance_is_none
        && let ReplacedContentInput::Input {
            size,
            is_text_entry: true,
        } = input
    {
        let (width, height) = style.text_control_default_preferred_size(size);
        facts.has_default_preferred_width = true;
        facts.default_preferred_width = width;
        facts.has_default_preferred_height = true;
        facts.default_preferred_height = height;
    }
    facts
}

/// A replaced box's natural size and aspect ratio, any of which it can lack.
#[derive(Default)]
struct AutoContentSize {
    width: Option<CssPixels>,
    height: Option<CssPixels>,
    aspect_ratio: Option<(CssPixels, CssPixels)>,
}

impl AutoContentSize {
    fn of_size((width, height): (CssPixels, CssPixels)) -> Self {
        Self {
            width: Some(width),
            height: Some(height),
            aspect_ratio: None,
        }
    }

    fn natural(natural_size: NaturalSize) -> Self {
        Self {
            width: natural_size.width.map(CssPixels::from_raw),
            height: natural_size.height.map(CssPixels::from_raw),
            aspect_ratio: natural_size
                .aspect_ratio
                .map(|(numerator, denominator)| (CssPixels::from_raw(numerator), CssPixels::from_raw(denominator))),
        }
    }
}

fn auto_content_size(kind: NodeKind, style: ReplacedContentStyle, input: ReplacedContentInput) -> AutoContentSize {
    if let Some(explicit_size) = style.size_contained {
        return AutoContentSize::of_size(explicit_size);
    }
    match kind {
        NodeKind::CheckBox => AutoContentSize::of_size((CssPixels::from_integer(13), CssPixels::from_integer(13))),
        NodeKind::RadioButton => AutoContentSize::of_size((CssPixels::from_integer(12), CssPixels::from_integer(12))),
        // AD-HOC: A slider has no in-flow content to size itself from, so provide a default content-box size for when
        //         its `width` or `height` is `auto`: 20ch by 16px.
        NodeKind::RangeInputBox => AutoContentSize::of_size((style.characters(20), CssPixels::from_integer(16))),
        NodeKind::TextAreaBox => {
            let ReplacedContentInput::TextArea { cols, rows } = input else {
                panic!("a textarea publishes its cols and rows as it arrives");
            };
            let block_size = CssPixels::nearest_value_for(f64::from(rows) * style.line_height.to_double());
            AutoContentSize::of_size(style.in_writing_mode(style.characters(cols), block_size))
        }
        NodeKind::CanvasBox => {
            let ReplacedContentInput::Canvas { width, height } = input else {
                panic!("a canvas publishes its width and height as it arrives");
            };
            let width = CssPixels::from_integer(i64::from(width));
            let height = CssPixels::from_integer(i64::from(height));
            AutoContentSize {
                width: Some(width),
                height: Some(height),
                aspect_ratio: (width != CssPixels::default() && height != CssPixels::default())
                    .then_some((width, height)),
            }
        }
        NodeKind::TextInputBox => {
            let ReplacedContentInput::Input { size, .. } = input else {
                panic!("an input publishes its size as it arrives");
            };
            AutoContentSize::of_size(style.text_control_default_preferred_size(size))
        }
        NodeKind::VideoBox => {
            let ReplacedContentInput::NaturalSize(natural_size) = input else {
                panic!("a video publishes its natural size as it arrives");
            };
            AutoContentSize::natural(natural_size)
        }
        NodeKind::ImageBox => {
            let ReplacedContentInput::NaturalSize(natural_size) = input else {
                panic!("an image box's element or its provider publishes its image's natural size");
            };
            AutoContentSize::natural(natural_size)
        }
        // An <object> showing an SVG document is sized from the document's root. Any other navigable container has
        // no natural size.
        NodeKind::NavigableContainerViewport => match input {
            ReplacedContentInput::NaturalSize(natural_size) => AutoContentSize::natural(natural_size),
            _ => AutoContentSize::default(),
        },
        _ => AutoContentSize::default(),
    }
}

/// The node's own computed style, read off the style container the node data points at. Callers inside a layout pass go
/// through the pass callbacks instead; this is for the node-data entry points the C++ side calls directly.
pub(crate) fn node_style_view(data: &NodeData) -> Option<ComputedValuesView<'_>> {
    if data.style.get().is_null() {
        return None;
    }
    // SAFETY: A non-null style pointer addresses the container's group
    // pointer array, which FfiStylePayloads mirrors exactly.
    let payloads = unsafe { data.style.get().deref() };
    Some(ComputedValuesView::new(&payloads.groups))
}

pub(crate) fn style_insets_use_anchor_functions(style: ComputedValuesView<'_>) -> bool {
    let surround = style.surround();
    [
        &surround.top_anchor_inset,
        &surround.right_anchor_inset,
        &surround.bottom_anchor_inset,
        &surround.left_anchor_inset,
    ]
    .iter()
    .any(|handle| !handle.pointer.is_null())
        || [
            &surround.inset.top,
            &surround.inset.right,
            &surround.inset.bottom,
            &surround.inset.left,
        ]
        .iter()
        .any(|side| {
            side.length_percentage()
                .is_some_and(|value| value.contains_anchor_function())
        })
}

pub(crate) fn node_uses_anchor_positioning(data: &NodeData) -> bool {
    kind_is_box(data.kind.get())
        && node_style_view(data).is_some_and(|style| {
            style.is_absolutely_positioned()
                && (style_insets_use_anchor_functions(style) || !style.anchor().position_area.as_slice().is_empty())
        })
}

pub(crate) fn node_is_out_of_flow(data: &impl NodeShape, style: Option<ComputedValuesView<'_>>) -> bool {
    let Some(style) = style else {
        return false;
    };
    (style.is_floating() && !has_flag(data, NodeFlag::IsFlexItem)) || style.is_absolutely_positioned()
}

/// Painting treats flex and grid items with a z-index other than auto as if
/// they were positioned, in addition to boxes whose position is not static.
pub(crate) fn node_is_positioned(data: &impl NodeShape, style: Option<ComputedValuesView<'_>>) -> bool {
    let Some(style) = style else {
        return false;
    };
    let box_values = style.box_values();
    let is_flex_or_grid_item = has_flag(data, NodeFlag::IsFlexItem) || has_flag(data, NodeFlag::IsGridItem);
    box_values.position != crate::css::css_enums::positioning::STATIC
        || (is_flex_or_grid_item && box_values.has_z_index)
}

pub(crate) fn node_position(style: Option<ComputedValuesView<'_>>) -> u8 {
    style.map_or(crate::css::css_enums::positioning::STATIC, |style| {
        style.box_values().position
    })
}

/// Flex items never float, whatever their computed float value says.
pub(crate) fn node_is_floating(data: &impl NodeShape, style: Option<ComputedValuesView<'_>>) -> bool {
    style.is_some_and(|style| style.is_floating()) && !has_flag(data, NodeFlag::IsFlexItem)
}

pub(crate) fn node_is_inline_outside(style: Option<ComputedValuesView<'_>>) -> bool {
    style.is_some_and(|style| style.display().is_inline_outside())
}

pub(crate) fn node_display(style: Option<ComputedValuesView<'_>>) -> crate::css::display::FfiDisplay {
    style.map_or_else(crate::css::display::FfiDisplay::none, |style| style.display())
}

pub(crate) fn node_can_have_children(data: &impl NodeShape) -> bool {
    match data.kind() {
        NodeKind::BreakNode => false,
        NodeKind::AudioBox | NodeKind::VideoBox => has_flag(data, NodeFlag::ReplacedBoxCanHaveChildren),
        NodeKind::SVGSVGBox => true,
        kind if kind_is_replaced_box(kind) => false,
        _ => true,
    }
}

/// Whether a box's in-flow descendants name it as their containing block. This is the question the walks that find
/// in-flow containing blocks ask, so anything that walks past a box on behalf of an enclosing formatting context has
/// to ask it too: the boxes inside such a box are laid out against it, not against the block container of the
/// context the walk started in.
pub(crate) fn node_forms_containing_block_for_children(
    data: &impl NodeShape,
    style: Option<ComputedValuesView<'_>>,
) -> bool {
    if kind_is_block_container(data.kind()) && !node_is_fragmented_inline(data, style) {
        return true;
    }
    if let Some(style) = style {
        let display = style.display();
        if display.is_flex_inside() || display.is_grid_inside() {
            return true;
        }
    }
    kind_is_replaced_box(data.kind()) && node_can_have_children(data)
}

/// https://drafts.csswg.org/css-display/#atomic-inline
/// An inline-level box laid out as a single opaque box on the line rather than as a fragmented inline. Inline flow
/// boxes are otherwise walked into by inline layout, which would lay the boxes inside out against the containing block
/// of the enclosing inline formatting context: a box that is its own children's containing block has to be laid out as
/// one box instead. Elements whose box type does not follow from their display reach that case, since <legend> and
/// <fieldset> get a block container box whatever their computed display says.
pub(crate) fn node_is_atomic_inline(data: &impl NodeShape, style: Option<ComputedValuesView<'_>>) -> bool {
    has_flag(data, NodeFlag::IsReplacedElement)
        || data.kind() == NodeKind::ListItemMarkerBox
        || style.is_some_and(|style| {
            let display = style.display();
            display.is_inline_outside()
                && (!display.is_flow_inside() || node_forms_containing_block_for_children(data, Some(style)))
        })
}

pub(crate) fn node_is_fragmented_inline(data: &impl NodeShape, style: Option<ComputedValuesView<'_>>) -> bool {
    data.kind() == NodeKind::InlineNode
        || (data.kind() == NodeKind::ListItemBox
            && style.is_some_and(|style| {
                let display = style.display();
                display.is_inline_outside() && display.is_flow_inside()
            }))
}

pub(crate) fn node_has_auto_content_box_size(data: &NodeData) -> bool {
    (kind_is_replaced_box(data.kind.get()) && data.kind.get() != NodeKind::AudioBox)
        || matches!(
            data.kind.get(),
            NodeKind::RangeInputBox | NodeKind::TextAreaBox | NodeKind::TextInputBox
        )
}

// https://developer.mozilla.org/en-US/docs/Web/Guide/CSS/Block_formatting_context
// ComputedValuesView::own_style_establishes_block_formatting_context covers
// computed-style-only terms; this composite adds the terms that need the node
// kind, stamped DOM identity, the live IsFlexItem flag, or whether the parent
// is a flex or grid container.
pub(crate) fn node_creates_block_formatting_context(
    data: &NodeData,
    style: Option<ComputedValuesView<'_>>,
    parent_is_flex_or_grid_container: bool,
) -> bool {
    if kind_is_replaced_box(data.kind.get()) {
        return false;
    }
    if data.kind.get() == NodeKind::SVGForeignObjectBox {
        return true;
    }
    if let Some(style) = style {
        let display = style.display();
        if display.is_table_inside() || display.is_flex_inside() || display.is_grid_inside() {
            return false;
        }
        if (style.is_floating() && !has_flag(data, NodeFlag::IsFlexItem))
            || style.own_style_establishes_block_formatting_context()
        {
            return true;
        }
        // An inline flow box that is its own children's containing block is laid out as an atomic inline, and an atomic
        // inline establishes an independent formatting context: the boxes inside it belong to a context it establishes
        // rather than to the one its line is part of.
        if display.is_inline_outside()
            && display.is_flow_inside()
            && node_forms_containing_block_for_children(data, Some(style))
        {
            return true;
        }
    }
    if has_flag(data, NodeFlag::IsHtmlHtmlElement)
        || data.kind.get() == NodeKind::FieldSetBox
        // https://drafts.csswg.org/css-lists-3/#list-style-position-outside
        // "If the list item is a block container: the marker box is a block container"
        || data.kind.get() == NodeKind::ListItemMarkerBox
        || has_flag(data, NodeFlag::UsesButtonLayout)
    {
        return true;
    }
    parent_is_flex_or_grid_container
}

pub(crate) fn node_is_flex_or_grid_container(style: Option<ComputedValuesView<'_>>) -> bool {
    style.is_some_and(|style| style.display().is_flex_inside() || style.display().is_grid_inside())
}

pub(crate) fn node_applies_text_overflow_ellipsis(style: Option<ComputedValuesView<'_>>) -> bool {
    style.is_some_and(|style| {
        style.text_overflow() == text_overflow::ELLIPSIS && style.overflow_x() != overflow::VISIBLE
    })
}

pub(crate) fn has_ancestor_fact(data: &NodeData, fact: AncestorFact) -> bool {
    data.ancestor_facts.get() & fact as u8 != 0
}

/// The construction facts a test row is built with, as the word the style mirror publishes them in.
#[cfg(test)]
pub(crate) fn construction_fact_word(facts: &FfiNodeConstructionFacts) -> u32 {
    use crate::css::style::bridge::element_construction_fact as fact;
    [
        (fact::IS_HTML_INPUT_ELEMENT, facts.is_html_input_element),
        (fact::IS_HTML_HTML_ELEMENT, facts.is_html_html_element),
        (fact::IS_IN_USER_AGENT_SHADOW_TREE, facts.is_in_user_agent_shadow_tree),
        (fact::USES_BUTTON_LAYOUT, facts.uses_button_layout),
        (fact::IS_EDITING_HOST, facts.is_editing_host),
        (fact::IS_BODY, facts.is_body),
        (fact::IS_DOCUMENT_ELEMENT, facts.is_document_element),
    ]
    .into_iter()
    .filter(|&(_, is_set)| is_set)
    .fold(0, |word, (bit, _)| word | bit)
}

/// The flags a row of `kind` is built with, from its node's published construction facts.
pub(crate) fn construction_flags(kind: NodeKind, is_anonymous: bool, construction_facts: u32) -> u32 {
    use crate::css::style::bridge::element_construction_fact as fact;
    let has = |bit: u32| construction_facts & bit != 0;
    let has_style = kind != NodeKind::Node && !kind_is_text(kind);
    // Some native controls use a generic box so they can host their internal shadow tree, but
    // remain replaced elements for CSS box generation and inline layout.
    let is_replaced_element = kind_is_replaced_box(kind) || has(fact::IS_HTML_INPUT_ELEMENT);
    [
        (NodeFlag::Anonymous, is_anonymous),
        (NodeFlag::HasStyle, has_style),
        (NodeFlag::IsReplacedElement, is_replaced_element),
        (NodeFlag::IsHtmlInputElement, has(fact::IS_HTML_INPUT_ELEMENT)),
        (NodeFlag::IsHtmlHtmlElement, has(fact::IS_HTML_HTML_ELEMENT)),
        (NodeFlag::IsDocumentElement, has(fact::IS_DOCUMENT_ELEMENT)),
        (
            NodeFlag::IsInUserAgentShadowTree,
            has(fact::IS_IN_USER_AGENT_SHADOW_TREE),
        ),
        (NodeFlag::UsesButtonLayout, has(fact::USES_BUTTON_LAYOUT)),
        (NodeFlag::IsEditingHost, has(fact::IS_EDITING_HOST)),
        (NodeFlag::IsBody, has(fact::IS_BODY)),
    ]
    .into_iter()
    .filter(|(_, is_set)| *is_set)
    .fold(0, |flags, (flag, _)| flags | flag as u32)
}

pub(crate) fn containing_block_establishment_flag(is_fixed_position: bool) -> NodeFlag {
    if is_fixed_position {
        NodeFlag::EstablishesFixedPositionContainingBlock
    } else {
        NodeFlag::EstablishesAbsolutePositionContainingBlock
    }
}

/// The facts of a node the questions above answer from: the live [`NodeData`] or a published
/// [`super::node_data::PaintNode`].
pub(crate) trait NodeShape {
    fn kind(&self) -> NodeKind;
    fn flags(&self) -> u32;
}

impl NodeShape for NodeData {
    #[inline]
    fn kind(&self) -> NodeKind {
        self.kind.get()
    }

    #[inline]
    fn flags(&self) -> u32 {
        self.flags.get()
    }
}

impl NodeShape for (NodeKind, u32) {
    #[inline]
    fn kind(&self) -> NodeKind {
        self.0
    }

    #[inline]
    fn flags(&self) -> u32 {
        self.1
    }
}

impl NodeShape for super::node_data::PaintNode {
    #[inline]
    fn kind(&self) -> NodeKind {
        self.kind
    }

    #[inline]
    fn flags(&self) -> u32 {
        self.flags
    }
}

pub(crate) fn has_flag(data: &impl NodeShape, flag: NodeFlag) -> bool {
    data.flags() & flag as u32 != 0
}

pub(crate) fn kind_is_text(kind: NodeKind) -> bool {
    matches!(kind, NodeKind::GeneratedTextNode | NodeKind::TextNode)
}

pub(crate) fn kind_and_style_make_scroll_container(kind: NodeKind, style: Option<ComputedValuesView<'_>>) -> bool {
    if kind == NodeKind::Viewport {
        return true;
    }
    let Some(style) = style else {
        return false;
    };
    let overflow_value_makes_box_a_scroll_container =
        |overflow_keyword: u8| matches!(overflow_keyword, overflow::AUTO | overflow::HIDDEN | overflow::SCROLL);
    overflow_value_makes_box_a_scroll_container(style.overflow_x())
        || overflow_value_makes_box_a_scroll_container(style.overflow_y())
}

pub(crate) fn kind_is_box(kind: NodeKind) -> bool {
    !matches!(
        kind,
        NodeKind::Unset
            | NodeKind::BreakNode
            | NodeKind::InlineNode
            | NodeKind::Node
            | NodeKind::NodeWithStyle
            | NodeKind::GeneratedTextNode
            | NodeKind::TextNode
    )
}

pub(crate) fn kind_is_block_container(kind: NodeKind) -> bool {
    matches!(
        kind,
        NodeKind::BlockContainer
            | NodeKind::FieldSetBox
            | NodeKind::LegendBox
            | NodeKind::ListItemBox
            | NodeKind::ListItemMarkerBox
            | NodeKind::RangeInputBox
            | NodeKind::SVGForeignObjectBox
            | NodeKind::TableWrapper
            | NodeKind::TextAreaBox
            | NodeKind::TextInputBox
            | NodeKind::Viewport
    )
}

pub(crate) fn kind_is_replaced_box(kind: NodeKind) -> bool {
    matches!(
        kind,
        NodeKind::AudioBox
            | NodeKind::CanvasBox
            | NodeKind::CheckBox
            | NodeKind::ImageBox
            | NodeKind::NavigableContainerViewport
            | NodeKind::RadioButton
            | NodeKind::ReplacedBox
            | NodeKind::SVGSVGBox
            | NodeKind::VideoBox
    )
}

pub(crate) fn kind_is_svg_box(kind: NodeKind) -> bool {
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

/// A pass-scoped lens over one node's facts. Pure classification reads the
/// arena NodeData directly; style-derived answers read the per-slot style
/// snapshot; content-derived answers read the kind-gated lazy fact stores.
/// Nothing is materialized per node, so every answer is as live as its source.
#[derive(Clone, Copy)]
pub(crate) struct NodeFacts<'pass> {
    callbacks: LayoutPass<'pass>,
    node: Node,
    data: &'pass NodeData,
    style_payloads: Option<&'pass FfiStylePayloads>,
}

impl<'pass> NodeFacts<'pass> {
    #[inline]
    pub(crate) fn new(callbacks: &LayoutPass<'pass>, node: Node) -> Self {
        let data = callbacks.node_data(node);
        let style_payloads = callbacks.arena().style_payloads(node);
        Self {
            callbacks: *callbacks,
            node,
            data,
            style_payloads,
        }
    }

    /// The facts of `node`, from the node data and style payloads that [`Self::new`] looked up for it.
    #[inline]
    pub(super) fn from_lookups(
        callbacks: &LayoutPass<'pass>,
        node: Node,
        data: &'pass NodeData,
        style_payloads: Option<&'pass FfiStylePayloads>,
    ) -> Self {
        Self {
            callbacks: *callbacks,
            node,
            data,
            style_payloads,
        }
    }

    pub(super) fn data(&self) -> &'pass NodeData {
        self.data
    }

    pub(super) fn style_payloads(&self) -> Option<&'pass FfiStylePayloads> {
        self.style_payloads
    }

    fn parent_data(&self) -> Option<&'pass NodeData> {
        let parent = self.data().parent.get();
        (!parent.is_invalid()).then(|| self.callbacks.node_data(parent))
    }

    pub(super) fn style(&self) -> StyleValues<'pass> {
        StyleValues::new(
            self.style_payloads
                .expect("styled node must publish its style container before layout"),
        )
    }

    pub(super) fn computed_values_view_if_styled(&self) -> Option<ComputedValuesView<'pass>> {
        self.style_payloads
            .map(|payloads| ComputedValuesView::new(&payloads.groups))
    }

    pub(super) fn parent_computed_values_view_if_styled(&self) -> Option<ComputedValuesView<'pass>> {
        let parent = self.data().parent.get();
        if parent.is_invalid() {
            return None;
        }
        self.callbacks.computed_values_view_if_styled(parent)
    }

    #[inline]
    fn replaced_content(&self) -> crate::layout::FfiReplacedContentFacts {
        if self.data().kind.get() == NodeKind::SVGSVGBox {
            return self.svg_root_replaced_content();
        }
        let Some(facts) = self.callbacks.replaced_content_facts(self.node) else {
            // The kind check is cheap enough for release builds; the style
            // half of the enrollment predicate is debug-only because this
            // miss path is the steady state for ordinary boxes.
            assert!(
                !node_may_have_replaced_content_facts(self.data()),
                "replaced content facts were not synced to the arena before layout"
            );
            debug_assert!(
                !node_may_have_replaced_content_facts_including_size_containment(self.data()),
                "replaced content facts were not synced to the arena before layout"
            );
            return crate::layout::FfiReplacedContentFacts::default();
        };
        facts
    }

    /// An <svg> root's natural size resolves the lengths its element published against its style and the
    /// viewport, so the pass negotiates it rather than reading synced facts.
    #[cold]
    fn svg_root_replaced_content(&self) -> crate::layout::FfiReplacedContentFacts {
        let mut facts = derived_replaced_content_facts(self.data(), ReplacedContentInput::None);
        let arena = self.callbacks.arena();
        let is_document_element_box = arena.is_document_svg_root_box(self.node);
        let size_contained = self.node_has_size_containment();
        if size_contained && !is_document_element_box {
            return facts;
        }
        let (width, height, aspect_ratio) =
            super::svg_formatting_context::svg_root_natural_size(&self.callbacks, self.node);
        // An <object> showing the document is sized from what its <svg> document element would be
        // without size containment.
        if is_document_element_box {
            arena.note_document_svg_root_natural_size(
                self.node,
                NaturalSize {
                    width: width.map(CssPixels::raw_value),
                    height: height.map(CssPixels::raw_value),
                    aspect_ratio: aspect_ratio
                        .map(|(numerator, denominator)| (numerator.raw_value(), denominator.raw_value())),
                },
            );
        }
        if !size_contained {
            set_auto_content_size(
                &mut facts,
                AutoContentSize {
                    width,
                    height,
                    aspect_ratio,
                },
            );
        }
        facts
    }

    pub(crate) fn is_text_node(&self) -> bool {
        kind_is_text(self.data().kind.get())
    }

    pub(crate) fn is_break_node(&self) -> bool {
        self.data().kind.get() == NodeKind::BreakNode
    }

    pub(crate) fn is_box(&self) -> bool {
        kind_is_box(self.data().kind.get())
    }

    pub(crate) fn is_block_container(&self) -> bool {
        kind_is_block_container(self.data().kind.get())
    }

    pub(crate) fn is_replaced_box(&self) -> bool {
        kind_is_replaced_box(self.data().kind.get())
    }

    pub(crate) fn is_range_input_box(&self) -> bool {
        self.data().kind.get() == NodeKind::RangeInputBox
    }

    pub(crate) fn is_native_form_control_box(&self) -> bool {
        matches!(
            self.data().kind.get(),
            NodeKind::RangeInputBox | NodeKind::TextAreaBox | NodeKind::TextInputBox
        )
    }

    pub(crate) fn is_replaced_box_with_children(&self) -> bool {
        let data = self.data();
        kind_is_replaced_box(data.kind.get()) && node_can_have_children(data)
    }

    pub(crate) fn is_floating(&self) -> bool {
        self.computed_values_view_if_styled()
            .is_some_and(|style| style.is_floating())
    }

    pub(crate) fn is_absolutely_positioned(&self) -> bool {
        self.computed_values_view_if_styled()
            .is_some_and(|style| style.is_absolutely_positioned())
    }

    pub(crate) fn is_fixed_position(&self) -> bool {
        self.computed_values_view_if_styled()
            .is_some_and(|style| style.position() == crate::css::css_enums::positioning::FIXED)
    }

    pub(crate) fn is_relatively_positioned(&self) -> bool {
        self.computed_values_view_if_styled()
            .is_some_and(|style| style.position() == crate::css::css_enums::positioning::RELATIVE)
    }

    pub(crate) fn is_in_flow(&self) -> bool {
        !node_is_out_of_flow(self.data(), self.computed_values_view_if_styled())
    }

    pub(crate) fn is_floating_or_absolutely_positioned(&self) -> bool {
        self.computed_values_view_if_styled()
            .is_some_and(|style| style.is_floating() || style.is_absolutely_positioned())
    }

    pub(crate) fn is_inline(&self) -> bool {
        kind_is_text(self.data().kind.get())
            || self
                .computed_values_view_if_styled()
                .is_some_and(|style| style.display().is_inline_outside())
    }

    pub(crate) fn is_atomic_inline(&self) -> bool {
        node_is_atomic_inline(self.data(), self.computed_values_view_if_styled())
    }

    pub(crate) fn has_box_model_metrics(&self) -> bool {
        !matches!(
            self.data().kind.get(),
            NodeKind::Unset
                | NodeKind::Node
                | NodeKind::NodeWithStyle
                | NodeKind::GeneratedTextNode
                | NodeKind::TextNode
        )
    }

    pub(crate) fn is_fragmented_inline(&self) -> bool {
        node_is_fragmented_inline(self.data(), self.computed_values_view_if_styled())
    }

    /// A box whose committed geometry is its own single box record; a
    /// fragmented inline's committed geometry is its line pieces instead.
    pub(crate) fn is_non_fragmented_box(&self) -> bool {
        self.is_box() && !self.is_fragmented_inline()
    }

    pub(crate) fn is_inline_flow_interrupting_block(&self) -> bool {
        if !self.has_box_model_metrics() {
            return false;
        }
        let data = self.data();
        let Some(parent) = self.parent_data() else {
            return false;
        };
        let parent_is_inline_flow = self.parent_computed_values_view_if_styled().is_some_and(|style| {
            let display = style.display();
            display.is_inline_outside() && display.is_flow_inside()
        });
        if !parent_is_inline_flow {
            return false;
        }
        let style = self.computed_values_view_if_styled();
        if style.is_some_and(|style| style.display().is_inline_outside()) || node_is_out_of_flow(data, style) {
            return false;
        }
        let display = self.display();
        if display.is_contents() {
            return false;
        }
        if display.is_internal_table() || display.is_table_caption() {
            return false;
        }
        if parent.kind.get() == NodeKind::SVGForeignObjectBox {
            return false;
        }
        if kind_is_svg_box(data.kind.get()) || data.kind.get() == NodeKind::SVGForeignObjectBox {
            return false;
        }
        if data.kind.get() == NodeKind::SVGSVGBox
            && (kind_is_svg_box(parent.kind.get()) || parent.kind.get() == NodeKind::SVGSVGBox)
        {
            return false;
        }
        if kind_is_replaced_box(parent.kind.get()) && node_can_have_children(parent) {
            return false;
        }
        true
    }

    pub(crate) fn is_list_item_marker_box(&self) -> bool {
        self.data().kind.get() == NodeKind::ListItemMarkerBox
    }

    pub(crate) fn is_list_item_box(&self) -> bool {
        self.data().kind.get() == NodeKind::ListItemBox
    }

    pub(crate) fn is_svg_mask_box(&self) -> bool {
        self.data().kind.get() == NodeKind::SVGMaskBox
    }

    pub(crate) fn is_svg_clip_box(&self) -> bool {
        self.data().kind.get() == NodeKind::SVGClipBox
    }

    pub(crate) fn is_flow_layout_participant(&self) -> bool {
        self.is_box()
            && !self.is_absolutely_positioned()
            && !self.is_list_item_marker_box()
            && !self.is_svg_mask_box()
            && !self.is_svg_clip_box()
    }

    pub(crate) fn display_before_box_type_transformation_is_block_outside(&self) -> bool {
        self.computed_values_view_if_styled()
            .is_some_and(|style| style.display_before_box_type_transformation().is_block_outside())
    }

    pub(crate) fn inline_axis_is_reverse(&self) -> bool {
        self.style().inline_axis_is_reverse()
    }

    pub(crate) fn has_dom_node(&self) -> bool {
        !has_flag(self.data(), NodeFlag::Anonymous)
    }

    pub(crate) fn is_generated_for_pseudo_element(&self) -> bool {
        self.data().generated_for.get() != 0
    }

    pub(crate) fn children_are_inline(&self) -> bool {
        has_flag(self.data(), NodeFlag::ChildrenAreInline)
    }

    pub(crate) fn is_anonymous(&self) -> bool {
        has_flag(self.data(), NodeFlag::Anonymous)
    }

    pub(crate) fn has_anchor_names(&self) -> bool {
        has_flag(self.data(), NodeFlag::HasAnchorNames)
    }

    pub(crate) fn can_have_children(&self) -> bool {
        node_can_have_children(self.data())
    }

    pub(crate) fn has_replaced_element_table_display_adjustment(&self) -> bool {
        has_flag(self.data(), NodeFlag::IsReplacedElement)
            && self.computed_values_view_if_styled().is_some_and(|style| {
                let display = style.display_before_box_type_transformation();
                display.is_table_inside() || display.is_internal_table() || display.is_table_caption()
            })
    }

    pub(crate) fn creates_block_formatting_context(&self) -> bool {
        node_creates_block_formatting_context(
            self.data(),
            self.computed_values_view_if_styled(),
            self.parent_is_flex_or_grid_container(),
        )
    }

    pub(crate) fn parent_is_flex_or_grid_container(&self) -> bool {
        has_ancestor_fact(self.data(), AncestorFact::ParentIsFlexOrGridContainer)
    }

    pub(crate) fn parent_is_unfloated_flow_container(&self) -> bool {
        has_ancestor_fact(self.data(), AncestorFact::ParentIsUnfloatedFlowContainer)
    }

    pub(crate) fn is_anonymous_button_content_wrapper(&self) -> bool {
        has_ancestor_fact(self.data(), AncestorFact::IsAnonymousButtonContentWrapper)
    }

    pub(crate) fn is_anonymous_button_content_box(&self) -> bool {
        has_ancestor_fact(self.data(), AncestorFact::IsAnonymousButtonContentBox)
    }

    pub(crate) fn has_inline_level_inclusive_ancestor(&self) -> bool {
        has_ancestor_fact(self.data(), AncestorFact::HasInlineLevelInclusiveAncestor)
    }

    pub(crate) fn inherits_text_overflow_ellipsis(&self) -> bool {
        has_ancestor_fact(self.data(), AncestorFact::InheritsTextOverflowEllipsis)
    }

    pub(crate) fn is_editing_host(&self) -> bool {
        has_flag(self.data(), NodeFlag::IsEditingHost)
    }

    pub(crate) fn produces_line_box_fragment_when_empty(&self) -> bool {
        has_flag(self.data(), NodeFlag::ProducesLineBoxFragmentWhenEmpty)
    }

    pub(crate) fn uses_button_layout(&self) -> bool {
        has_flag(self.data(), NodeFlag::UsesButtonLayout)
    }

    // https://drafts.csswg.org/css2/#propdef-vertical-align
    // Applies to: inline-level and table-cell elements
    pub(crate) fn vertical_align_applies(&self) -> bool {
        let data = self.data();
        kind_is_box(data.kind.get())
            && (self.display().is_inline_outside() || self.display().is_table_cell())
            && !has_flag(data, NodeFlag::IsFlexItem)
            && !has_flag(data, NodeFlag::IsGridItem)
    }

    pub(crate) fn is_html_input_element(&self) -> bool {
        has_flag(self.data(), NodeFlag::IsHtmlInputElement)
    }

    pub(crate) fn is_fieldset_box(&self) -> bool {
        self.data().kind.get() == NodeKind::FieldSetBox
    }

    pub(crate) fn rendered_legend(&self) -> Node {
        let data = self.data();
        if data.kind.get() != NodeKind::FieldSetBox {
            return Node::INVALID;
        }
        let mut child = data.first_child.get();
        while !child.is_invalid() {
            let child_data = self.callbacks.node_data(child);
            if child_data.kind.get() == NodeKind::LegendBox
                && !node_is_out_of_flow(child_data, self.callbacks.computed_values_view_if_styled(child))
            {
                return child;
            }
            child = child_data.next_sibling.get();
        }
        Node::INVALID
    }

    pub(crate) fn list_item_marker(&self) -> Node {
        if !self.is_list_item_box() {
            return Node::INVALID;
        }
        // Outside markers are direct children. Inside markers participate in
        // an inline run and can therefore move below anonymous block wrappers.
        // Walk those wrappers, but not nested authored or generated list items.
        let mut candidate = self.data().first_child.get();
        while !candidate.is_invalid() {
            let data = self.callbacks.node_data(candidate);
            if data.kind.get() == NodeKind::ListItemMarkerBox {
                return candidate;
            }
            if data.kind.get() == NodeKind::BlockContainer
                && has_flag(data, NodeFlag::Anonymous)
                && data.generated_for.get() == 0
                && !data.first_child.get().is_invalid()
            {
                candidate = data.first_child.get();
                continue;
            }
            let mut current_data = data;
            loop {
                if !current_data.next_sibling.get().is_invalid() {
                    candidate = current_data.next_sibling.get();
                    break;
                }
                if current_data.parent.get() == self.node {
                    return Node::INVALID;
                }
                current_data = self.callbacks.node_data(current_data.parent.get());
            }
        }
        Node::INVALID
    }

    pub(crate) fn list_marker_is_inside(&self) -> bool {
        has_flag(self.data(), NodeFlag::ListMarkerIsInside)
    }

    pub(crate) fn has_auto_content_width(&self) -> bool {
        self.replaced_content().has_auto_content_width
    }

    pub(crate) fn auto_content_width(&self) -> crate::layout::CssPixels {
        self.replaced_content().auto_content_width
    }

    pub(crate) fn has_auto_content_height(&self) -> bool {
        self.replaced_content().has_auto_content_height
    }

    pub(crate) fn auto_content_height(&self) -> crate::layout::CssPixels {
        self.replaced_content().auto_content_height
    }

    pub(crate) fn has_auto_content_aspect_ratio(&self) -> bool {
        self.replaced_content().auto_content_aspect_ratio_denominator != crate::layout::CssPixels::default()
    }

    pub(crate) fn auto_content_aspect_ratio_numerator(&self) -> crate::layout::CssPixels {
        self.replaced_content().auto_content_aspect_ratio_numerator
    }

    pub(crate) fn auto_content_aspect_ratio_denominator(&self) -> crate::layout::CssPixels {
        self.replaced_content().auto_content_aspect_ratio_denominator
    }

    pub(crate) fn has_auto_content_box_size(&self) -> bool {
        node_has_auto_content_box_size(self.data())
    }

    pub(crate) fn node_has_size_containment(&self) -> bool {
        node_style_view(self.data()).is_some_and(style_has_size_containment)
    }

    // https://drafts.csswg.org/css-contain-2/#containment-inline-size
    // "Giving an element inline-size containment applies size containment to the inline-axis sizing of its principal
    //  box."
    pub(crate) fn node_has_inline_size_containment(&self) -> bool {
        // NB: So it has no effect where size containment would have none.
        let display = self.display();
        if display.is_table_inside() || display.is_internal_table() {
            return false;
        }
        let style = self.style();
        style.has_inline_size_containment() || style.is_inline_size_container()
    }

    pub(crate) fn has_preferred_aspect_ratio(&self) -> bool {
        self.preferred_aspect_ratio().is_some()
    }

    pub(crate) fn preferred_aspect_ratio(&self) -> Option<formatting_context::PixelFraction> {
        let style = self.style();
        if !self.node_has_size_containment() && style.aspect_ratio_uses_natural_when_available() {
            let replaced = self.replaced_content();
            if replaced.auto_content_aspect_ratio_denominator != crate::layout::CssPixels::default() {
                return Some(formatting_context::PixelFraction {
                    numerator: replaced.auto_content_aspect_ratio_numerator,
                    denominator: replaced.auto_content_aspect_ratio_denominator,
                });
            }
        }
        let (numerator, denominator) = style.css_preferred_aspect_ratio();
        (denominator != crate::layout::CssPixels::default())
            .then_some(formatting_context::PixelFraction { numerator, denominator })
    }

    pub(crate) fn has_default_preferred_width(&self) -> bool {
        self.replaced_content().has_default_preferred_width
    }

    pub(crate) fn default_preferred_width(&self) -> crate::layout::CssPixels {
        self.replaced_content().default_preferred_width
    }

    pub(crate) fn has_default_preferred_height(&self) -> bool {
        self.replaced_content().has_default_preferred_height
    }

    pub(crate) fn default_preferred_height(&self) -> crate::layout::CssPixels {
        self.replaced_content().default_preferred_height
    }

    pub(crate) fn initial_containing_block_inline_size(&self) -> crate::layout::CssPixels {
        self.callbacks.initial_containing_block_inline_size
    }

    pub(crate) fn is_scroll_container(&self) -> bool {
        kind_and_style_make_scroll_container(self.data().kind.get(), self.computed_values_view_if_styled())
    }

    pub(crate) fn display(&self) -> crate::layout::FfiDisplay {
        if self.data().style.get().is_null() {
            return crate::layout::FfiDisplay::block();
        }
        self.style().display()
    }

    pub(crate) fn is_svg_box(&self) -> bool {
        kind_is_svg_box(self.data().kind.get())
    }

    pub(crate) fn is_svg_svg_box(&self) -> bool {
        self.data().kind.get() == NodeKind::SVGSVGBox
    }

    pub(crate) fn is_table_box(&self) -> bool {
        self.display().is_table_inside()
    }

    pub(crate) fn is_table_wrapper(&self) -> bool {
        self.data().kind.get() == NodeKind::TableWrapper
    }

    pub(crate) fn is_table_row_group(&self) -> bool {
        self.display().is_table_row_group()
    }

    pub(crate) fn is_table_header_group(&self) -> bool {
        self.display().is_table_header_group()
    }

    pub(crate) fn is_table_footer_group(&self) -> bool {
        self.display().is_table_footer_group()
    }

    pub(crate) fn is_table_row_group_kind(&self) -> bool {
        self.display().is_table_row_group_kind()
    }

    pub(crate) fn is_table_row(&self) -> bool {
        self.display().is_table_row()
    }

    pub(crate) fn is_table_cell(&self) -> bool {
        self.display().is_table_cell()
    }

    pub(crate) fn is_table_column_group(&self) -> bool {
        self.display().is_table_column_group()
    }

    pub(crate) fn is_table_column(&self) -> bool {
        self.display().is_table_column()
    }

    pub(crate) fn is_table_caption(&self) -> bool {
        self.display().is_table_caption()
    }

    pub(crate) fn is_viewport(&self) -> bool {
        self.data().kind.get() == NodeKind::Viewport
    }

    pub(crate) fn document_in_quirks_mode(&self) -> bool {
        self.callbacks.document_in_quirks_mode
    }

    pub(crate) fn is_in_user_agent_shadow_tree(&self) -> bool {
        has_flag(self.data(), NodeFlag::IsInUserAgentShadowTree)
    }

    pub(crate) fn is_html_html_element(&self) -> bool {
        has_flag(self.data(), NodeFlag::IsHtmlHtmlElement)
    }

    pub(crate) fn is_html_body_element(&self) -> bool {
        has_flag(self.data(), NodeFlag::IsBody)
    }
}

#[cfg(test)]
mod node_facts_tests {
    use crate::css::style::{NaturalSize, ReplacedContentInput};
    use crate::layout::CssPixels;
    use crate::layout::node_data::{NodeData, NodeFlag, NodeKind};

    fn data_with_kind(kind: NodeKind) -> NodeData {
        let mut data = NodeData::default();
        *data.kind.get_mut() = kind;
        data
    }

    #[test]
    fn child_policy_follows_kind_and_replaced_shadow_root_flag() {
        assert!(!super::node_can_have_children(&data_with_kind(NodeKind::BreakNode)));
        assert!(super::node_can_have_children(&data_with_kind(
            NodeKind::ListItemMarkerBox
        )));
        assert!(!super::node_can_have_children(&data_with_kind(NodeKind::ImageBox)));
        assert!(!super::node_can_have_children(&data_with_kind(NodeKind::ReplacedBox)));
        assert!(!super::node_can_have_children(&data_with_kind(
            NodeKind::NavigableContainerViewport
        )));
        assert!(super::node_can_have_children(&data_with_kind(NodeKind::SVGSVGBox)));
        assert!(super::node_can_have_children(&data_with_kind(NodeKind::BlockContainer)));
        assert!(super::node_can_have_children(&data_with_kind(NodeKind::InlineNode)));
        assert!(super::node_can_have_children(&data_with_kind(NodeKind::TextNode)));

        let mut media = data_with_kind(NodeKind::AudioBox);
        assert!(!super::node_can_have_children(&media));
        *media.flags.get_mut() = NodeFlag::ReplacedBoxCanHaveChildren as u32;
        assert!(super::node_can_have_children(&media));
        *media.kind.get_mut() = NodeKind::VideoBox;
        assert!(super::node_can_have_children(&media));
    }
    fn horizontal_style() -> super::ReplacedContentStyle {
        super::ReplacedContentStyle {
            size_contained: None,
            zero_advance: CssPixels::from_integer(8),
            line_height: CssPixels::from_integer(16),
            is_horizontal: true,
            appearance_is_none: false,
        }
    }

    fn auto_content_size(facts: crate::layout::FfiReplacedContentFacts) -> Option<(i64, i64)> {
        (facts.has_auto_content_width && facts.has_auto_content_height).then(|| {
            (
                i64::from(facts.auto_content_width.to_int()),
                i64::from(facts.auto_content_height.to_int()),
            )
        })
    }

    #[test]
    fn a_vertical_textarea_takes_its_columns_in_the_block_axis() {
        let input = ReplacedContentInput::TextArea { cols: 10, rows: 3 };
        let horizontal = super::replaced_content_facts(NodeKind::TextAreaBox, horizontal_style(), input);
        assert_eq!(auto_content_size(horizontal), Some((80, 48)));
        let vertical_style = super::ReplacedContentStyle {
            is_horizontal: false,
            ..horizontal_style()
        };
        let vertical = super::replaced_content_facts(NodeKind::TextAreaBox, vertical_style, input);
        assert_eq!(auto_content_size(vertical), Some((48, 80)));
    }

    #[test]
    fn a_text_entry_input_without_appearance_gets_a_default_preferred_size() {
        let input = ReplacedContentInput::Input {
            size: 20,
            is_text_entry: true,
        };
        let text_input = super::replaced_content_facts(NodeKind::TextInputBox, horizontal_style(), input);
        assert_eq!(auto_content_size(text_input), Some((160, 16)));
        assert!(!text_input.has_default_preferred_width);

        let primitive_style = super::ReplacedContentStyle {
            appearance_is_none: true,
            ..horizontal_style()
        };
        let primitive = super::replaced_content_facts(NodeKind::BlockContainer, primitive_style, input);
        assert_eq!(auto_content_size(primitive), None);
        assert!(primitive.has_default_preferred_width && primitive.has_default_preferred_height);
        assert_eq!(primitive.default_preferred_width.to_int(), 160);

        let checkbox_input = ReplacedContentInput::Input {
            size: 20,
            is_text_entry: false,
        };
        let checkbox = super::replaced_content_facts(NodeKind::BlockContainer, primitive_style, checkbox_input);
        assert!(!checkbox.has_default_preferred_width);
    }

    #[test]
    fn an_empty_canvas_has_no_aspect_ratio() {
        let empty = super::replaced_content_facts(
            NodeKind::CanvasBox,
            horizontal_style(),
            ReplacedContentInput::Canvas { width: 0, height: 150 },
        );
        assert_eq!(auto_content_size(empty), Some((0, 150)));
        assert_eq!(empty.auto_content_aspect_ratio_denominator, CssPixels::default());
        let sized = super::replaced_content_facts(
            NodeKind::CanvasBox,
            horizontal_style(),
            ReplacedContentInput::Canvas {
                width: 300,
                height: 150,
            },
        );
        assert_eq!(sized.auto_content_aspect_ratio_numerator.to_int(), 300);
        assert_eq!(sized.auto_content_aspect_ratio_denominator.to_int(), 150);
    }

    #[test]
    fn a_video_without_a_size_has_no_natural_size_and_containment_overrides_any() {
        let without_size = super::replaced_content_facts(
            NodeKind::VideoBox,
            horizontal_style(),
            ReplacedContentInput::NaturalSize(NaturalSize::default()),
        );
        assert!(!without_size.has_auto_content_width && !without_size.has_auto_content_height);
        let contained_style = super::ReplacedContentStyle {
            size_contained: Some((CssPixels::from_integer(5), CssPixels::default())),
            ..horizontal_style()
        };
        let contained = super::replaced_content_facts(
            NodeKind::VideoBox,
            contained_style,
            ReplacedContentInput::NaturalSize(NaturalSize::default()),
        );
        assert_eq!(auto_content_size(contained), Some((5, 0)));
    }
}

/// Writes the natural size of the document's `<svg>` document element as its last layout
/// negotiated it to `natural_size` and returns true, unless it is what the last call handed over.
/// The size is all absent if the document element is no `<svg>` with a box.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and `natural_size` writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_take_changed_document_svg_root_natural_size(
    host: *const crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    natural_size: *mut crate::painting::host::FfiNaturalSize,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    let changed = unsafe {
        super::shell_reads::read_arena(host, read, (), |arena, ()| {
            arena.take_changed_document_svg_root_natural_size()
        })
    };
    let Some(changed) = changed else {
        return false;
    };
    // SAFETY: Guaranteed by the caller.
    unsafe { natural_size.write(changed.into()) };
    true
}
