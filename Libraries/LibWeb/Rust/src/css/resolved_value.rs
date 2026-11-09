/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The resolved values of the properties whose resolved value is not their computed value.
//! https://drafts.csswg.org/cssom/#resolved-values

use core::ffi::c_void;

use crate::css::calc::canonical_unit_code;
use crate::css::color_conversion::{LAB, LCH, OKLAB, OKLCH};
use crate::css::color_resolution::{
    ColorResolutionInput, Rgba, SerializationBehavior, descriptor_for_color_type, resolve_color_mix,
    resolve_relative_form, to_color,
};
use crate::css::computed_value_types::{
    BackgroundValues, BorderValues, ComputedLengthPercentageOrAuto, ComputedShadow, ComputedStyleValueHandle,
    EffectsValues, InheritedTextValues, InheritedUIValues, MiscLayoutValues, TextResetValues, TransformValues,
};
use crate::css::computed_value_views::{ComputedValuesView, StyleRecordGroups, resolve_calc_to_px_without_rounding};
use crate::css::css_enums::{box_sizing, keyword, positioning, writing_mode};
use crate::css::css_pixels::{
    CssPixelRect, CssPixels, FfiCssPixelRect, FfiCssPixelSize, MAX_INTEGER_VALUE, MIN_INTEGER_VALUE,
};
use crate::css::property_metadata::{longhand_is_logical_alias, property_id};
use crate::css::serialize::color_function_has_missing_component;
use crate::css::serialize::transform_function;
use crate::css::style_compute::map_logical_alias_to_physical;
use crate::css::style_value::{
    ColorBase, ColorFunctionPayload, CssString, FILTER_KIND_DROP_SHADOW, RetainedStyleValueData,
    RetainedStyleValueDataList, ShadowPayload, StyleValueBox, StyleValueData, TransformationPayload,
    shared_style_value, value_depends_on_current_color,
};
use crate::css::style_value_arc::StyleValueArc;
use crate::css::table_group_builder::group_index;
use crate::css::transform_functions::multiply_transform_functions;
use crate::css::value_codes::{COLOR_SYNTAX_MODERN, COLOR_TYPE_RGB};
use crate::layout::node_data::NodeSlotId;
use crate::layout::script_entries::FfiAnchorSizedProperty;
use crate::painting::ffi::FfiBoxModelMetrics;
use crate::render_state::DocumentHost;

const VALUE_LIST_SEPARATOR_SPACE: u8 = 0;
const VALUE_LIST_SEPARATOR_COMMA: u8 = 1;
const SHADOW_TYPE_NORMAL: u8 = 0;
const SHADOW_TYPE_TEXT: u8 = 1;

fn number(value: f64) -> RetainedStyleValueData {
    RetainedStyleValueData::from_arc(shared_style_value(StyleValueData::Number { value }))
}

fn length_px(value: f64) -> RetainedStyleValueData {
    RetainedStyleValueData::from_arc(shared_style_value(StyleValueData::Length {
        value,
        unit: canonical_unit_code(&crate::css::style_compute::LENGTH_UNIT_CANONICAL_PX_RATIOS),
    }))
}

fn color_value(color: Rgba) -> StyleValueArc {
    StyleValueArc::new(StyleValueData::ColorFunction(StyleValueBox::new(
        ColorFunctionPayload {
            color_base: ColorBase {
                has_color_type: true,
                color_type: COLOR_TYPE_RGB,
                color_syntax: COLOR_SYNTAX_MODERN,
            },
            channel_0: number(f64::from(color.r)),
            channel_1: number(f64::from(color.g)),
            channel_2: number(f64::from(color.b)),
            alpha: number(f64::from(color.a) / 255.0),
            has_name: false,
            name: CssString::none(),
            origin_color: RetainedStyleValueData::none(),
        },
    )))
}

fn color_base(value: &StyleValueData) -> Option<&ColorBase> {
    match value {
        StyleValueData::ColorFunction(payload) => Some(&payload.color_base),
        StyleValueData::ColorMix(payload) => Some(&payload.color_base),
        StyleValueData::LightDark(payload) => Some(&payload.color_base),
        StyleValueData::ContrastColor { color_base, .. } => Some(color_base),
        _ => None,
    }
}

/// The resolved value of a color property, whose computed value is `value` and whose used color is `computed_color`.
/// A `style` of `None` keeps relative colors and mixes as they are.
fn resolve_color_style_value(
    value: &StyleValueArc,
    computed_color: Rgba,
    style: Option<&ColorResolutionInput>,
) -> StyleValueArc {
    let color_type = color_base(value)
        .filter(|base| base.has_color_type)
        .map(|base| base.color_type);

    if let Some(style) = style {
        if let StyleValueData::ColorFunction(color_function) = &**value
            && color_function.origin_color.optional_data().is_some()
            && color_type.is_some()
        {
            return match resolve_relative_form(value, style) {
                Some(resolved) => StyleValueArc::new(crate::css::color_resolution::relative_color_computed_value_form(
                    resolved,
                )),
                None => value.clone(),
            };
        }

        // NB: A computed color-mix() is still a mix only where it reads currentcolor, which the used value resolves.
        if matches!(&**value, StyleValueData::ColorMix(_))
            && let Some(resolved) = resolve_color_mix(value, style)
        {
            return StyleValueArc::new(resolved);
        }
    }

    if let StyleValueData::ColorFunction(_) = &**value {
        let serializes_as_color_function = color_type.is_some_and(|color_type| {
            matches!(
                descriptor_for_color_type(color_type).behavior,
                SerializationBehavior::ColorFunction
            )
        });
        if serializes_as_color_function {
            return value.clone();
        }
        // https://drafts.csswg.org/css-color-4/#css-serialization-of-srgb
        // NB: An sRGB value with missing components serializes them as none, which the used color cannot represent.
        if color_function_has_missing_component(value) {
            return value.clone();
        }
    }
    if color_type.is_some_and(|color_type| matches!(color_type, LAB | OKLAB | LCH | OKLCH)) {
        return value.clone();
    }

    color_value(computed_color)
}

// https://drafts.csswg.org/css-color-4/#resolving-color-values
// The resolved value of a <color> is its used value.
fn resolve_svg_color_style_value(value: &StyleValueArc, style: &ColorResolutionInput) -> StyleValueArc {
    if !value_depends_on_current_color(value) {
        return value.clone();
    }

    if let StyleValueData::ValueList {
        values,
        separator,
        collapsible,
    } = &**value
    {
        let values = values
            .as_slice()
            .iter()
            .map(|item| {
                RetainedStyleValueData::from_arc(resolve_svg_color_style_value(&item.clone().into_arc(), style))
            })
            .collect();
        return StyleValueArc::new(StyleValueData::ValueList {
            values: RetainedStyleValueDataList::from_retained_values(values),
            separator: *separator,
            collapsible: *collapsible,
        });
    }

    match to_color(value, style) {
        Some(color) => resolve_color_style_value(value, color, Some(style)),
        None => value.clone(),
    }
}

// The stored computed filter value keeps currentcolor inside drop-shadow(); the resolved value
// serializes it against the element's used color, like the color-valued properties.
fn resolve_filter_style_value(value: &StyleValueArc, current_color: Rgba) -> StyleValueArc {
    let StyleValueData::ValueList { values, .. } = &**value else {
        return value.clone();
    };
    let mut resolved_any = false;
    let filters = values
        .as_slice()
        .iter()
        .map(|filter_value| {
            if let Some(StyleValueData::Filter {
                kind: FILTER_KIND_DROP_SHADOW,
                color_operation,
                value: shadow,
            }) = filter_value.optional_data()
                && let Some(StyleValueData::Shadow(shadow)) = shadow.optional_data()
                && shadow.color.optional_data().is_none_or(
                    |color| matches!(color, StyleValueData::Keyword { keyword } if *keyword == keyword::CURRENTCOLOR),
                )
            {
                resolved_any = true;
                let shadow = StyleValueData::Shadow(StyleValueBox::new(ShadowPayload {
                    color: RetainedStyleValueData::from_arc(color_value(current_color)),
                    ..(**shadow).clone()
                }));
                return RetainedStyleValueData::from_arc(StyleValueArc::new(StyleValueData::Filter {
                    kind: FILTER_KIND_DROP_SHADOW,
                    color_operation: *color_operation,
                    value: RetainedStyleValueData::from_arc(StyleValueArc::new(shadow)),
                }));
            }
            filter_value.clone()
        })
        .collect();
    if !resolved_any {
        return value.clone();
    }
    StyleValueArc::new(StyleValueData::ValueList {
        values: RetainedStyleValueDataList::from_retained_values(filters),
        separator: VALUE_LIST_SEPARATOR_SPACE,
        collapsible: false,
    })
}

// Allowing exactly panning and pinch-zoom serializes as `manipulation`, whatever form specified it. The parser
// orders a list as horizontal, vertical, pinch-zoom, with at most one of each.
fn resolve_touch_action_style_value(value: &StyleValueArc) -> StyleValueArc {
    if let StyleValueData::ValueList { values, .. } = &**value {
        let is_keyword = |item: &RetainedStyleValueData, expected: u16| matches!(item.optional_data(), Some(StyleValueData::Keyword { keyword }) if *keyword == expected);
        if let [horizontal, vertical, _] = values.as_slice()
            && is_keyword(horizontal, keyword::PAN_X)
            && is_keyword(vertical, keyword::PAN_Y)
        {
            return shared_style_value(StyleValueData::Keyword {
                keyword: keyword::MANIPULATION,
            });
        }
    }
    value.clone()
}

fn style_value_for_shadow(shadow_type: u8, shadows: &[ComputedShadow], current_color: Rgba) -> StyleValueArc {
    let pixels = |raw: i32| length_px(CssPixels::from_raw(raw).to_double());
    let make_shadow_style_value = |shadow: &ComputedShadow| {
        let color = if shadow.color.is_current_color {
            current_color
        } else {
            Rgba::from_packed(shadow.color.rgba)
        };
        StyleValueArc::new(StyleValueData::Shadow(StyleValueBox::new(ShadowPayload {
            shadow_type,
            color: RetainedStyleValueData::from_arc(color_value(color)),
            offset_x: pixels(shadow.offset_x),
            offset_y: pixels(shadow.offset_y),
            blur_radius: pixels(shadow.blur_radius),
            spread_distance: pixels(shadow.spread_distance),
            placement: shadow.placement as u8,
        })))
    };

    match shadows {
        [] => shared_style_value(StyleValueData::Keyword { keyword: keyword::NONE }),
        [shadow] => make_shadow_style_value(shadow),
        shadows => StyleValueArc::new(StyleValueData::ValueList {
            values: RetainedStyleValueDataList::from_retained_values(
                shadows
                    .iter()
                    .map(|shadow| RetainedStyleValueData::from_arc(make_shadow_style_value(shadow)))
                    .collect(),
            ),
            separator: VALUE_LIST_SEPARATOR_COMMA,
            collapsible: true,
        }),
    }
}

/// A new reference to `value`, which the caller lends.
fn retained(value: &StyleValueData) -> StyleValueArc {
    // SAFETY: Every value the host lends is a counted one.
    unsafe {
        StyleValueArc::increment_strong_count(value);
        StyleValueArc::from_raw(value)
    }
}

const TIME_UNIT_SECONDS: u8 = 1;

fn length_value(pixels: f64) -> StyleValueArc {
    length_px(pixels).into_arc()
}

fn keyword_value(keyword: u16) -> StyleValueArc {
    shared_style_value(StyleValueData::Keyword { keyword })
}

fn absolute_length_px(value: &StyleValueData) -> Option<f64> {
    match value {
        StyleValueData::Length { value, unit } => crate::css::style_compute::absolute_length_to_px(*value, *unit),
        _ => None,
    }
}

fn handle_value(handle: &ComputedStyleValueHandle) -> Option<&StyleValueData> {
    // SAFETY: A non-null handle points at the style value the group payload retains.
    unsafe { handle.pointer.cast::<StyleValueData>().as_ref() }
}

fn style_value_for_length_percentage_or_auto(value: &ComputedLengthPercentageOrAuto) -> StyleValueArc {
    if value.is_auto {
        return keyword_value(keyword::AUTO);
    }
    retained(handle_value(&value.value).expect("a computed length-percentage holds a value"))
}

/// The value of a length-percentage against `reference`, in pixels and without rounding.
fn offset_in_px(offset: &StyleValueData, reference: CssPixels) -> f64 {
    match offset {
        StyleValueData::Percentage { value } => reference.to_double() * (value * 0.01),
        StyleValueData::Calculated(_) => {
            resolve_calc_to_px_without_rounding(std::ptr::from_ref(offset).cast(), reference)
        }
        value => absolute_length_px(value).expect("a computed length-percentage is absolute"),
    }
}

fn style_value_for_transform_origin(
    x: &StyleValueData,
    y: &StyleValueData,
    z: Option<&StyleValueData>,
    reference_box: Option<CssPixelRect>,
) -> StyleValueArc {
    let mut offsets = match reference_box {
        Some(reference_box) => vec![
            length_px(offset_in_px(x, reference_box.width)),
            length_px(offset_in_px(y, reference_box.height)),
        ],
        None => vec![
            RetainedStyleValueData::from_arc(retained(x)),
            RetainedStyleValueData::from_arc(retained(y)),
        ],
    };
    if let Some(z) = z {
        let z_offset = offset_in_px(z, CssPixels::from_raw(0));
        if z_offset != 0.0 {
            offsets.push(length_px(z_offset));
        }
    }
    StyleValueArc::new(StyleValueData::ValueList {
        values: RetainedStyleValueDataList::from_retained_values(offsets),
        separator: VALUE_LIST_SEPARATOR_SPACE,
        collapsible: false,
    })
}

// NB: The computed value of a min or max sizing property resolves its anchor-size() functions. Where layout did not
//     resolve them against an anchor box, they are unresolvable: each resolves to its fallback value, and one without
//     makes the declaration invalid at computed-value time.
fn resolve_unresolvable_anchor_functions(property: u16, computed_value: StyleValueArc) -> StyleValueArc {
    let mut is_invalid = false;
    let substituted =
        crate::css::calc::style_value_with_unresolvable_anchor_functions(&computed_value, &mut is_invalid);
    if !substituted.is_null() {
        // SAFETY: The call hands over one strong reference.
        return unsafe { StyleValueArc::from_raw(substituted) };
    }
    if is_invalid {
        return retained(crate::css::style_compute::initial_value(property).expect("a longhand has an initial value"));
    }
    computed_value
}

// https://drafts.csswg.org/css-transforms-1/#2d-matrix
fn is_2d_matrix(matrix: &libgfx_rust::FloatMatrix4x4) -> bool {
    // A 3x2 transformation matrix,
    // or a 4x4 matrix where the items m31, m32, m13, m23, m43, m14, m24, m34 are equal to 0
    // and m33, m44 are equal to 1.
    // NB: We only care about 4x4 matrices here.
    // NB: Our elements are 0-indexed not 1-indexed, and in the opposite order.
    // NB: We use epsilon comparisons here to account for inaccuracies when doing trigonometric calculations.
    let m = &matrix.elements;
    [m[0][2], m[1][2], m[2][0], m[2][1], m[2][3], m[3][0], m[3][1], m[3][2]]
        .iter()
        .all(|element| element.abs() <= f32::EPSILON)
        && (m[2][2] - 1.0).abs() <= f32::EPSILON
        && (m[3][3] - 1.0).abs() <= f32::EPSILON
}

fn transformation_value(transform_function: u8, parameters: &[f32]) -> StyleValueArc {
    StyleValueArc::new(StyleValueData::Transformation(StyleValueBox::new(
        TransformationPayload {
            property: property_id::TRANSFORM,
            transform_function,
            values: RetainedStyleValueDataList::from_retained_values(
                parameters
                    .iter()
                    .map(|&parameter| number(f64::from(parameter)))
                    .collect(),
            ),
            matrix_values: RetainedStyleValueDataList::from_retained_values(Vec::new()),
        },
    )))
}

/// What the host names for the box of the element whose resolved value is read.
#[repr(C)]
pub struct FfiResolvedValueBox {
    pub host: *const c_void,
    /// The element's layout node.
    pub node: u32,
    /// The layout node holding its insets and margins: its table wrapper for a table.
    pub principal_node: u32,
    pub has_containing_block: bool,
    /// The containing block of the principal node.
    pub containing_block: u32,
    pub has_scroll_container: bool,
    /// The nearest scroll container of the principal node, for sticky positioning.
    pub scroll_container: u32,
    /// The groups of the style of the element, or pseudo-element, itself.
    pub element_style: *const c_void,
    pub svg_sizing_properties_apply: bool,
    pub computed_value_context: *mut c_void,
    /// The computed value of a longhand of the element, as one strong reference.
    pub computed_value: unsafe extern "C" fn(context: *mut c_void, property: u16) -> *const StyleValueData,
}

struct ResolvedValueBox<'a> {
    query: &'a FfiResolvedValueBox,
    host: &'a DocumentHost,
    node: NodeSlotId,
    principal_node: NodeSlotId,
    style: ComputedValuesView<'a>,
    principal_style: ComputedValuesView<'a>,
    element_style: ComputedValuesView<'a>,
}

fn node_style(host: &DocumentHost, node: NodeSlotId) -> ComputedValuesView<'_> {
    let groups = crate::layout::shell_reads::layout_row_style(host, node);
    assert!(!groups.is_null(), "a layout node has a style");
    // SAFETY: The style of a layout node stays live while the host reads the node.
    ComputedValuesView::of_record_groups(unsafe { &*groups.cast::<StyleRecordGroups>() })
}

fn has_committed_box(host: &DocumentHost, node: NodeSlotId) -> bool {
    crate::painting::ffi::render_state_has_paintable_row(host, node)
}

fn box_model(host: &DocumentHost, node: NodeSlotId) -> FfiBoxModelMetrics {
    let mut metrics = FfiBoxModelMetrics::default();
    crate::layout::script_entries::layout_script_paintable_box_model(host, node, &mut metrics);
    metrics
}

fn content_size(host: &DocumentHost, node: NodeSlotId) -> FfiCssPixelSize {
    let mut size = FfiCssPixelSize::default();
    crate::layout::script_entries::layout_script_paintable_content_size(host, node, &mut size);
    size
}

fn transform_reference_box(host: &DocumentHost, node: NodeSlotId) -> CssPixelRect {
    let mut rect = FfiCssPixelRect::default();
    crate::painting::ffi::render_state_paintable_transform_reference_box(host, node, &mut rect);
    rect.into()
}

fn physical_property(property: u16, writing_mode: u8, direction: u8) -> u16 {
    if longhand_is_logical_alias(property) {
        return map_logical_alias_to_physical(property, writing_mode, direction);
    }
    property
}

#[derive(Clone, Copy)]
enum InsetAxis {
    Horizontal,
    Vertical,
}

impl ResolvedValueBox<'_> {
    fn computed(&self, property: u16) -> StyleValueArc {
        // SAFETY: The host passes a live context, and the call hands over one strong reference.
        unsafe { StyleValueArc::from_raw((self.query.computed_value)(self.query.computed_value_context, property)) }
    }

    fn containing_block(&self) -> Option<NodeSlotId> {
        self.query.has_containing_block.then_some(NodeSlotId {
            index: self.query.containing_block,
        })
    }

    fn has_used_box(&self, node: NodeSlotId, style: ComputedValuesView) -> bool {
        let display = style.display();
        !display.is_none() && !display.is_contents() && has_committed_box(self.host, node)
    }

    fn resolved_color(&self, property: u16, used_color: u32) -> StyleValueArc {
        resolve_color_style_value(
            &self.computed(property),
            Rgba::from_packed(used_color),
            Some(&ColorResolutionInput::for_style(self.style)),
        )
    }

    // https://drafts.csswg.org/cssom/#resolved-value-special-case-property-like-height
    // If the property applies to the element or pseudo-element and the resolved value of the display property is not
    // none or contents, then the resolved value is the used value. Otherwise the resolved value is the computed value.
    fn resolved_value_for_margin(
        &self,
        margin: &ComputedLengthPercentageOrAuto,
        used_value: impl FnOnce(&FfiBoxModelMetrics) -> CssPixels,
    ) -> StyleValueArc {
        if !self.has_used_box(self.principal_node, self.principal_style) {
            return style_value_for_length_percentage_or_auto(margin);
        }
        let used_value = used_value(&box_model(self.host, self.principal_node)).to_double();
        let containing_block = self.containing_block();
        let containing_block_style = containing_block.map(|node| node_style(self.host, node));
        // NB: Positioned boxes can have position-try overrides, and grid/flex items can use a specialized basis.
        let is_absolutely_positioned = matches!(
            self.principal_style.position(),
            positioning::ABSOLUTE | positioning::FIXED
        );
        // NB: Grid layout aligns an item in its grid area through the item's used margins, which then hold the offset
        //     of its alignment, and the space its auto margins absorb, as well as its margins. So an auto margin of a
        //     grid item resolves to zero, and a length to itself, as other engines report them.
        // FIXME: A percentage resolves against the item's grid area, which isn't known here, so it still resolves to
        //        the used value, alignment and all.
        if !is_absolutely_positioned && containing_block_style.is_some_and(|style| style.display().is_grid_inside()) {
            if margin.is_auto {
                return length_value(0.0);
            }
            if let Some(pixels) = handle_value(&margin.value).and_then(absolute_length_px) {
                return length_value(pixels);
            }
        }
        if !self.principal_style.display().is_block_outside()
            || is_absolutely_positioned
            || containing_block_style.is_some_and(|style| {
                let display = style.display();
                display.is_grid_inside() || display.is_flex_inside()
            })
        {
            return length_value(used_value);
        }
        let resolved_length = |pixels: f64| {
            // NB: Retain fractional precision within the layout range, and preserve saturation outside it.
            if pixels < f64::from(MIN_INTEGER_VALUE) || pixels > f64::from(MAX_INTEGER_VALUE) {
                return length_value(CssPixels::nearest_value_for(pixels).to_double());
            }
            length_value(pixels)
        };
        // INTEROP: Chrome and Safari exclude the adjustment that satisfies an over-constrained block's inline-size
        //          equation.
        let value = handle_value(&margin.value);
        if let Some(pixels) = value.filter(|_| !margin.is_auto).and_then(absolute_length_px) {
            return resolved_length(pixels);
        }
        if !margin.is_auto
            && let (Some(node), Some(style), Some(value)) = (containing_block, containing_block_style, value)
            && has_committed_box(self.host, node)
        {
            let size = content_size(self.host, node);
            let basis = if style.writing_mode() == writing_mode::HORIZONTAL_TB {
                size.width
            } else {
                size.height
            };
            return match value {
                StyleValueData::Percentage { value } => resolved_length(basis.to_double() * (value * 0.01)),
                value => match crate::css::calc::resolve_calculated_length_without_context(value, basis.to_double()) {
                    Some(pixels) => resolved_length(pixels),
                    None => length_value(used_value),
                },
            };
        }
        length_value(used_value)
    }

    fn resolved_value_for_padding(
        &self,
        padding: &ComputedLengthPercentageOrAuto,
        used_value: impl FnOnce(&FfiBoxModelMetrics) -> CssPixels,
    ) -> StyleValueArc {
        if self.has_used_box(self.node, self.style) {
            return length_value(used_value(&box_model(self.host, self.node)).to_double());
        }
        style_value_for_length_percentage_or_auto(padding)
    }

    fn resolved_value_for_size(
        &self,
        property: u16,
        size_in_axis: impl FnOnce(FfiCssPixelSize) -> CssPixels,
    ) -> StyleValueArc {
        if !self.query.svg_sizing_properties_apply || !self.has_used_box(self.node, self.style) {
            return self.computed(property);
        }
        let used_size = if self.style.box_sizing() == box_sizing::BORDER_BOX {
            let mut size = FfiCssPixelSize::default();
            crate::layout::script_entries::layout_script_paintable_unbroken_border_box_size(
                self.host, self.node, &mut size,
            );
            size
        } else {
            content_size(self.host, self.node)
        };
        length_value(size_in_axis(used_size).to_double())
    }

    // https://drafts.csswg.org/cssom/#resolved-value-special-case-property-like-top
    fn resolved_value_for_inset(
        &self,
        side: &ComputedLengthPercentageOrAuto,
        opposite_side: &ComputedLengthPercentageOrAuto,
        axis: InsetAxis,
        used_value: impl FnOnce(&FfiBoxModelMetrics) -> CssPixels,
    ) -> StyleValueArc {
        let computed_value = || style_value_for_length_percentage_or_auto(side);
        let position = self.principal_style.position();
        if position == positioning::STATIC || !self.has_used_box(self.principal_node, self.principal_style) {
            return computed_value();
        }

        // NB: The used value of a fixed length is the length itself. Taking it from the computed value keeps
        //     the precision that layout units would lose. Layout saturates lengths beyond the range of layout
        //     units, so those are reported as saturated.
        let value = handle_value(&side.value).filter(|_| !side.is_auto);
        if let Some(pixels) = value.and_then(absolute_length_px) {
            if pixels >= f64::from(MIN_INTEGER_VALUE) && pixels <= f64::from(MAX_INTEGER_VALUE) {
                return computed_value();
            }
            return length_value(CssPixels::nearest_value_for(pixels).to_double());
        }

        let absolutized_against = |percentage_basis: CssPixels| {
            length_value(offset_in_px(
                value.expect("an inset to absolutize is not auto"),
                percentage_basis,
            ))
        };
        let size_in_axis = |size: FfiCssPixelSize| match axis {
            InsetAxis::Horizontal => size.width,
            InsetAxis::Vertical => size.height,
        };

        // https://drafts.csswg.org/css-position/#insets
        // For sticky positioned boxes, the inset is instead relative to the relevant scrollport's size.
        if position == positioning::STICKY {
            // NB: Sticky positioning leaves an auto inset unconstrained, so its used value stays auto.
            if side.is_auto {
                return computed_value();
            }
            // NB: This uses the same scrollport size as the sticky constraints: the content box of the nearest
            //     scroll container, or of the viewport.
            let scroll_container = NodeSlotId {
                index: self.query.scroll_container,
            };
            if !self.query.has_scroll_container || !has_committed_box(self.host, scroll_container) {
                return computed_value();
            }
            return absolutized_against(size_in_axis(content_size(self.host, scroll_container)));
        }

        // https://drafts.csswg.org/css-position/#relpos-insets
        // If neither is auto, the position is over-constrained;
        // NB: The resolved value is then the computed value, absolutized against the containing block.
        // INTEROP: Percentages are absolutized rather than serialized as-is, to match other browsers.
        if position == positioning::RELATIVE && !side.is_auto && !opposite_side.is_auto {
            return match self
                .containing_block()
                .filter(|&node| has_committed_box(self.host, node))
            {
                Some(node) => absolutized_against(size_in_axis(content_size(self.host, node))),
                None => computed_value(),
            };
        }

        length_value(used_value(&box_model(self.host, self.principal_node)).to_double())
    }

    fn resolved_transform(&self) -> StyleValueArc {
        let transform_values = self.style.transform();
        if transform_values.transformations.pointer.is_null() {
            return keyword_value(keyword::NONE);
        }

        // https://drafts.csswg.org/css-transforms-2/#serialization-of-the-computed-value
        // The transform property is a resolved value special case property. [CSSOM]
        // When the computed value is a <transform-list>, the resolved value is one <matrix()> function or one
        // <matrix3d()> function computed by the following algorithm:
        // 1. Let transform be a 4x4 matrix initialized to the identity matrix.
        //    The elements m11, m22, m33 and m44 of transform must be set to 1; all other elements of transform must be
        //    set to 0.
        // 2. Post-multiply all <transform-function>s in <transform-list> to transform.
        //    NB: The resolved transforms start with those of the translate, rotate and scale properties.
        assert!(has_committed_box(self.host, self.node));
        let reference_box = transform_reference_box(self.host, self.node);
        let individual_transform_count = [
            &transform_values.translate,
            &transform_values.rotate,
            &transform_values.scale,
        ]
        .iter()
        .filter(|handle| !handle.pointer.is_null())
        .count();
        let resolved_transforms = &transform_values.resolved_transforms;
        // SAFETY: The list holds `length` transforms while the style lives.
        let entries =
            unsafe { ak::slice_from_raw_parts(resolved_transforms.pointer.cast_const(), resolved_transforms.length) };
        let transform = multiply_transform_functions(
            libgfx_rust::FloatMatrix4x4::identity(),
            entries.get(individual_transform_count..).unwrap_or_default(),
            reference_box,
        );
        let m = &transform.elements;

        // 3. Chose between <matrix()> or <matrix3d()> serialization:
        // -> If transform is a 2D matrix
        //        Serialize transform to a <matrix()> function.
        if is_2d_matrix(&transform) {
            return transformation_value(
                transform_function::MATRIX,
                &[m[0][0], m[1][0], m[0][1], m[1][1], m[0][3], m[1][3]],
            );
        }
        // -> Otherwise
        //        Serialize transform to a <matrix3d()> function.
        transformation_value(
            transform_function::MATRIX3D,
            &[
                m[0][0], m[1][0], m[2][0], m[3][0], m[0][1], m[1][1], m[2][1], m[3][1], m[0][2], m[1][2], m[2][2],
                m[3][2], m[0][3], m[1][3], m[2][3], m[3][3],
            ],
        )
    }

    fn reference_box_if_used(&self) -> Option<CssPixelRect> {
        self.has_used_box(self.node, self.style)
            .then(|| transform_reference_box(self.host, self.node))
    }

    fn resolved_animation_duration(&self) -> StyleValueArc {
        // https://drafts.csswg.org/css-animations-2/#animation-duration
        // For backwards-compatibility with Level 1, when the computed value of animation-timeline is auto (i.e. only
        // one list value, and that value being auto), the resolved value of auto for animation-duration is 0s whenever
        // its used value would also be 0s.
        let is_auto = |value: &RetainedStyleValueData| matches!(value.optional_data(), Some(StyleValueData::Keyword { keyword }) if *keyword == keyword::AUTO);
        let animation_timeline = self.computed(property_id::ANIMATION_TIMELINE);
        let animation_duration = self.computed(property_id::ANIMATION_DURATION);
        let (
            StyleValueData::ValueList { values: timelines, .. },
            StyleValueData::ValueList {
                values: durations,
                separator,
                collapsible,
            },
        ) = (&*animation_timeline, &*animation_duration)
        else {
            unreachable!("the computed animation-timeline and animation-duration are lists");
        };
        if !matches!(timelines.as_slice(), [timeline] if is_auto(timeline)) {
            return animation_duration.clone();
        }
        let resolved_durations = durations
            .as_slice()
            .iter()
            .map(|duration| {
                if !is_auto(duration) {
                    return duration.clone();
                }
                RetainedStyleValueData::from_arc(shared_style_value(StyleValueData::Time {
                    unit: TIME_UNIT_SECONDS,
                    value: 0.0,
                }))
            })
            .collect();
        StyleValueArc::new(StyleValueData::ValueList {
            values: RetainedStyleValueDataList::from_retained_values(resolved_durations),
            separator: *separator,
            collapsible: *collapsible,
        })
    }

    fn resolved_min_or_max_size(&self, property: u16, anchor_property: FfiAnchorSizedProperty) -> StyleValueArc {
        let mut size = CssPixels::from_raw(0);
        if crate::layout::script_entries::layout_script_anchor_resolved_size(
            self.host,
            self.node,
            anchor_property,
            &mut size,
        ) {
            return length_value(size.to_double());
        }
        resolve_unresolvable_anchor_functions(property, self.computed(property))
    }

    fn resolved_value(&self, property: u16) -> StyleValueArc {
        let style = self.style;
        let current_color = style.current_color();
        let current = Rgba::from_packed(current_color);
        let color_input = ColorResolutionInput::for_style(style);
        let principal_surround = self.principal_style.surround();
        let surround = style.surround();
        let inset = &principal_surround.inset;

        // A logical property maps by the element's own writing mode and direction, which its box may not show yet: a box
        // awaiting the layout tree update that replaces it keeps the style it was built for.
        let property = physical_property(
            property,
            self.element_style.writing_mode(),
            self.element_style.direction(),
        );

        // A limited number of properties have special rules for producing their "resolved value".
        // Everything else uses the computed value.
        // https://drafts.csswg.org/cssom/#resolved-values
        match property {
            property_id::BACKGROUND_COLOR => {
                let element_style = self.element_style;
                let value = handle_value(&element_style.background().background_color_style_value)
                    .expect("the background group holds a color value");
                resolve_color_style_value(
                    &retained(value),
                    Rgba::from_packed(element_style.background_color()),
                    Some(&color_input),
                )
            }
            property_id::BORDER_BOTTOM_COLOR => self.resolved_color(property, style.border_bottom_color()),
            property_id::BORDER_LEFT_COLOR => self.resolved_color(property, style.border_left_color()),
            property_id::BORDER_RIGHT_COLOR => self.resolved_color(property, style.border_right_color()),
            property_id::BORDER_TOP_COLOR => self.resolved_color(property, style.border_top_color()),
            property_id::BOX_SHADOW => {
                let shadows = &style.effects().box_shadows;
                // SAFETY: The list holds `length` shadows while the style lives.
                let shadows = unsafe { ak::slice_from_raw_parts(shadows.pointer.cast_const(), shadows.length) };
                style_value_for_shadow(SHADOW_TYPE_NORMAL, shadows, current)
            }
            property_id::CARET_COLOR => {
                self.resolved_color(property, style.inherited_ui().caret_color.color.resolve(current_color))
            }
            property_id::COLOR => resolve_color_style_value(
                &self.computed(property),
                Rgba::from_packed(self.element_style.current_color()),
                Some(&ColorResolutionInput::for_style(self.element_style)),
            ),
            property_id::FILL | property_id::FLOOD_COLOR | property_id::STOP_COLOR | property_id::STROKE => {
                resolve_svg_color_style_value(&self.computed(property), &color_input)
            }
            property_id::COLUMN_RULE_COLOR => self.resolved_color(property, style.column_rule_color()),
            property_id::OUTLINE_COLOR => self.resolved_color(property, style.outline_color()),
            property_id::TEXT_DECORATION_COLOR => self.resolved_color(property, style.text_decoration_color()),
            property_id::TEXT_SHADOW => {
                let shadows = &style.inherited_text().text_shadow;
                // SAFETY: The list holds `length` shadows while the style lives.
                let shadows = unsafe { ak::slice_from_raw_parts(shadows.pointer.cast_const(), shadows.length) };
                style_value_for_shadow(SHADOW_TYPE_TEXT, shadows, current)
            }
            property_id::BACKDROP_FILTER | property_id::FILTER => {
                resolve_filter_style_value(&self.computed(property), current)
            }
            property_id::TOUCH_ACTION => resolve_touch_action_style_value(&self.computed(property)),

            // -> line-height
            //    The resolved value is normal if the computed value is normal, or the used value otherwise.
            property_id::LINE_HEIGHT => {
                let line_height = self.computed(property);
                if matches!(&*line_height, StyleValueData::Keyword { keyword } if *keyword == keyword::NORMAL) {
                    return line_height;
                }
                length_value(self.element_style.font().line_height_used().to_double())
            }

            property_id::HEIGHT => self.resolved_value_for_size(property, |size| size.height),
            property_id::WIDTH => self.resolved_value_for_size(property, |size| size.width),
            property_id::MARGIN_BOTTOM => {
                self.resolved_value_for_margin(&principal_surround.margin.bottom, |metrics| metrics.margin.bottom)
            }
            property_id::MARGIN_LEFT => {
                self.resolved_value_for_margin(&principal_surround.margin.left, |metrics| metrics.margin.left)
            }
            property_id::MARGIN_RIGHT => {
                self.resolved_value_for_margin(&principal_surround.margin.right, |metrics| metrics.margin.right)
            }
            property_id::MARGIN_TOP => {
                self.resolved_value_for_margin(&principal_surround.margin.top, |metrics| metrics.margin.top)
            }
            property_id::PADDING_BOTTOM => {
                self.resolved_value_for_padding(&surround.padding.bottom, |metrics| metrics.padding.bottom)
            }
            property_id::PADDING_LEFT => {
                self.resolved_value_for_padding(&surround.padding.left, |metrics| metrics.padding.left)
            }
            property_id::PADDING_RIGHT => {
                self.resolved_value_for_padding(&surround.padding.right, |metrics| metrics.padding.right)
            }
            property_id::PADDING_TOP => {
                self.resolved_value_for_padding(&surround.padding.top, |metrics| metrics.padding.top)
            }

            // NB: The computed value of a min or max sizing property resolves its anchor-size() functions, which layout
            //     resolves against an anchor box.
            property_id::MAX_HEIGHT => self.resolved_min_or_max_size(property, FfiAnchorSizedProperty::MaxHeight),
            property_id::MAX_WIDTH => self.resolved_min_or_max_size(property, FfiAnchorSizedProperty::MaxWidth),
            property_id::MIN_HEIGHT => self.resolved_min_or_max_size(property, FfiAnchorSizedProperty::MinHeight),
            property_id::MIN_WIDTH => self.resolved_min_or_max_size(property, FfiAnchorSizedProperty::MinWidth),

            property_id::BOTTOM => {
                self.resolved_value_for_inset(&inset.bottom, &inset.top, InsetAxis::Vertical, |metrics| {
                    metrics.inset.bottom
                })
            }
            property_id::LEFT => {
                self.resolved_value_for_inset(&inset.left, &inset.right, InsetAxis::Horizontal, |metrics| {
                    metrics.inset.left
                })
            }
            property_id::RIGHT => {
                self.resolved_value_for_inset(&inset.right, &inset.left, InsetAxis::Horizontal, |metrics| {
                    metrics.inset.right
                })
            }
            property_id::TOP => {
                self.resolved_value_for_inset(&inset.top, &inset.bottom, InsetAxis::Vertical, |metrics| {
                    metrics.inset.top
                })
            }

            // -> A resolved value special case property defined in another specification
            //    As defined in the relevant specification.
            property_id::TRANSFORM => self.resolved_transform(),
            property_id::TRANSFORM_ORIGIN => {
                // https://drafts.csswg.org/css-transforms/#transform-origin-property
                // The transform-origin property is a resolved value special case property like height. [CSSOM]
                let values = style.transform();
                style_value_for_transform_origin(
                    handle_value(&values.transform_origin_x).expect("transform-origin has an x offset"),
                    handle_value(&values.transform_origin_y).expect("transform-origin has a y offset"),
                    handle_value(&values.transform_origin_z),
                    self.reference_box_if_used(),
                )
            }
            property_id::PERSPECTIVE_ORIGIN => {
                // https://drafts.csswg.org/css-transforms-2/#perspective-origin-property
                // The perspective-origin property is a resolved value special case property like height. [CSSOM]
                let Some(reference_box) = self.reference_box_if_used() else {
                    return self.computed(property);
                };
                let values = style.transform();
                style_value_for_transform_origin(
                    handle_value(&values.perspective_origin_x).expect("perspective-origin has an x offset"),
                    handle_value(&values.perspective_origin_y).expect("perspective-origin has a y offset"),
                    None,
                    Some(reference_box),
                )
            }
            property_id::ANIMATION_DURATION => self.resolved_animation_duration(),

            // If the border-style corresponding to a given border-width is none or hidden, then the used width is 0.
            // https://drafts.csswg.org/css-backgrounds/#border-width
            // NB: We do this adjustment when assigning to ComputedValues, so read from there.
            property_id::BORDER_BOTTOM_WIDTH => length_value(style.border_bottom_width().to_double()),
            property_id::BORDER_LEFT_WIDTH => length_value(style.border_left_width().to_double()),
            property_id::BORDER_RIGHT_WIDTH => length_value(style.border_right_width().to_double()),
            property_id::BORDER_TOP_WIDTH => length_value(style.border_top_width().to_double()),

            // -> Any other property
            //    The resolved value is the computed value.
            property_id::_WEBKIT_TEXT_FILL_COLOR => {
                self.resolved_color(property, style.inherited_text().webkit_text_fill_color)
            }
            property_id::LETTER_SPACING => {
                // https://drafts.csswg.org/css-text-4/#letter-spacing-property
                // For legacy reasons, a computed letter-spacing of zero yields a resolved value (getComputedStyle()
                // return value) of normal.
                if style.letter_spacing().raw_value() == 0 {
                    return keyword_value(keyword::NORMAL);
                }
                self.computed(property)
            }
            _ => {
                // For grid-template-columns and grid-template-rows the resolved value is the used value.
                // https://www.w3.org/TR/css-grid-2/#resolved-track-list-standalone
                if matches!(
                    property,
                    property_id::GRID_TEMPLATE_COLUMNS | property_id::GRID_TEMPLATE_ROWS
                ) && has_committed_box(self.host, self.node)
                {
                    let tracks = crate::painting::ffi::render_state_paintable_used_grid_tracks(
                        self.host,
                        self.node,
                        property == property_id::GRID_TEMPLATE_COLUMNS,
                    );
                    if !tracks.is_null() {
                        // SAFETY: The call hands over one strong reference.
                        return unsafe { StyleValueArc::from_raw(tracks.cast()) };
                    }
                }

                self.computed(property)
            }
        }
    }
}

/// The resolved value of `property` for the element of a box, as one strong reference.
///
/// # Safety
/// The query must name a live host, nodes of its layout tree and a live style.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_resolved_value_of_box(
    query: &FfiResolvedValueBox,
    property: u16,
) -> *const StyleValueData {
    // SAFETY: Guaranteed by the caller.
    let (host, element_style) = unsafe {
        (
            &*query.host.cast::<DocumentHost>(),
            ComputedValuesView::of_record_groups(&*query.element_style.cast::<StyleRecordGroups>()),
        )
    };
    let node = NodeSlotId { index: query.node };
    let principal_node = NodeSlotId {
        index: query.principal_node,
    };
    let resolved_box = ResolvedValueBox {
        query,
        host,
        node,
        principal_node,
        style: node_style(host, node),
        principal_style: node_style(host, principal_node),
        element_style,
    };
    StyleValueArc::into_raw(resolved_box.resolved_value(property))
}

/// What the host names for a style with no box behind it.
#[repr(C)]
pub struct FfiResolvedValueStyle {
    /// The payload of each style group, by group index.
    pub groups: *const *const c_void,
    pub writing_mode: u8,
    pub direction: u8,
    pub generates_no_box: bool,
    pub computed_value_context: *mut c_void,
    /// The computed value of a longhand of the style, as one strong reference.
    pub computed_value: unsafe extern "C" fn(context: *mut c_void, property: u16) -> *const StyleValueData,
}

/// The resolved value of the longhand `property` of a style with no box behind it, as one strong reference.
/// Resolved value special cases that need a box read the style's own values instead.
///
/// # Safety
/// The query must name the live payload of every group, and a context its callback accepts.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_resolved_value_of_style(
    query: &FfiResolvedValueStyle,
    property: u16,
) -> *const StyleValueData {
    /// The group at `index`, whose type the caller names.
    ///
    /// # Safety
    /// `T` must be the type of the group at `index`, and the payload must outlive the borrow of `query`.
    unsafe fn group<T>(query: &FfiResolvedValueStyle, index: usize) -> &T {
        // SAFETY: Guaranteed by the caller.
        unsafe { &*(*query.groups.add(index)).cast::<T>() }
    }
    // SAFETY: Each type is the one of its group, and the caller keeps the payloads live.
    let (text, ui, background, border, misc_layout, text_reset, effects, transform) = unsafe {
        (
            group::<InheritedTextValues>(query, group_index::INHERITED_TEXT),
            group::<InheritedUIValues>(query, group_index::INHERITED_UI),
            group::<BackgroundValues>(query, group_index::BACKGROUND),
            group::<BorderValues>(query, group_index::BORDER),
            group::<MiscLayoutValues>(query, group_index::MISC_LAYOUT),
            group::<TextResetValues>(query, group_index::TEXT_RESET),
            group::<EffectsValues>(query, group_index::EFFECTS),
            group::<TransformValues>(query, group_index::TRANSFORM),
        )
    };
    let current_color = text.color;
    let current = Rgba::from_packed(current_color);
    let color_input = ColorResolutionInput::for_style_groups(Some(ui), Some(text));
    let property = physical_property(property, query.writing_mode, query.direction);
    // SAFETY: The caller passes a context its callback accepts, and the call hands over one strong reference.
    let computed =
        || unsafe { StyleValueArc::from_raw((query.computed_value)(query.computed_value_context, property)) };
    let used_color = |color: u32| resolve_color_style_value(&computed(), Rgba::from_packed(color), None);

    // NB: These are the resolved value special cases of a box, read from the style's own groups.
    let resolved = match property {
        property_id::COLOR => resolve_color_style_value(&computed(), current, Some(&color_input)),
        property_id::FILL | property_id::FLOOD_COLOR | property_id::STOP_COLOR | property_id::STROKE => {
            resolve_svg_color_style_value(&computed(), &color_input)
        }
        property_id::CARET_COLOR => used_color(ui.caret_color.color.resolve(current_color)),
        property_id::BACKGROUND_COLOR => match handle_value(&background.background_color_style_value) {
            Some(value) if !value_depends_on_current_color(value) => retained(value),
            _ => used_color(background.background_color.resolve(current_color)),
        },
        property_id::BORDER_BOTTOM_COLOR => used_color(border.border_bottom.color.resolve(current_color)),
        property_id::BORDER_LEFT_COLOR => used_color(border.border_left.color.resolve(current_color)),
        property_id::BORDER_RIGHT_COLOR => used_color(border.border_right.color.resolve(current_color)),
        property_id::BORDER_TOP_COLOR => used_color(border.border_top.color.resolve(current_color)),
        property_id::COLUMN_RULE_COLOR => used_color(misc_layout.column_rule_color.resolve(current_color)),
        property_id::OUTLINE_COLOR => used_color(misc_layout.outline_color.resolve(current_color)),
        property_id::TEXT_DECORATION_COLOR => used_color(text_reset.text_decoration_color.resolve(current_color)),
        property_id::_WEBKIT_TEXT_FILL_COLOR => used_color(text.webkit_text_fill_color),
        property_id::BOX_SHADOW => {
            // SAFETY: The list holds `length` shadows while the style lives.
            let shadows = unsafe {
                ak::slice_from_raw_parts(effects.box_shadows.pointer.cast_const(), effects.box_shadows.length)
            };
            style_value_for_shadow(SHADOW_TYPE_NORMAL, shadows, current)
        }
        property_id::TEXT_SHADOW => {
            // SAFETY: The list holds `length` shadows while the style lives.
            let shadows =
                unsafe { ak::slice_from_raw_parts(text.text_shadow.pointer.cast_const(), text.text_shadow.length) };
            style_value_for_shadow(SHADOW_TYPE_TEXT, shadows, current)
        }
        property_id::BACKDROP_FILTER | property_id::FILTER => resolve_filter_style_value(&computed(), current),
        property_id::TOUCH_ACTION => resolve_touch_action_style_value(&computed()),
        property_id::TRANSFORM_ORIGIN => style_value_for_transform_origin(
            handle_value(&transform.transform_origin_x).expect("transform-origin has an x offset"),
            handle_value(&transform.transform_origin_y).expect("transform-origin has a y offset"),
            handle_value(&transform.transform_origin_z),
            None,
        ),
        property_id::MIN_HEIGHT | property_id::MIN_WIDTH => {
            // https://drafts.csswg.org/css-sizing-3/#valdef-width-auto
            // For backwards-compatibility, the resolved value of this keyword is zero for boxes of all [CSS2]
            // display types (block and inline boxes, inline blocks, and all table display types) when
            // 'aspect-ratio' is 'auto'. It also resolves to zero when no box is generated.
            let value = computed();
            if query.generates_no_box
                && matches!(&*value, StyleValueData::Keyword { keyword } if *keyword == keyword::AUTO)
            {
                length_value(0.0)
            } else {
                resolve_unresolvable_anchor_functions(property, value)
            }
        }
        property_id::MAX_HEIGHT | property_id::MAX_WIDTH => resolve_unresolvable_anchor_functions(property, computed()),
        _ => computed(),
    };
    StyleValueArc::into_raw(resolved)
}
