/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Registered indices of the style groups the layout engine reads, pinned to
// the C++ StyleGroupIndex enum by static_asserts in LayoutRustBridge.cpp.
pub const STYLE_GROUP_INDEX_INHERITED_TABLE: usize = 0;
pub const STYLE_GROUP_INDEX_GRID: usize = 9;
pub const STYLE_GROUP_INDEX_INHERITED_TEXT: usize = 4;
pub const STYLE_GROUP_INDEX_INHERITED_BOX: usize = 5;
pub const STYLE_GROUP_INDEX_FONT: usize = 6;
pub const STYLE_GROUP_INDEX_SVG_RESET: usize = 8;
pub const STYLE_GROUP_INDEX_BORDER: usize = 17;
pub const STYLE_GROUP_INDEX_ALIGNMENT: usize = 18;
pub const STYLE_GROUP_INDEX_SIZING: usize = 20;
pub const STYLE_GROUP_INDEX_SURROUND: usize = 21;
pub const STYLE_GROUP_INDEX_BOX: usize = 22;

pub type FfiReleaseAnchorNameHandleCallback = unsafe extern "C" fn(usize);

// https://drafts.csswg.org/css-contain-2/#containment-types
fn containment_applies_to_principal_box(display: crate::css::display::FfiDisplay) -> bool {
    if display.is_internal_table() && !display.is_table_cell() {
        return false;
    }
    if display.is_inline_outside() && display.is_flow_inside() {
        return false;
    }
    true
}

/// The four inset properties; the discriminant indexes the anchor-inset
/// store fields.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum InsetField {
    Top = 0,
    Right = 1,
    Bottom = 2,
    Left = 3,
}

/// A resolved px-or-auto inset written back by anchor resolution.
#[derive(Clone, Copy)]
pub(crate) struct ResolvedInsetOverride {
    pub(crate) is_auto: bool,
    pub(crate) px: CssPixels,
}

#[derive(Default)]
struct AnchorInsetField {
    /// Resolved value written by replace_resolved_anchor_insets. It takes
    /// precedence over every style read and reports
    /// contains_anchor_function() == false; the abspos engine's early-out on
    /// re-entry depends on both properties.
    resolved_override: Cell<Option<ResolvedInsetOverride>>,
    /// Memoized in-crate calculated wrapper for a bare anchor() inset: the
    /// single owner of the Arc that the returned inset values borrow.
    /// Written at most once and never replaced or dropped before the owning
    /// LayoutState drops.
    wrapper: std::cell::OnceCell<std::sync::Arc<crate::css::style_value::StyleValueData>>,
}

#[derive(Default)]
struct AnchorInsetSlot {
    fields: [AnchorInsetField; 4],
}

/// Per-LayoutState side store for the four inset fields of anchor-positioned
/// nodes, the only style reads whose results are not a pure function of the
/// immutable group payloads.
#[derive(Default)]
pub(crate) struct AnchorInsetStore {
    slots: PagedStore<AnchorInsetSlot>,
    /// False until the first override write, so inset reads on anchor-free
    /// pages never touch the slots store.
    any_overrides: Cell<bool>,
}

impl AnchorInsetStore {
    fn slot(&self, slot_index: u32) -> &AnchorInsetSlot {
        self.slots
            .get(slot_index)
            .unwrap_or_else(|| self.slots.allocate(slot_index, AnchorInsetSlot::default()))
    }

    fn override_for(&self, slot_index: u32, field: InsetField) -> Option<ResolvedInsetOverride> {
        if !self.any_overrides.get() {
            return None;
        }
        self.slots.get(slot_index)?.fields[field as usize].resolved_override.get()
    }

    fn memoized_bare_anchor_wrapper(
        &self,
        slot_index: u32,
        field: InsetField,
        build_wrapper: impl FnOnce() -> std::sync::Arc<crate::css::style_value::StyleValueData>,
    ) -> &crate::css::style_value::StyleValueData {
        self.slot(slot_index).fields[field as usize]
            .wrapper
            .get_or_init(build_wrapper)
    }

    pub(crate) fn set_override(&self, slot_index: u32, field: InsetField, value: ResolvedInsetOverride) {
        self.slot(slot_index).fields[field as usize].resolved_override.set(Some(value));
        self.any_overrides.set(true);
    }
}

#[derive(Clone, Copy)]
pub(crate) enum InsetValue<'a> {
    FromStyle(&'a ComputedLengthPercentageOrAuto),
    BareAnchor(&'a crate::css::style_value::StyleValueData),
    Resolved(ResolvedInsetOverride),
}

impl InsetValue<'_> {
    pub(crate) fn auto_value() -> Self {
        Self::Resolved(ResolvedInsetOverride {
            is_auto: true,
            px: CssPixels::default(),
        })
    }

    pub(crate) fn is_auto(self) -> bool {
        match self {
            Self::FromStyle(value) => value.is_auto(),
            Self::BareAnchor(_) => false,
            Self::Resolved(resolved) => resolved.is_auto,
        }
    }

    pub(crate) fn to_px(self, reference: CssPixels) -> CssPixels {
        match self {
            Self::FromStyle(value) => value.to_px(reference),
            Self::BareAnchor(wrapper) => resolve_calc_to_px(std::ptr::from_ref(wrapper).cast(), reference),
            Self::Resolved(resolved) if resolved.is_auto => CssPixels::default(),
            Self::Resolved(resolved) => resolved.px,
        }
    }

    pub(crate) fn contains_percentage(self) -> bool {
        match self {
            Self::FromStyle(value) => value.contains_percentage(),
            Self::BareAnchor(_) | Self::Resolved(_) => false,
        }
    }

    pub(crate) fn contains_anchor_function(self) -> bool {
        match self {
            Self::FromStyle(value) => value
                .length_percentage()
                .is_some_and(|length_percentage| length_percentage.contains_anchor_function()),
            Self::BareAnchor(_) => true,
            Self::Resolved(_) => false,
        }
    }

    /// The calculated style value carrying the anchor() function, for the
    /// abspos engine's anchor-aware resolution.
    pub(crate) fn anchor_bearing_calculated(self) -> *const c_void {
        match self {
            Self::FromStyle(value) => value
                .length_percentage()
                .expect("anchor-bearing inset must hold a style value")
                .calculated_pointer(),
            Self::BareAnchor(wrapper) => std::ptr::from_ref(wrapper).cast(),
            Self::Resolved(_) => unreachable!("resolved inset overrides never carry anchor functions"),
        }
    }
}

/// The computed `aspect-ratio` <ratio> term as a CSSPixels fraction. A zero
/// denominator means no usable ratio (none specified, degenerate, or collapsed
/// to zero by the fixed-point conversion).
fn decode_css_preferred_aspect_ratio(
    ratio: &crate::css::computed_value_types::ComputedAspectRatio,
) -> (CssPixels, CssPixels) {
    let no_usable_ratio = (CssPixels::default(), CssPixels::default());
    if !ratio.has_preferred_ratio {
        return no_usable_ratio;
    }
    let numerator = ratio.preferred_ratio_numerator;
    let denominator = ratio.preferred_ratio_denominator;
    let is_degenerate = !numerator.is_finite() || numerator == 0.0 || !denominator.is_finite() || denominator == 0.0;
    if is_degenerate {
        return no_usable_ratio;
    }
    let (numerator, denominator) = CssPixels::fraction_nearest_values_for(numerator, denominator);
    if numerator.raw_value() == 0 {
        return no_usable_ratio;
    }
    (numerator, denominator)
}

/// A thin Rust-only view over a node's immutable computed-value group
/// payloads; every read hands out payload references or lazy views over the
/// typed group payloads. During a layout pass the four inset fields
/// additionally consult the per-LayoutState anchor-inset store, the only
/// style state a pass can change; outside a pass (tree building, node facts)
/// the store is absent and the inset accessors must not be used.
#[derive(Clone, Copy)]
pub(crate) struct StyleValues<'a> {
    payloads: &'a FfiStylePayloads,
    anchor_insets: Option<&'a AnchorInsetStore>,
    slot_index: u32,
    vertical_align_override: u16,
}

macro_rules! scalar_accessors {
    ($($group:ident: { $($name:ident: $ty:ty => $($field:ident).+,)+ })+) => {
        impl StyleValues<'_> {
            $($(
                #[inline]
                pub(crate) fn $name(self) -> $ty {
                    self.$group().$($field).+
                }
            )+)+
        }
    };
}

scalar_accessors! {
    box_values: {
        display: FfiDisplay => display,
        display_before_box_type_transformation: FfiDisplay => display_before_box_type_transformation,
        position: u8 => position,
        float_: u8 => float_,
        clear: u8 => clear,
        box_sizing: u8 => box_sizing,
        overflow_x: u8 => overflow_x,
        overflow_y: u8 => overflow_y,
        text_overflow: u8 => text_overflow,
        table_layout: u8 => table_layout,
        unicode_bidi: u8 => unicode_bidi,
        grid_auto_flow_row: bool => grid_auto_flow_row,
        grid_auto_flow_dense: bool => grid_auto_flow_dense,
    }
    border_facts: {
        border_top_width: CssPixels => border_top.width,
        border_right_width: CssPixels => border_right.width,
        border_bottom_width: CssPixels => border_bottom.width,
        border_left_width: CssPixels => border_left.width,
        border_top_style: u8 => border_top.line_style,
        border_right_style: u8 => border_right.line_style,
        border_bottom_style: u8 => border_bottom.line_style,
        border_left_style: u8 => border_left.line_style,
        border_top_color: u32 => border_top.color,
        border_right_color: u32 => border_right.color,
        border_bottom_color: u32 => border_bottom.color,
        border_left_color: u32 => border_left.color,
    }
    inherited_box: {
        writing_mode: u8 => writing_mode,
        direction: u8 => direction,
        visibility: u8 => visibility,
    }
    inherited_table: {
        border_collapse: u8 => border_collapse,
        caption_side: u8 => caption_side,
    }
    inherited_text_facts: {
        text_align: u8 => text_align,
        text_justify: u8 => text_justify,
        white_space_collapse: u8 => white_space_collapse,
        text_wrap_mode: u8 => text_wrap_mode,
        word_break: u8 => word_break,
        letter_spacing: CssPixels => letter_spacing,
        word_spacing: CssPixels => word_spacing,
    }
    font_facts: {
        font_variant_emoji: u8 => font_variant_emoji,
        line_height: CssPixels => line_height_used,
        font_size: CssPixels => font_size,
    }
    alignment: {
        flex_direction: u8 => flex_direction,
        flex_wrap: u8 => flex_wrap,
        flex_grow: f64 => flex_grow,
        flex_shrink: f64 => flex_shrink,
        order: i32 => order,
        align_items: u8 => align_items,
        align_self: u8 => align_self,
        align_content: u8 => align_content,
        justify_content: u8 => justify_content,
        justify_items: u8 => justify_items,
        justify_self: u8 => justify_self,
    }
}

impl<'a> StyleValues<'a> {
    #[inline]
    pub(crate) fn new(payloads: &'a FfiStylePayloads, anchor_insets: &'a AnchorInsetStore, slot_index: u32) -> Self {
        Self {
            payloads,
            anchor_insets: Some(anchor_insets),
            slot_index,
            vertical_align_override: u16::MAX,
        }
    }

    /// A reader without the layout pass state: payload-backed reads only,
    /// with the inset accessors unavailable.
    #[inline]
    pub(crate) fn from_payloads(payloads: &'a FfiStylePayloads) -> Self {
        Self {
            payloads,
            anchor_insets: None,
            slot_index: 0,
            vertical_align_override: u16::MAX,
        }
    }

    #[inline]
    fn native_group<T>(self, group_index: usize) -> &'a T {
        let payload = self.payloads.groups[group_index];
        debug_assert!(!payload.is_null());
        // SAFETY: The payload is the Rust-defined group struct itself; C++
        // derives its mirror from the cbindgen twin of the same type, and the
        // node's ComputedValues keep it alive while readers exist.
        unsafe { &*payload.cast::<T>() }
    }

    #[inline]
    fn sizing(self) -> &'a crate::layout::SizingValues {
        self.native_group(STYLE_GROUP_INDEX_SIZING)
    }

    #[inline]
    fn surround(self) -> &'a crate::layout::SurroundValues {
        self.native_group(STYLE_GROUP_INDEX_SURROUND)
    }

    #[inline]
    fn alignment(self) -> &'a crate::layout::AlignmentValues {
        self.native_group(STYLE_GROUP_INDEX_ALIGNMENT)
    }

    #[inline]
    fn svg_reset(self) -> &'a crate::layout::SVGResetValues {
        self.native_group(STYLE_GROUP_INDEX_SVG_RESET)
    }

    #[inline]
    fn inherited_box(self) -> &'a crate::css::computed_values::InheritedBoxValues {
        self.native_group(STYLE_GROUP_INDEX_INHERITED_BOX)
    }

    #[inline]
    fn inherited_table(self) -> &'a crate::css::computed_values::InheritedTableValues {
        self.native_group(STYLE_GROUP_INDEX_INHERITED_TABLE)
    }

    #[inline]
    fn box_values(self) -> &'a crate::layout::BoxValues {
        self.native_group(STYLE_GROUP_INDEX_BOX)
    }

    #[inline]
    pub(crate) fn grid_values(self) -> &'a crate::layout::GridValues {
        self.native_group(STYLE_GROUP_INDEX_GRID)
    }

    #[inline]
    fn border_facts(self) -> &'a crate::layout::BorderLayoutFacts {
        self.native_group(STYLE_GROUP_INDEX_BORDER)
    }

    #[inline]
    fn inherited_text_facts(self) -> &'a crate::layout::InheritedTextLayoutFacts {
        self.native_group(STYLE_GROUP_INDEX_INHERITED_TEXT)
    }

    #[inline]
    fn font_facts(self) -> &'a crate::layout::FontLayoutFacts {
        self.native_group(STYLE_GROUP_INDEX_FONT)
    }

    pub(crate) fn is_floating(self) -> bool {
        self.box_values().float_ != crate::css::css_enums::float::NONE
    }

    pub(crate) fn is_absolutely_positioned(self) -> bool {
        matches!(
            self.box_values().position,
            crate::css::css_enums::positioning::ABSOLUTE | crate::css::css_enums::positioning::FIXED
        )
    }

    // https://developer.mozilla.org/en-US/docs/Web/Guide/CSS/Block_formatting_context
    // The computed-style-only half of the block-formatting-context predicate;
    // node_creates_block_formatting_context adds the terms that need the node
    // kind, stamped DOM identity, the live IsFlexItem flag, or the parent's
    // display. The float term is deliberately absent for the same reason: only
    // non-flex-items establish one by floating.
    pub(crate) fn own_style_establishes_block_formatting_context(self) -> bool {
        let box_values = self.box_values();
        let display = box_values.display;

        if self.is_absolutely_positioned() {
            return true;
        }

        if display.is_inline_block() {
            return true;
        }

        if display.is_table_cell() || display.is_table_caption() {
            return true;
        }

        let overflow_establishes_context = |overflow: u8| {
            overflow != crate::css::css_enums::overflow::VISIBLE && overflow != crate::css::css_enums::overflow::CLIP
        };
        if overflow_establishes_context(box_values.overflow_x) || overflow_establishes_context(box_values.overflow_y) {
            return true;
        }

        if display.is_flow_root_inside() {
            return true;
        }

        // https://drafts.csswg.org/css-contain-2/#containment-types
        // 1. The layout containment box establishes an independent formatting context.
        // 4. The paint containment box establishes an independent formatting context.
        let content_visibility_forces_containment = self.inherited_box().content_visibility
            == crate::css::css_enums::content_visibility::AUTO;
        if (box_values.layout_containment || box_values.paint_containment || content_visibility_forces_containment)
            && containment_applies_to_principal_box(display)
        {
            return true;
        }

        // https://drafts.csswg.org/css-conditional-5/#valdef-container-type-size
        // Applies style containment and size containment to the principal box, and establishes an independent
        // formatting context.
        if box_values.is_size_container || box_values.is_inline_size_container {
            return true;
        }

        // https://drafts.csswg.org/css-multicol-2/#the-multi-column-model
        // An element whose 'column-width', 'column-count', or 'column-height' property is not 'auto' establishes a
        // multi-column container (or multicol container for short), and therefore acts as a container for
        // multi-column layout.
        if box_values.column_width.kind != crate::layout::ComputedSizeKind::Auto || box_values.column_count_has_value {
            return true;
        }

        false
    }

    fn anchor_inset_handle(self, field: InsetField) -> Option<&'a crate::layout::ComputedStyleValueHandle> {
        let values = self.surround();
        let handle = match field {
            InsetField::Top => &values.top_anchor_inset,
            InsetField::Right => &values.right_anchor_inset,
            InsetField::Bottom => &values.bottom_anchor_inset,
            InsetField::Left => &values.left_anchor_inset,
        };
        (!handle.pointer.is_null()).then_some(handle)
    }

    fn inset_value(self, field: InsetField) -> InsetValue<'a> {
        let anchor_insets = self
            .anchor_insets
            .expect("inset reads require the layout pass anchor-inset store");
        // The resolved override must mask BOTH anchor representations: a
        // bare anchor() inset carries the surround anchor handle below,
        // while a calc() containing anchor() has a null handle and reads
        // from the stored inset value with contains_anchor_function() set.
        if let Some(resolved) = anchor_insets.override_for(self.slot_index, field) {
            return InsetValue::Resolved(resolved);
        }
        if let Some(handle) = self.anchor_inset_handle(field) {
            return InsetValue::BareAnchor(anchor_insets.memoized_bare_anchor_wrapper(
                self.slot_index,
                field,
                || {
                    // SAFETY: The handle is non-null, and the node's style
                    // group payload keeps the anchor value alive for the
                    // synchronous layout pass.
                    unsafe { crate::css::calc::create_anchor_inset_calculated(handle.pointer.cast()) }
                },
            ));
        }
        let values = self.surround();
        InsetValue::FromStyle(match field {
            InsetField::Top => &values.inset.top,
            InsetField::Right => &values.inset.right,
            InsetField::Bottom => &values.inset.bottom,
            InsetField::Left => &values.inset.left,
        })
    }

    pub(crate) fn inset_top(self) -> InsetValue<'a> {
        self.inset_value(InsetField::Top)
    }

    pub(crate) fn inset_right(self) -> InsetValue<'a> {
        self.inset_value(InsetField::Right)
    }

    pub(crate) fn inset_bottom(self) -> InsetValue<'a> {
        self.inset_value(InsetField::Bottom)
    }

    pub(crate) fn inset_left(self) -> InsetValue<'a> {
        self.inset_value(InsetField::Left)
    }

    pub(crate) fn row_gap(self) -> &'a ComputedGap {
        &self.alignment().row_gap
    }

    pub(crate) fn column_gap(self) -> &'a ComputedGap {
        &self.alignment().column_gap
    }

    pub(crate) fn x(self) -> LengthPercentageRef<'a> {
        self.svg_reset()
            .x
            .length_percentage()
            .expect("computed x lost its style value")
    }

    pub(crate) fn y(self) -> LengthPercentageRef<'a> {
        self.svg_reset()
            .y
            .length_percentage()
            .expect("computed y lost its style value")
    }

    pub(crate) fn text_indent(self) -> LengthPercentageRef<'a> {
        self.inherited_text_facts()
            .text_indent
            .length_percentage
            .length_percentage()
            .expect("computed text-indent lost its style value")
    }

    pub(crate) fn with_vertical_align_keyword(mut self, keyword: u8) -> Self {
        self.vertical_align_override = keyword as u16;
        self
    }

    pub(crate) fn vertical_align_is_keyword(self) -> bool {
        self.vertical_align_override != u16::MAX || self.box_values().vertical_align.is_keyword
    }

    pub(crate) fn vertical_align_keyword(self) -> u8 {
        if self.vertical_align_override != u16::MAX {
            self.vertical_align_override as u8
        } else {
            self.box_values().vertical_align.keyword
        }
    }

    pub(crate) fn vertical_align_value(self) -> LengthPercentageRef<'a> {
        self.box_values()
            .vertical_align
            .value
            .length_percentage()
            .expect("computed vertical-align lost its style value")
    }

    pub(crate) fn has_position_anchor(self) -> bool {
        self.surround().position_anchor_name.raw() != 0
    }

    /// The raw fly-string representation of the computed position-anchor
    /// name, borrowed from the surround payload for the duration of the pass.
    pub(crate) fn position_anchor_name(self) -> usize {
        self.surround().position_anchor_name.raw()
    }

    pub(crate) fn first_available_font(self) -> *const c_void {
        let font = self.font_facts().first_available_font;
        debug_assert!(!font.is_null(), "layout read a font group that never received a font list");
        font
    }

    pub(crate) fn font_cascade_list(self) -> *const c_void {
        let list = self.font_facts().font_cascade_list;
        debug_assert!(!list.is_null(), "layout read a font group that never received a font list");
        list
    }

    pub(crate) fn font_ascent(self) -> f32 {
        self.font_facts().font_ascent
    }

    pub(crate) fn font_descent(self) -> f32 {
        self.font_facts().font_descent
    }

    pub(crate) fn font_x_height(self) -> f32 {
        self.font_facts().font_x_height
    }

    pub(crate) fn box_sizing_for_aspect_ratio(self) -> u8 {
        let values = self.box_values();
        if values.aspect_ratio.use_natural_aspect_ratio_if_available {
            crate::css::css_enums::box_sizing::CONTENT_BOX
        } else {
            values.box_sizing
        }
    }

    pub(crate) fn css_preferred_aspect_ratio(self) -> (CssPixels, CssPixels) {
        decode_css_preferred_aspect_ratio(&self.box_values().aspect_ratio)
    }

    pub(crate) fn border_spacing_horizontal(self) -> CssPixels {
        CssPixels::from_raw(self.inherited_table().border_spacing_horizontal)
    }

    pub(crate) fn border_spacing_vertical(self) -> CssPixels {
        CssPixels::from_raw(self.inherited_table().border_spacing_vertical)
    }

    pub(crate) fn aspect_ratio_uses_natural_when_available(self) -> bool {
        self.box_values().aspect_ratio.use_natural_aspect_ratio_if_available
    }

    pub(crate) fn flex_basis_is_content(self) -> bool {
        self.alignment().flex_basis.is_content
    }

    pub(crate) fn flex_basis(self) -> &'a ComputedSize {
        let flex_basis = &self.alignment().flex_basis;
        if flex_basis.is_content {
            auto_computed_size()
        } else {
            &flex_basis.size
        }
    }

    pub(crate) fn width(self) -> &'a ComputedSize {
        &self.sizing().width
    }

    pub(crate) fn height(self) -> &'a ComputedSize {
        &self.sizing().height
    }

    pub(crate) fn min_width(self) -> &'a ComputedSize {
        &self.sizing().min_width
    }

    pub(crate) fn min_height(self) -> &'a ComputedSize {
        &self.sizing().min_height
    }

    pub(crate) fn max_width(self) -> &'a ComputedSize {
        &self.sizing().max_width
    }

    pub(crate) fn max_height(self) -> &'a ComputedSize {
        &self.sizing().max_height
    }

    pub(crate) fn column_width(self) -> &'a ComputedSize {
        &self.box_values().column_width
    }

    pub(crate) fn margin_top(self) -> &'a ComputedLengthPercentageOrAuto {
        &self.surround().margin.top
    }

    pub(crate) fn margin_right(self) -> &'a ComputedLengthPercentageOrAuto {
        &self.surround().margin.right
    }

    pub(crate) fn margin_bottom(self) -> &'a ComputedLengthPercentageOrAuto {
        &self.surround().margin.bottom
    }

    pub(crate) fn margin_left(self) -> &'a ComputedLengthPercentageOrAuto {
        &self.surround().margin.left
    }

    pub(crate) fn padding_top(self) -> &'a ComputedLengthPercentageOrAuto {
        &self.surround().padding.top
    }

    pub(crate) fn padding_right(self) -> &'a ComputedLengthPercentageOrAuto {
        &self.surround().padding.right
    }

    pub(crate) fn padding_bottom(self) -> &'a ComputedLengthPercentageOrAuto {
        &self.surround().padding.bottom
    }

    pub(crate) fn padding_left(self) -> &'a ComputedLengthPercentageOrAuto {
        &self.surround().padding.left
    }

    pub(crate) fn has_column_count(self) -> bool {
        self.box_values().column_count_has_value
    }

    pub(crate) fn column_count(self) -> i32 {
        self.box_values().column_count
    }

    pub(crate) fn has_size_containment(self) -> bool {
        self.box_values().size_containment
    }

    pub(crate) fn is_size_container(self) -> bool {
        self.box_values().is_size_container
    }

    pub(crate) fn text_indent_each_line(self) -> bool {
        self.inherited_text_facts().text_indent.each_line
    }

    pub(crate) fn text_indent_hanging(self) -> bool {
        self.inherited_text_facts().text_indent.hanging
    }

    pub(crate) fn tab_size_is_number(self) -> bool {
        self.inherited_text_facts().tab_size_is_number
    }

    pub(crate) fn tab_size(self) -> CssPixels {
        self.inherited_text_facts().tab_size_length
    }

    pub(crate) fn tab_size_number(self) -> f64 {
        self.inherited_text_facts().tab_size_number
    }
}

pub(crate) unsafe fn resolve_calc_with_external_resolutions(
    calculated: *const c_void,
    percentage_basis: CssPixels,
    callback_context: *mut c_void,
    resolve_non_math_function: Option<unsafe extern "C" fn(*mut c_void, *const c_void) -> *const c_void>,
) -> crate::css::calc::FfiResolvedCalc {
    use crate::css::calc::{
        FfiCalcExternalResolutionKind, rust_calc_external_resolutions, rust_calc_external_resolutions_release,
        rust_calc_resolve, rust_calc_root_from_calculated,
    };

    let mut context = px_calc_resolution_context(percentage_basis);
    let root = unsafe { rust_calc_root_from_calculated(calculated) };
    let external =
        unsafe { rust_calc_external_resolutions(root, context.basis_kind, context.basis_value, context.basis_unit) };
    context.external_resolutions = external.resolutions;
    context.external_resolution_count = external.resolution_count;
    if let Some(resolve_non_math_function) = resolve_non_math_function
        && external.resolution_count > 0
    {
        for resolution in unsafe { std::slice::from_raw_parts_mut(external.resolutions, external.resolution_count) } {
            if resolution.kind == FfiCalcExternalResolutionKind::NonMathFunction {
                resolution.resolved_node =
                    unsafe { resolve_non_math_function(callback_context, resolution.source) }.cast();
            }
        }
    }
    let result = unsafe { rust_calc_resolve(calculated, &raw const context, true) };
    unsafe {
        rust_calc_external_resolutions_release(external.storage);
    }
    result
}
