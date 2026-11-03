/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

pub(crate) fn display_is_none(raw: u32) -> bool {
    FfiDisplay::from_raw(raw).is_none()
}

/// The element facts the box type transformation needs, marshalled by the C++
/// side. The parent display is the first non-`display: contents` ancestor's.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiBoxTypeTransformationInput {
    pub display: FfiDisplay,
    /// Computed position and float keywords.
    pub position: u16,
    pub float_value: u16,
    pub is_br_element: bool,
    pub is_document_element: bool,
    pub is_mathml_element: bool,
    pub is_mathml_mtable: bool,
    pub is_mathml_mtr: bool,
    pub is_mathml_mtd: bool,
    pub has_parent_display: bool,
    pub parent_display: FfiDisplay,
    /// The computed writing-mode of the element and of the box it would be the child of.
    pub writing_mode: u8,
    pub parent_writing_mode: u8,
    /// Whether the element is an SVG element, whose boxes are not laid out by the CSS display types.
    pub is_svg_element: bool,
    pub is_wbr_element: bool,
    pub disallow_display_contents: bool,
    pub rewrite_inline_flow: bool,
    pub is_button_element: bool,
    pub force_line_height_normal: bool,
    pub check_input_line_height: bool,
    pub hide_audio_without_controls: bool,
    pub is_table_element: bool,
    pub force_position_static: bool,
    pub force_symbol_display_inline: bool,
    pub webkit_box_layout_transformation_applies: bool,
}

/// Whether element-specific adjustments apply to the subject being computed.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum StyleAdjustmentTarget {
    Element,
    PseudoElement,
}

/// Build the shared adjustment inputs from element facts. The caller supplies the first
/// non-`display: contents` ancestor's display, using live styles on the C++ path.
pub(crate) fn rust_box_type_transformation_input(
    facts: u32,
    target: StyleAdjustmentTarget,
    has_parent_display: bool,
    parent_display: FfiDisplay,
    parent_writing_mode: u8,
) -> FfiBoxTypeTransformationInput {
    use crate::css::style::bridge::element_adjustment_fact as fact;

    let has = |bit| facts & bit != 0;
    let adjust_element = target == StyleAdjustmentTarget::Element;
    FfiBoxTypeTransformationInput {
        display: crate::css::display::FfiDisplay::inline(),
        position: keyword::STATIC,
        float_value: keyword::NONE,
        is_br_element: adjust_element && has(fact::IS_BR),
        is_document_element: adjust_element && has(fact::IS_DOCUMENT_ELEMENT),
        is_mathml_element: has(fact::IS_MATHML),
        is_mathml_mtable: has(fact::IS_MATHML_MTABLE),
        is_mathml_mtr: has(fact::IS_MATHML_MTR),
        is_mathml_mtd: has(fact::IS_MATHML_MTD),
        has_parent_display,
        parent_display,
        writing_mode: crate::css::css_enums::writing_mode::HORIZONTAL_TB,
        parent_writing_mode,
        is_svg_element: has(fact::IS_SVG_ELEMENT),
        is_wbr_element: adjust_element && has(fact::IS_WBR),
        disallow_display_contents: adjust_element && has(fact::DISALLOW_DISPLAY_CONTENTS),
        rewrite_inline_flow: adjust_element && has(fact::REWRITE_INLINE_FLOW),
        is_button_element: adjust_element && has(fact::IS_BUTTON),
        force_line_height_normal: adjust_element && has(fact::FORCE_LINE_HEIGHT_NORMAL),
        check_input_line_height: adjust_element && has(fact::CHECK_INPUT_LINE_HEIGHT),
        hide_audio_without_controls: adjust_element && has(fact::HIDE_AUDIO_WITHOUT_CONTROLS),
        is_table_element: adjust_element && has(fact::IS_TABLE),
        force_position_static: adjust_element && has(fact::FORCE_POSITION_STATIC),
        force_symbol_display_inline: adjust_element && has(fact::FORCE_SYMBOL_DISPLAY_INLINE),
        webkit_box_layout_transformation_applies: false,
    }
}

/// The box-type transformation input of a style target's animated composition, which the host
/// samples over the record the engine holds for it: the same input the engine drives the record
/// against, read from the element's published facts and its parent's record. A target the engine
/// does not know is transformed with no facts and no parent display.
#[unsafe(no_mangle)]
pub extern "C" fn rust_animated_box_type_transformation_input(
    host: &crate::render_state::DocumentHost,
    read: &crate::render_state::BegunRead,
    node: u32,
    pseudo_kind: u32,
) -> FfiBoxTypeTransformationInput {
    let pseudo_kind = pseudo_kind as u8;
    match crate::css::style::tree::StyleNodeID::from_raw(node) {
        Some(node) => crate::css::style::engine_calls::with_engine(read, host, |engine| {
            engine.composition_box_type_transformation_input(node, pseudo_kind)
        }),
        None => rust_box_type_transformation_input(
            0,
            StyleAdjustmentTarget::Element,
            false,
            FfiDisplay::block(),
            crate::css::css_enums::writing_mode::HORIZONTAL_TB,
        ),
    }
}

/// Result of the box type transformation: whether float must be reset to none,
/// and the possibly replaced display.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiBoxTypeTransformation {
    pub set_float_none: bool,
    pub changed_display: bool,
    pub display: FfiDisplay,
}

#[derive(Clone, Copy)]
pub struct ElementStyleAdjustment {
    pub changed_display: bool,
    pub display: FfiDisplay,
    pub set_line_height_normal: bool,
    pub check_input_line_height: bool,
    pub set_position_static: bool,
    pub changed_text_align: bool,
    pub text_align: u16,
}

/// The automatic box type transformation and element-specific adjustment,
/// sequenced as they are in the normal property computation path.
#[derive(Clone, Copy)]
pub struct ElementStyleAdjustments {
    pub box_type: FfiBoxTypeTransformation,
    pub element_style: ElementStyleAdjustment,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiInputLineHeightMetrics {
    pub current_line_height: f64,
    pub minimum_line_height: f64,
}

pub(super) fn should_clamp_input_line_height(
    adjustment: &ElementStyleAdjustment,
    metrics: &FfiInputLineHeightMetrics,
) -> bool {
    adjustment.check_input_line_height && metrics.current_line_height < metrics.minimum_line_height
}

pub(super) fn adjust_element_style(
    input: &FfiBoxTypeTransformationInput,
    display: FfiDisplay,
    text_align: u16,
) -> ElementStyleAdjustment {
    let mut new_display = display;
    if input.is_wbr_element && display.is_contents() {
        new_display = FfiDisplay::none();
    }
    if input.disallow_display_contents && new_display.is_contents() {
        new_display = FfiDisplay::none();
    }
    if input.rewrite_inline_flow
        && new_display.is_outside_and_inside()
        && new_display.is_inline_outside()
        && new_display.inside == display_inside::FLOW
    {
        new_display = FfiDisplay::inline_block();
    }
    if input.is_button_element
        && !(new_display.is_flex_inside()
            || new_display.is_grid_inside()
            || new_display.is_none()
            || new_display.is_contents())
    {
        new_display = if new_display.is_inline_outside() && !new_display.is_internal_ruby() {
            FfiDisplay::inline_block()
        } else {
            FfiDisplay::flow_root()
        };
    }
    if input.is_br_element {
        new_display = if new_display.is_contents() {
            FfiDisplay::none()
        } else if new_display.is_none() {
            new_display
        } else {
            FfiDisplay::inline()
        };
    }
    if input.hide_audio_without_controls {
        new_display = FfiDisplay::none();
    }
    if input.force_symbol_display_inline {
        new_display = FfiDisplay::inline();
    }

    let new_text_align = if input.is_table_element
        && matches!(
            text_align,
            keyword::_LIBWEB_LEFT | keyword::_LIBWEB_CENTER | keyword::_LIBWEB_RIGHT
        ) {
        keyword::START
    } else {
        text_align
    };

    ElementStyleAdjustment {
        changed_display: new_display != display,
        display: new_display,
        set_line_height_normal: input.force_line_height_normal,
        check_input_line_height: input.check_input_line_height,
        set_position_static: input.force_position_static,
        changed_text_align: new_text_align != text_align,
        text_align: new_text_align,
    }
}

pub(super) fn compute_element_style_adjustments(
    input: &FfiBoxTypeTransformationInput,
    text_align: u16,
) -> ElementStyleAdjustments {
    let box_type = transform_box_type(input);
    let display = if box_type.changed_display {
        box_type.display
    } else {
        input.display
    };
    let element_style = adjust_element_style(input, display, text_align);
    ElementStyleAdjustments {
        box_type,
        element_style,
    }
}

// NB: css-display-3 also defines inlinification, but nothing triggers it yet (the only
//     candidate is the ruby containment FIXME below), so only blockification is modeled.
pub(super) enum BoxTypeTransformation {
    None,
    Blockify,
}

pub(super) fn required_box_type_transformation(input: &FfiBoxTypeTransformationInput) -> BoxTypeTransformation {
    // NOTE: We never blockify <br> elements. They are always inline.
    //       There is currently no way to express in CSS how a <br> element really behaves.
    //       Spec issue: https://github.com/whatwg/html/issues/2291
    if input.is_br_element {
        return BoxTypeTransformation::None;
    }

    // Absolute positioning or floating an element blockifies the box's display type. [CSS2]
    if input.position == keyword::ABSOLUTE || input.position == keyword::FIXED || input.float_value != keyword::NONE {
        return BoxTypeTransformation::Blockify;
    }

    // FIXME: Containment in a ruby container inlinifies the box's display type, as described in [CSS-RUBY-1].

    // A parent with a grid or flex display value blockifies the box's display type. [CSS-GRID-1] [CSS-FLEXBOX-1]
    // NB: The C++ side supplies the first ancestor that is not `display: contents`; for a
    //     pseudo-element that climb starts at the originating element itself.
    if input.has_parent_display && (input.parent_display.is_grid_inside() || input.parent_display.is_flex_inside()) {
        return BoxTypeTransformation::Blockify;
    }

    BoxTypeTransformation::None
}

/// https://drafts.csswg.org/css-display/#transformations
/// 2.7. Automatic Box Type Transformations
pub(super) fn transform_box_type(input: &FfiBoxTypeTransformationInput) -> FfiBoxTypeTransformation {
    let display_before_transformation = input.display;
    let mut display = display_before_transformation;

    // https://drafts.csswg.org/css-overflow-4/#webkit-line-clamp
    // If the specified value of the 'display' property is '-webkit-box', the computed value becomes 'flow-root'
    // and the box establishes a BFC.
    if input.webkit_box_layout_transformation_applies && display.is_webkit_box_inside() {
        display = if display.is_inline_outside() {
            FfiDisplay::inline_block()
        } else {
            FfiDisplay::flow_root()
        };
    }
    let unchanged = |set_float_none: bool| FfiBoxTypeTransformation {
        set_float_none,
        changed_display: display != display_before_transformation,
        display,
    };

    // Some layout effects require blockification or inlinification of the box type,
    // which sets the box's computed outer display type to block or inline (respectively).
    // (This has no effect on display types that generate no box at all, such as none or contents.)
    if display.is_none() || (display.is_contents() && !input.is_document_element) {
        return unchanged(false);
    }

    // https://drafts.csswg.org/css-display/#root
    // The root element's display type is always blockified, and its principal box always establishes an independent formatting context.
    if input.is_document_element && !display.is_block_outside() {
        return FfiBoxTypeTransformation {
            set_float_none: false,
            changed_display: true,
            display: FfiDisplay::block(),
        };
    }

    let mut new_display = display;

    if display.is_math_inside() {
        // https://w3c.github.io/mathml-core/#new-display-math-value
        // For elements that are not MathML elements, if the specified value of display is inline math or block math
        // then the computed value is block flow and inline flow respectively.
        if !input.is_mathml_element {
            new_display = FfiDisplay::outside_and_inside(display.outside, display_inside::FLOW, display.list_item);
        }
        // For the mtable element the computed value is block table and inline table respectively.
        else if input.is_mathml_mtable {
            new_display = FfiDisplay::outside_and_inside(display.outside, display_inside::TABLE, display.list_item);
        }
        // For the mtr element, the computed value is table-row.
        else if input.is_mathml_mtr {
            new_display = FfiDisplay::internal(display_internal::TABLE_ROW);
        }
        // For the mtd element, the computed value is table-cell.
        else if input.is_mathml_mtd {
            new_display = FfiDisplay::internal(display_internal::TABLE_CELL);
        }
    }

    // https://www.w3.org/TR/CSS2/visuren.html#dis-pos-flo
    // If 'position' has the value 'absolute' or 'fixed', [...] 'float' is set to 'none'
    let set_float_none = input.position == keyword::ABSOLUTE || input.position == keyword::FIXED;

    match required_box_type_transformation(input) {
        BoxTypeTransformation::None => {}
        BoxTypeTransformation::Blockify => {
            if display.is_block_outside() {
                return unchanged(set_float_none);
            }
            // If a layout-internal box is blockified, its inner display type converts to flow so that it becomes a block container.
            if display.is_internal() {
                new_display = FfiDisplay::block();
            } else {
                assert!(display.is_outside_and_inside());

                // For legacy reasons, if an inline block box (inline flow-root) is blockified, it becomes a block box (losing its flow-root nature).
                // For consistency, a run-in flow-root box also blockifies to a block box.
                if display.is_inline_block() {
                    new_display =
                        FfiDisplay::outside_and_inside(display_outside::BLOCK, display_inside::FLOW, display.list_item);
                } else {
                    new_display =
                        FfiDisplay::outside_and_inside(display_outside::BLOCK, display.inside, display.list_item);
                }
            }
        }
    }

    // https://drafts.csswg.org/css-writing-modes-4/#block-flow
    // If a box has a different 'writing-mode' value than its parent box (i.e. nearest ancestor without
    // 'display: contents'):
    if input.has_parent_display
        && input.writing_mode != input.parent_writing_mode
        && !input.is_svg_element
        && new_display.is_outside_and_inside()
    {
        // - If the box would otherwise become an in-flow box with a computed 'display' of 'inline', its 'display'
        //   computes instead to 'inline-block'.
        // - If the box is a block container, then it establishes an independent block formatting context.
        // - More generally, if its specified inner display type is 'flow', then its computed inner display type
        //   becomes 'flow-root'.
        // NB: Layout makes a block container with another writing mode than its parent box establish an
        //     independent formatting context, so only the inline box's display changes here.
        if new_display.is_outside_and_inside()
            && new_display.inside == display_inside::FLOW
            && new_display.outside == display_outside::INLINE
        {
            new_display = FfiDisplay::outside_and_inside(
                new_display.outside,
                crate::css::css_enums::display_inside::FLOW_ROOT,
                new_display.list_item,
            );
        }
    }

    FfiBoxTypeTransformation {
        set_float_none,
        changed_display: new_display != display_before_transformation,
        display: new_display,
    }
}

/// Result of resolving the effective overflow pair: each axis' possibly
/// replaced computed keyword.
pub struct EffectiveOverflow {
    pub changed_x: bool,
    pub x_keyword: u16,
    pub changed_y: bool,
    pub y_keyword: u16,
}

/// https://www.w3.org/TR/css-overflow-3/#overflow-control
/// The visible/clip values of overflow compute to auto/hidden (respectively) if one of overflow-x or
/// overflow-y is neither visible nor clip.
pub(super) fn resolve_effective_overflow_keywords(overflow_x: u16, overflow_y: u16) -> EffectiveOverflow {
    let is_visible_or_clip = |keyword: u16| keyword == keyword::VISIBLE || keyword == keyword::CLIP;
    let mut result = EffectiveOverflow {
        changed_x: false,
        x_keyword: overflow_x,
        changed_y: false,
        y_keyword: overflow_y,
    };
    if !is_visible_or_clip(overflow_x) || !is_visible_or_clip(overflow_y) {
        if overflow_x == keyword::VISIBLE {
            result.changed_x = true;
            result.x_keyword = keyword::AUTO;
        }
        if overflow_x == keyword::CLIP {
            result.changed_x = true;
            result.x_keyword = keyword::HIDDEN;
        }
        if overflow_y == keyword::VISIBLE {
            result.changed_y = true;
            result.y_keyword = keyword::AUTO;
        }
        if overflow_y == keyword::CLIP {
            result.changed_y = true;
            result.y_keyword = keyword::HIDDEN;
        }
    }
    result
}

// https://drafts.csswg.org/css-color-adjust-1/#determine-the-used-color-scheme
pub(super) fn resolve_effective_color_scheme(
    schemes: &[u8],
    preferred_scheme: u8,
    document_supported_schemes: Option<&[u8]>,
) -> u8 {
    const AUTO: u8 = 0;
    const LIGHT: u8 = 2;

    // To determine the used color scheme of an element:

    // 1. If the user’s preferred color scheme, as indicated by the prefers-color-scheme media feature,
    //    is present among the listed color schemes, and is supported by the user agent,
    //    that’s the element’s used color scheme.
    if preferred_scheme != AUTO && schemes.contains(&preferred_scheme) {
        return preferred_scheme;
    }

    // 2. Otherwise, if the user has indicated an overriding preference for their chosen color scheme,
    //    and the only keyword is not present in color-scheme for the element,
    //    the user agent must override the color scheme with the user’s preferred color scheme.
    //    See § 2.3 Overriding the Color Scheme.
    // FIXME: We don't currently support setting an "overriding preference" for color schemes.

    // 3. Otherwise, if the user agent supports at least one of the listed color schemes,
    //    the used color scheme is the first supported color scheme in the list.
    if let Some(first_supported) = schemes.iter().find(|scheme| **scheme != AUTO) {
        return *first_supported;
    }

    // 4. Otherwise, the used color scheme is the browser default. (Same as normal.)
    // `normal` indicates that the element supports the page’s supported color schemes, if they are set
    if let Some(document_supported_schemes) = document_supported_schemes {
        if preferred_scheme != AUTO && document_supported_schemes.contains(&preferred_scheme) {
            return preferred_scheme;
        }
        if let Some(first_supported) = document_supported_schemes.iter().find(|scheme| **scheme != AUTO) {
            return *first_supported;
        }
    }

    LIGHT
}

/// Resolves the used color scheme for a value that required C++ computation.
///
/// # Safety
/// `input` must point at valid document inputs for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_resolve_effective_color_scheme(
    value: &StyleValueData,
    input: &FfiEffectiveColorSchemeInput,
) -> u8 {
    let StyleValueData::ColorScheme(color_scheme_payload) = value else {
        unreachable!("computed color-scheme must have color-scheme data");
    };
    let ColorSchemePayload { scheme_codes, .. } = &**color_scheme_payload;
    resolve_effective_color_scheme(scheme_codes.as_slice(), input.preferred_color_scheme, unsafe {
        input.document_supported_schemes()
    })
}

/// Result of the text-align adjustment: the replacement keyword and whether it
/// counts as inherited.
pub struct TextAlignAdjustment {
    pub changed: bool,
    pub keyword: u16,
    pub inherited: bool,
}

/// Decides the text-align adjustments applied after computation. The parent
/// arguments are only read when `has_parent_with_computed_values` is set.
pub(super) fn compute_text_align_adjustment(
    text_align: u16,
    is_th_element: bool,
    has_parent_with_computed_values: bool,
    parent_text_align: u16,
    parent_direction_is_ltr: bool,
) -> TextAlignAdjustment {
    let unchanged = TextAlignAdjustment {
        changed: false,
        keyword: text_align,
        inherited: false,
    };
    let replace = |keyword: u16| TextAlignAdjustment {
        changed: true,
        keyword,
        inherited: false,
    };

    // https://drafts.csswg.org/css-text-4/#valdef-text-align-match-parent
    // This value behaves the same as inherit (computes to its parent's computed value) except that an inherited
    // value of start or end is interpreted against the parent's direction value and results in a computed value of
    // either left or right. Computes to start when specified on the root element.
    if text_align == keyword::MATCH_PARENT {
        if !has_parent_with_computed_values {
            return replace(keyword::START);
        }
        return match parent_text_align {
            keyword::START if parent_direction_is_ltr => replace(keyword::LEFT),
            keyword::START => replace(keyword::RIGHT),
            keyword::END if parent_direction_is_ltr => replace(keyword::RIGHT),
            keyword::END => replace(keyword::LEFT),
            _ => replace(parent_text_align),
        };
    }

    // AD-HOC: The -libweb-inherit-or-center style defaults to centering, unless the parent element has a
    //         non-initial computed text-align value. This is used to support the ad-hoc default <th>
    //         text-align behavior.
    if text_align == keyword::_LIBWEB_INHERIT_OR_CENTER && is_th_element {
        if has_parent_with_computed_values && parent_text_align != keyword::START {
            return TextAlignAdjustment {
                changed: true,
                keyword: parent_text_align,
                inherited: true,
            };
        }
        return replace(keyword::CENTER);
    }

    unchanged
}

/// Applies the box type transformation and element adjustments to an animated composition, in its overlay, over a base
/// that stays as computed: what the transformation changes about the animated display, position, float, line-height and
/// overflow is what the overlay holds.
pub(super) fn finalize_animated_box_type(
    mut box_type: FfiBoxTypeTransformationInput,
    longhands: crate::css::style::ViewLonghands<'_>,
    display_before_box_type_transformation: u32,
    overlay: &mut AnimatedOverlay,
    input_line_height_metrics: Option<&FfiInputLineHeightMetrics>,
) {
    use crate::css::property_metadata::property_id as prop;

    let view = Some(&*overlay);
    let text_align = effective_keyword(longhands, view, prop::TEXT_ALIGN);
    let animated_display_missing = overlay.get(prop::DISPLAY).is_none();
    box_type.display = if animated_display_missing {
        FfiDisplay::from_raw(display_before_box_type_transformation)
    } else {
        longhands.effective_display(view)
    };
    box_type.position = effective_keyword(longhands, view, prop::POSITION);
    box_type.float_value = effective_keyword(longhands, view, prop::FLOAT);
    box_type.writing_mode = longhands.effective_writing_mode(view);
    box_type.webkit_box_layout_transformation_applies = webkit_box_layout_transformation_applies(longhands, view);
    // An axis the overlay does not animate is adjusted from its keyword before the base adjusted it against the other
    // axis, which the overlay may animate out of that adjustment.
    let before = longhands.overflow_before_adjustment();
    let unadjusted = |property: u16, axis: usize| match before {
        Some(before) if overlay.get(property).is_none() => before[axis],
        _ => effective_keyword(longhands, view, property),
    };
    let overflow =
        resolve_effective_overflow_keywords(unadjusted(prop::OVERFLOW_X, 0), unadjusted(prop::OVERFLOW_Y, 1));
    let adjustments = compute_element_style_adjustments(&box_type, text_align);

    // A value the overlay animates keeps its animation's place; one it does not is adjusted over the base value.
    let mut adjust = |property_id: u16, keyword_or_display: StyleValueData| {
        let value = retained_new(keyword_or_display);
        let effective = longhands.effective_value(Some(&*overlay), property_id);
        if effective.data() == value.data() {
            return;
        }
        match overlay
            .get(property_id)
            .map(|entry| (entry.inherited, entry.result_of_transition))
        {
            Some((inherited, result_of_transition)) => {
                overlay.set_owned(property_id, value, inherited, result_of_transition);
            }
            None if property_id == prop::OVERFLOW_X || property_id == prop::OVERFLOW_Y => {
                overlay.set_adjusted(property_id, value);
            }
            None => overlay.set_owned(property_id, value, false, false),
        }
    };
    let display = |display: FfiDisplay| StyleValueData::Display { raw: display.encoded() };
    let keyword = |keyword: u16| StyleValueData::Keyword { keyword };
    if animated_display_missing {
        adjust(prop::DISPLAY, display(box_type.display));
    }
    if adjustments.box_type.set_float_none {
        adjust(prop::FLOAT, keyword(keyword::NONE));
    }
    if adjustments.box_type.changed_display {
        adjust(prop::DISPLAY, display(adjustments.box_type.display));
    }
    let element_style = adjustments.element_style;
    if element_style.changed_display {
        adjust(prop::DISPLAY, display(element_style.display));
    }
    if element_style.set_position_static {
        adjust(prop::POSITION, keyword(keyword::STATIC));
    }
    if element_style.changed_text_align {
        adjust(prop::TEXT_ALIGN, keyword(element_style.text_align));
    }
    if element_style.set_line_height_normal
        || (element_style.check_input_line_height
            && should_clamp_input_line_height(
                &element_style,
                input_line_height_metrics.expect("input line-height adjustment requires font metrics"),
            ))
    {
        adjust(prop::LINE_HEIGHT, keyword(keyword::NORMAL));
    }
    adjust(prop::OVERFLOW_X, keyword(overflow.x_keyword));
    adjust(prop::OVERFLOW_Y, keyword(overflow.y_keyword));
}

/// Applies the box type transformation and element adjustments to the animated composition `animated_overlay` holds
/// over the longhands of `record`, whose display before the box type transformation the host settled as
/// `display_before_box_type_transformation`.
///
/// # Safety
/// `record` must be a style record the caller holds a reference to.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_finalize_animated_box_type(
    box_type: FfiBoxTypeTransformationInput,
    record: *const c_void,
    display_before_box_type_transformation: u32,
    animated_overlay: &mut AnimatedOverlay,
    input_line_height_metrics: Option<&FfiInputLineHeightMetrics>,
) {
    // SAFETY: Guaranteed by the caller.
    let longhands = unsafe { crate::css::style::ViewLonghands::of_raw_record(record) };
    finalize_animated_box_type(
        box_type,
        longhands,
        display_before_box_type_transformation,
        animated_overlay,
        input_line_height_metrics,
    );
}
