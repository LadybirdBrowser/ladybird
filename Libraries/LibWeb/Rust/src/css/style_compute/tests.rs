/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use crate::css::style_value::StyleValueBox;

#[test]
fn all_transition_longhands_match_the_recursive_expansion_of_all() {
    use crate::css::property_metadata::{longhand_is_logical_alias, property_id as prop};
    fn expand(properties: &mut Vec<u16>, property: u16, writing_mode: u8, direction: u8) {
        if property_is_shorthand(property) {
            for &longhand in longhands_for_shorthand(property) {
                expand(properties, longhand, writing_mode, direction);
            }
        } else if property != prop::CUSTOM {
            properties.push(match longhand_is_logical_alias(property) {
                true => map_logical_alias_to_physical(property, writing_mode, direction),
                false => property,
            });
        }
    }
    for writing_mode in 0..WRITING_MODE_COUNT as u8 {
        for direction in 0..DIRECTION_COUNT as u8 {
            let mut expected = Vec::new();
            expand(&mut expected, prop::ALL, writing_mode, direction);
            assert_eq!(
                crate::css::computed_values::all_transition_longhands(writing_mode, direction),
                &expected[..]
            );
        }
    }
}

#[test]
fn url_dependencies_only_read_a_base_for_relative_urls() {
    for (url, needs_base) in [
        ("https://example.com/paint.svg#fill", false),
        ("data:image/svg+xml,paint", false),
        ("https://example.com/色.svg#fill", false),
        ("paint.svg#fill", true),
        ("色.svg#fill", true),
        ("/paint.svg#fill", true),
        ("//example.com/paint.svg#fill", true),
        ("#fill", false),
        ("", false),
    ] {
        let value = StyleValueData::Url(StyleValueBox::new(UrlPayload {
            url: crate::css::style_value::RetainedString::from_utf16(&url.encode_utf16().collect::<Vec<_>>()).unwrap(),
            url_type: 0,
            modifiers: crate::css::style_value::RetainedRequestUrlModifierList::from_retained_modifiers(Vec::new()),
        }));
        let dependencies = collect_external_value_dependencies(&value);
        assert_eq!(dependencies.needs_document_base_url, needs_base, "{url}");
        assert_eq!(dependencies.may_need_style_sheet_resource_context, needs_base, "{url}");
    }
}

#[test]
fn calculated_random_values_record_their_document_dependency() {
    let number = || {
        std::sync::Arc::new(crate::css::calc::CalcNode::Numeric(
            crate::css::calc::CalcNumericValue::Number {
                value: 1.0,
                number_type: 0,
            },
        ))
    };
    let calculation = crate::css::calc::CalcNode::Random {
        min: number(),
        max: number(),
        step: None,
        sharing: RetainedStyleValueData::from_owned(StyleValueData::Keyword { keyword: keyword::AUTO }),
    };
    let value = StyleValueData::Calculated(StyleValueBox::new(CalculatedPayload {
        rust_calculation: crate::css::calc::CalcNodeHandle::from_arc(std::sync::Arc::new(calculation)),
        resolve_as_is_number: true,
        resolve_as_base: 0,
        resolved_type: crate::css::calc::FfiNumericType::from_calc(None),
        has_percentages_resolve_as: false,
        percentages_resolve_as: 0,
        resolve_numbers_as_integers: false,
        accepted_ranges: crate::css::style_value::RetainedNumericRangeList::empty(),
    }));

    assert!(external_value_dependencies(&value).uses_random_function);
}

#[test]
fn effective_overflow_keywords_compute_together() {
    let result = resolve_effective_overflow_keywords(keyword::VISIBLE, keyword::AUTO);
    assert!(result.changed_x);
    assert_eq!(result.x_keyword, keyword::AUTO);
    assert!(!result.changed_y);
    assert_eq!(result.y_keyword, keyword::AUTO);

    let result = resolve_effective_overflow_keywords(keyword::CLIP, keyword::SCROLL);
    assert!(result.changed_x);
    assert_eq!(result.x_keyword, keyword::HIDDEN);
    assert!(!result.changed_y);
    assert_eq!(result.y_keyword, keyword::SCROLL);

    let result = resolve_effective_overflow_keywords(keyword::VISIBLE, keyword::CLIP);
    assert!(!result.changed_x);
    assert!(!result.changed_y);
}

#[test]
fn effective_color_scheme_follows_element_and_document_preferences() {
    const AUTO: u8 = 0;
    const DARK: u8 = 1;
    const LIGHT: u8 = 2;

    assert_eq!(resolve_effective_color_scheme(&[LIGHT, DARK], DARK, None), DARK);
    assert_eq!(resolve_effective_color_scheme(&[DARK, LIGHT], LIGHT, None), LIGHT);
    assert_eq!(resolve_effective_color_scheme(&[DARK, LIGHT], AUTO, None), DARK);
    assert_eq!(
        resolve_effective_color_scheme(&[AUTO], DARK, Some(&[LIGHT, DARK])),
        DARK
    );
    assert_eq!(resolve_effective_color_scheme(&[], AUTO, Some(&[DARK])), DARK);
    assert_eq!(resolve_effective_color_scheme(&[], AUTO, None), LIGHT);
}

#[test]
fn color_keyword_classification_uses_rust_color_metadata() {
    assert!(keyword_is_color(keyword::CURRENTCOLOR));
    assert!(keyword_is_color(keyword::CANVAS));
    assert!(keyword_is_color(keyword::_LIBWEB_BUTTONFACEHOVER));
    assert!(!keyword_is_color(keyword::AUTO));
}

#[test]
fn text_align_adjusts_match_parent_and_table_header_defaults() {
    let result = compute_text_align_adjustment(keyword::MATCH_PARENT, false, true, keyword::START, false);
    assert!(result.changed);
    assert_eq!(result.keyword, keyword::RIGHT);
    assert!(!result.inherited);

    let result = compute_text_align_adjustment(keyword::_LIBWEB_INHERIT_OR_CENTER, true, true, keyword::JUSTIFY, true);
    assert!(result.changed);
    assert_eq!(result.keyword, keyword::JUSTIFY);
    assert!(result.inherited);

    let result = compute_text_align_adjustment(keyword::_LIBWEB_INHERIT_OR_CENTER, true, false, 0, true);
    assert!(result.changed);
    assert_eq!(result.keyword, keyword::CENTER);
    assert!(!result.inherited);
}

fn element_adjustment_input() -> FfiBoxTypeTransformationInput {
    FfiBoxTypeTransformationInput {
        display: FfiDisplay::inline(),
        position: keyword::STATIC,
        float_value: keyword::NONE,
        is_br_element: false,
        is_document_element: false,
        is_mathml_element: false,
        is_mathml_mtable: false,
        is_mathml_mtr: false,
        is_mathml_mtd: false,
        has_parent_display: false,
        writing_mode: crate::css::css_enums::writing_mode::HORIZONTAL_TB,
        parent_writing_mode: crate::css::css_enums::writing_mode::HORIZONTAL_TB,
        is_svg_element: false,
        parent_display: FfiDisplay::block(),
        is_wbr_element: false,
        disallow_display_contents: false,
        rewrite_inline_flow: false,
        is_button_element: false,
        force_line_height_normal: false,
        check_input_line_height: false,
        hide_audio_without_controls: false,
        is_table_element: false,
        force_position_static: false,
        force_symbol_display_inline: false,
        webkit_box_layout_transformation_applies: false,
    }
}

#[test]
fn element_styles_adjust_from_marshaled_facts() {
    let mut input = element_adjustment_input();
    input.disallow_display_contents = true;
    let adjustment = adjust_element_style(&input, FfiDisplay::contents(), keyword::LEFT);
    assert!(adjustment.changed_display);
    assert!(adjustment.display.is_none());

    input = element_adjustment_input();
    input.is_button_element = true;
    let adjustment = adjust_element_style(&input, FfiDisplay::inline(), keyword::LEFT);
    assert!(adjustment.changed_display);
    assert!(adjustment.display.is_inline_block());

    input = element_adjustment_input();
    input.is_table_element = true;
    let adjustment = adjust_element_style(&input, FfiDisplay::block(), keyword::_LIBWEB_CENTER);
    assert!(adjustment.changed_text_align);
    assert_eq!(adjustment.text_align, keyword::START);

    input = element_adjustment_input();
    input.check_input_line_height = true;
    let adjustment = adjust_element_style(&input, FfiDisplay::inline(), keyword::LEFT);
    assert!(should_clamp_input_line_height(
        &adjustment,
        &FfiInputLineHeightMetrics {
            current_line_height: 12.0,
            minimum_line_height: 16.0,
        }
    ));
    assert!(!should_clamp_input_line_height(
        &adjustment,
        &FfiInputLineHeightMetrics {
            current_line_height: 18.0,
            minimum_line_height: 16.0,
        }
    ));
}

#[test]
fn element_style_adjustments_follow_box_type_transformation() {
    let mut input = element_adjustment_input();
    input.is_button_element = true;
    input.position = keyword::ABSOLUTE;

    let adjustments = compute_element_style_adjustments(&input, keyword::LEFT);
    assert!(adjustments.box_type.changed_display);
    assert!(adjustments.box_type.display.is_block_outside());
    assert!(adjustments.element_style.changed_display);
    assert!(adjustments.element_style.display.is_flow_root_inside());
}

fn test_context() -> FfiLengthResolutionContext {
    FfiLengthResolutionContext {
        viewport_width: 800.0,
        viewport_height: 600.0,
        font_metrics: FfiFontMetrics {
            font_size: 16.0,
            x_height: 8.0,
            cap_height: 11.0,
            zero_advance: 7.5,
            line_height: 19.0,
        },
        root_font_metrics: FfiFontMetrics {
            font_size: 20.0,
            x_height: 10.0,
            cap_height: 14.0,
            zero_advance: 9.0,
            line_height: 24.0,
        },
        font_metrics_depend_on_viewport_metrics: false,
        root_font_metrics_depend_on_viewport_metrics: true,
        has_container_width_basis: false,
        has_container_height_basis: false,
        container_width_basis: 0.0,
        container_height_basis: 0.0,
        container_width_basis_depends_on_viewport_metrics: false,
        container_height_basis_depends_on_viewport_metrics: false,
        subject_inline_axis_is_horizontal: true,
        resolved_viewport_relative_length: std::ptr::null_mut(),
        zoom: 1.0,
    }
}

fn unit_code(name: &str) -> usize {
    LENGTH_UNIT_NAMES.iter().position(|&n| n == name).unwrap()
}

#[test]
fn px_is_unchanged() {
    let result = absolutize_length(4.0, unit_code("px"), &test_context());
    assert!(result.handled);
    assert!(!result.changed);
}

#[test]
fn font_relative_units_resolve() {
    let context = test_context();
    let em = absolutize_length(2.0, unit_code("em"), &context);
    assert!(em.changed);
    assert_eq!(em.px, 32.0);
    assert!(!em.resolved_viewport_relative_length);

    let rem = absolutize_length(2.0, unit_code("rem"), &context);
    assert_eq!(rem.px, 40.0);
    assert!(rem.resolved_viewport_relative_length);

    assert_eq!(absolutize_length(1.0, unit_code("lh"), &context).px, 19.0);
    assert_eq!(absolutize_length(1.0, unit_code("rch"), &context).px, 9.0);
}

#[test]
fn font_size_recascade_resolves_em_without_a_host_context() {
    let percentage = StyleValueData::Percentage { value: 200.0 };
    let em = StyleValueData::Length {
        value: 2.0,
        unit: unit_code("em") as u8,
    };
    let final_percentage = StyleValueData::Percentage { value: 50.0 };
    let values = [None, Some(&percentage), Some(&em), Some(&final_percentage)];
    let default_size = CssPixels::from_integer(13);
    let batch = recascade_font_size_batch(
        values.len(),
        |index| values[index],
        |_| 1.0,
        |_| true,
        0,
        default_size.raw_value(),
        false,
        default_size.raw_value(),
        FontSizeRecascadeDocumentInputs {
            root_font_size: 16.0,
            root_effective_zoom: 1.0,
            root_font_metrics_depend_on_viewport_metrics: false,
            viewport_width: 800.0,
            viewport_height: 600.0,
        },
        None,
    );
    assert!(batch.status == FontSizeRecascadeStatus::Complete);
    assert_eq!(batch.next_index, values.len());
    assert_eq!(batch.current_size_raw, CssPixels::from_integer(26).raw_value());
}

#[test]
fn font_size_recascade_resolves_document_lengths() {
    let rem = StyleValueData::Length {
        value: 2.0,
        unit: unit_code("rem") as u8,
    };
    let vw = StyleValueData::Length {
        value: 10.0,
        unit: unit_code("vw") as u8,
    };
    let values = [Some(&rem), Some(&vw)];
    let batch = recascade_font_size_batch(
        values.len(),
        |index| values[index],
        |_| 1.0,
        |_| true,
        0,
        CssPixels::from_integer(13).raw_value(),
        false,
        CssPixels::from_integer(13).raw_value(),
        FontSizeRecascadeDocumentInputs {
            root_font_size: 20.0,
            root_effective_zoom: 1.0,
            root_font_metrics_depend_on_viewport_metrics: false,
            viewport_width: 800.0,
            viewport_height: 600.0,
        },
        None,
    );
    assert!(batch.status == FontSizeRecascadeStatus::Complete);
    assert_eq!(batch.current_size_raw, CssPixels::from_integer(80).raw_value());
    assert!(batch.depends_on_viewport_metrics);
}

#[test]
fn font_size_recascade_applies_each_ancestor_zoom_once() {
    let px = StyleValueData::Length {
        value: 10.0,
        unit: unit_code("px") as u8,
    };
    let percentage = StyleValueData::Percentage { value: 200.0 };
    let em = StyleValueData::Length {
        value: 2.0,
        unit: unit_code("em") as u8,
    };
    let values = [Some(&px), None, Some(&percentage), Some(&em)];
    let zooms = [2.0, 6.0, 3.0, 1.5];
    let batch = recascade_font_size_batch(
        values.len(),
        |index| values[index],
        |index| zooms[index],
        |_| true,
        0,
        CssPixels::from_integer(13).raw_value(),
        false,
        CssPixels::from_integer(13).raw_value(),
        FontSizeRecascadeDocumentInputs {
            root_font_size: 20.0,
            root_effective_zoom: 2.0,
            root_font_metrics_depend_on_viewport_metrics: false,
            viewport_width: 800.0,
            viewport_height: 600.0,
        },
        None,
    );
    assert!(batch.status == FontSizeRecascadeStatus::Complete);
    assert_eq!(batch.current_size_raw, CssPixels::from_integer(60).raw_value());
}

#[test]
fn font_size_recascade_stops_for_a_length_that_needs_font_metrics() {
    let ex = StyleValueData::Length {
        value: 2.0,
        unit: unit_code("ex") as u8,
    };
    let values = [Some(&ex)];
    let batch = recascade_font_size_batch(
        values.len(),
        |index| values[index],
        |_| 1.0,
        |_| true,
        0,
        CssPixels::from_integer(13).raw_value(),
        false,
        CssPixels::from_integer(13).raw_value(),
        FontSizeRecascadeDocumentInputs {
            root_font_size: 16.0,
            root_effective_zoom: 1.0,
            root_font_metrics_depend_on_viewport_metrics: false,
            viewport_width: 800.0,
            viewport_height: 600.0,
        },
        None,
    );
    assert!(batch.status == FontSizeRecascadeStatus::NeedsLengthResolution);
    assert_eq!(batch.next_index, 0);
}

#[test]
fn viewport_relative_units_resolve() {
    let context = test_context();
    let vw = absolutize_length(50.0, unit_code("vw"), &context);
    assert_eq!(vw.px, 400.0);
    assert!(vw.resolved_viewport_relative_length);
    assert_eq!(absolutize_length(50.0, unit_code("vmin"), &context).px, 300.0);
    assert_eq!(absolutize_length(50.0, unit_code("dvmax"), &context).px, 400.0);
}

#[test]
fn absolute_units_resolve() {
    let context = test_context();
    let inch = absolutize_length(1.0, unit_code("in"), &context);
    assert!(inch.changed);
    assert_eq!(inch.px, 96.0);
}

fn computed(value: f64) -> Option<ComputedNumber> {
    Some(ComputedNumber::Computed(value))
}

#[test]
fn font_weight_keywords_compute() {
    let bolder = StyleValueData::Keyword {
        keyword: keyword::BOLDER,
    };
    assert_eq!(compute_font_weight(&bolder, 300.0), computed(400.0));
    assert_eq!(compute_font_weight(&bolder, 400.0), computed(700.0));
    assert_eq!(compute_font_weight(&bolder, 700.0), computed(900.0));
    assert_eq!(compute_font_weight(&bolder, 900.0), computed(900.0));

    let lighter = StyleValueData::Keyword {
        keyword: keyword::LIGHTER,
    };
    assert_eq!(compute_font_weight(&lighter, 50.0), computed(50.0));
    assert_eq!(compute_font_weight(&lighter, 400.0), computed(100.0));
    assert_eq!(compute_font_weight(&lighter, 700.0), computed(400.0));
    assert_eq!(compute_font_weight(&lighter, 900.0), computed(700.0));

    assert_eq!(
        compute_font_weight(
            &StyleValueData::Keyword {
                keyword: keyword::NORMAL
            },
            700.0
        ),
        computed(400.0)
    );
    assert_eq!(
        compute_font_weight(&StyleValueData::Keyword { keyword: keyword::BOLD }, 100.0),
        computed(700.0)
    );
    assert_eq!(
        compute_font_weight(&StyleValueData::Number { value: 512.0 }, 100.0),
        Some(ComputedNumber::Unchanged)
    );
}

#[test]
fn font_width_keywords_compute() {
    assert_eq!(
        compute_font_width(&StyleValueData::Keyword {
            keyword: keyword::ULTRA_CONDENSED
        }),
        computed(50.0)
    );
    assert_eq!(
        compute_font_width(&StyleValueData::Keyword {
            keyword: keyword::SEMI_EXPANDED
        }),
        computed(112.5)
    );
    assert_eq!(
        compute_font_width(&StyleValueData::Keyword {
            keyword: keyword::NORMAL
        }),
        computed(100.0)
    );
    assert_eq!(
        compute_font_width(&StyleValueData::Percentage { value: 80.0 }),
        Some(ComputedNumber::Unchanged)
    );
}

#[test]
fn font_size_keywords_compute() {
    let sixteen = CssPixels::from_integer(16);
    let medium = compute_font_size(
        &StyleValueData::Keyword {
            keyword: keyword::MEDIUM,
        },
        0,
        sixteen,
        0,
        sixteen,
        1.0,
    );
    assert_eq!(medium, computed(16.0));
    let large = compute_font_size(
        &StyleValueData::Keyword {
            keyword: keyword::LARGE,
        },
        0,
        sixteen,
        0,
        sixteen,
        1.0,
    );
    assert_eq!(large, computed(18.0));
    let percent = compute_font_size(
        &StyleValueData::Percentage { value: 150.0 },
        0,
        sixteen,
        0,
        sixteen,
        1.0,
    );
    assert_eq!(percent, computed(24.0));
    let math = compute_font_size(
        &StyleValueData::Keyword { keyword: keyword::MATH },
        1,
        sixteen,
        0,
        sixteen,
        1.0,
    );
    assert_eq!(math, computed(CssPixels::from_integer(16).scaled(0.71).to_double()));
    let math_two_levels = compute_font_size(
        &StyleValueData::Keyword { keyword: keyword::MATH },
        2,
        sixteen,
        0,
        sixteen,
        1.0,
    );
    assert_eq!(
        math_two_levels,
        computed(CssPixels::from_integer(16).scaled(0.71f64.powi(2)).to_double())
    );
}

#[test]
fn math_depth_computes() {
    let auto_add = StyleValueData::Keyword {
        keyword: keyword::AUTO_ADD,
    };
    assert_eq!(compute_math_depth(&auto_add, 2, true), 3);
    assert_eq!(compute_math_depth(&auto_add, 2, false), 2);
    assert_eq!(compute_math_depth(&StyleValueData::Integer { value: 5 }, 2, true), 5);
    assert_eq!(
        compute_math_depth(&StyleValueData::Keyword { keyword: keyword::AUTO }, 4, true),
        4
    );
}

#[test]
fn line_height_computes() {
    let sixteen = CssPixels::from_integer(16);
    assert_eq!(
        compute_line_height(
            &StyleValueData::Keyword {
                keyword: keyword::NORMAL
            },
            sixteen
        ),
        ComputedLineHeight::Unchanged
    );
    assert_eq!(
        compute_line_height(&StyleValueData::Number { value: 1.5 }, sixteen),
        ComputedLineHeight::Unchanged
    );
    assert_eq!(
        compute_line_height(&StyleValueData::Percentage { value: 150.0 }, sixteen),
        ComputedLineHeight::Px(24.0)
    );
}

#[test]
fn border_width_snaps() {
    // 2.5px at 2 dppx is an integer number of device pixels: unchanged.
    let length = StyleValueData::Length {
        value: 2.5,
        unit: unit_code("px") as u8,
    };
    assert_eq!(compute_border_or_outline_width(&length, 2.0, 1.0, None), 2.5);
    // 0.4px at 1 dppx rounds up to 1 device pixel.
    let thin = StyleValueData::Length {
        value: 0.4,
        unit: unit_code("px") as u8,
    };
    assert_eq!(compute_border_or_outline_width(&thin, 1.0, 1.0, None), 1.0);
    // medium is 3px.
    assert_eq!(
        compute_border_or_outline_width(
            &StyleValueData::Keyword {
                keyword: keyword::MEDIUM
            },
            1.0,
            1.0,
            None
        ),
        3.0
    );
}

#[test]
fn corner_shapes_map_to_superellipse_parameters() {
    assert_eq!(
        compute_corner_shape_parameter(&StyleValueData::Keyword {
            keyword: keyword::ROUND
        }),
        Some(1.0)
    );
    assert_eq!(
        compute_corner_shape_parameter(&StyleValueData::Keyword {
            keyword: keyword::NOTCH
        }),
        Some(f64::NEG_INFINITY)
    );
}

#[test]
fn highlight_longhand_inheritance() {
    use crate::css::property_metadata::property_id;
    let selection = crate::css::selector::PseudoElementType::Selection as u8;
    let inherit = StyleValueData::Keyword {
        keyword: keyword::INHERIT,
    };
    let unset = StyleValueData::Keyword {
        keyword: keyword::UNSET,
    };
    let initial = StyleValueData::Keyword {
        keyword: keyword::INITIAL,
    };
    let currentcolor = StyleValueData::Keyword {
        keyword: keyword::CURRENTCOLOR,
    };

    // A supported property without a cascaded value, or with inherit or unset, takes the parent
    // highlight's value whether or not it is an inherited property.
    assert!(highlight_longhand_inherits(
        None,
        selection,
        property_id::BACKGROUND_COLOR
    ));
    assert!(highlight_longhand_inherits(
        Some(&inherit),
        selection,
        property_id::BACKGROUND_COLOR
    ));
    assert!(highlight_longhand_inherits(
        Some(&unset),
        selection,
        property_id::BACKGROUND_COLOR
    ));
    assert!(highlight_longhand_inherits(None, selection, property_id::COLOR));

    // Any other cascaded value stands, currentcolor included.
    assert!(!highlight_longhand_inherits(
        Some(&initial),
        selection,
        property_id::BACKGROUND_COLOR
    ));
    assert!(!highlight_longhand_inherits(
        Some(&currentcolor),
        selection,
        property_id::COLOR
    ));

    // A property the highlight cannot carry keeps the ordinary decision.
    assert!(!highlight_longhand_inherits(None, selection, property_id::DISPLAY));
}

#[test]
fn longhand_decisions() {
    use crate::css::property_metadata::{FIRST_INHERITED_PROPERTY_ID, LAST_LONGHAND_PROPERTY_ID};
    let inherited_id = FIRST_INHERITED_PROPERTY_ID;
    let reset_id = LAST_LONGHAND_PROPERTY_ID;

    // Missing value: inherited properties inherit, reset properties go initial.
    let missing_inherited = longhand_decision(None, inherited_id);
    assert!(missing_inherited.should_inherit);
    let missing_reset = longhand_decision(None, reset_id);
    assert!(!missing_reset.should_inherit);
    assert!(missing_reset.use_initial_without_inherit);

    // Explicit inherit on a reset property inherits.
    let inherit = StyleValueData::Keyword {
        keyword: keyword::INHERIT,
    };
    let explicit = longhand_decision(Some(&inherit), reset_id);
    assert!(explicit.should_inherit);

    // unset: inherit for inherited properties, initial for reset ones.
    let unset = StyleValueData::Keyword {
        keyword: keyword::UNSET,
    };
    assert!(longhand_decision(Some(&unset), inherited_id).should_inherit);
    let unset_reset = longhand_decision(Some(&unset), reset_id);
    assert!(!unset_reset.should_inherit);
    assert!(unset_reset.use_initial_without_inherit);

    // currentcolor in the color property inherits.
    let currentcolor = StyleValueData::Keyword {
        keyword: keyword::CURRENTCOLOR,
    };
    assert!(longhand_decision(Some(&currentcolor), crate::css::property_metadata::property_id::COLOR).should_inherit);
    assert!(!longhand_decision(Some(&currentcolor), reset_id).should_inherit);
}

#[test]
fn container_relative_units_without_snapshotted_bases_are_unhandled() {
    assert!(!absolutize_length(1.0, unit_code("cqw"), &test_context()).handled);
}

#[test]
fn container_relative_units_resolve_from_snapshotted_bases() {
    let mut context = test_context();
    context.has_container_width_basis = true;
    context.has_container_height_basis = true;
    context.container_width_basis = 400.0;
    context.container_height_basis = 200.0;

    assert_eq!(absolutize_length(10.0, unit_code("cqw"), &context).px, 40.0);
    assert_eq!(absolutize_length(10.0, unit_code("cqh"), &context).px, 20.0);
    assert_eq!(absolutize_length(10.0, unit_code("cqi"), &context).px, 40.0);
    assert_eq!(absolutize_length(10.0, unit_code("cqb"), &context).px, 20.0);
    assert_eq!(absolutize_length(10.0, unit_code("cqmin"), &context).px, 20.0);
    assert_eq!(absolutize_length(10.0, unit_code("cqmax"), &context).px, 40.0);

    context.subject_inline_axis_is_horizontal = false;
    assert_eq!(absolutize_length(10.0, unit_code("cqi"), &context).px, 20.0);
    assert_eq!(absolutize_length(10.0, unit_code("cqb"), &context).px, 40.0);
}

#[test]
fn container_relative_viewport_fallback_records_its_dependency() {
    let mut dependency_was_recorded = false;
    let mut context = test_context();
    context.has_container_width_basis = true;
    context.container_width_basis = 800.0;
    context.container_width_basis_depends_on_viewport_metrics = true;
    context.resolved_viewport_relative_length = &raw mut dependency_was_recorded;

    let result = absolutize_length(10.0, unit_code("cqw"), &context);
    assert_eq!(result.px, 80.0);
    assert!(result.resolved_viewport_relative_length);
    assert!(dependency_was_recorded);
}

fn svg_axis_case(svg: bool, vertical: bool) -> [f64; 3] {
    use crate::css::property_metadata::property_id as prop;
    let zoom = (prop::ZOOM, retained_new(StyleValueData::Number { value: 2.0 }));
    let mut store = crate::css::cascaded_properties::CascadedPropertyStore::new();
    store.seed_retained_property(
        prop::WRITING_MODE,
        retained_new(StyleValueData::Keyword {
            keyword: if vertical {
                keyword::VERTICAL_RL
            } else {
                keyword::HORIZONTAL_TB
            },
        }),
        false,
        false,
    );
    for (property, value, unit) in [
        (prop::WIDTH, 7.0, "px"),
        (prop::HEIGHT, 1.0, "rem"),
        (prop::MARGIN_LEFT, 10.0, "vi"),
    ] {
        store.seed_retained_property(
            property,
            retained_new(StyleValueData::Length {
                value,
                unit: unit_code(unit) as u8,
            }),
            false,
            false,
        );
    }
    let mut box_type_input = element_adjustment_input();
    box_type_input.is_svg_element = svg;
    let environment = FfiStyleComputationEnvironment {
        box_type_input,
        color_scheme_input: FfiEffectiveColorSchemeInput {
            preferred_color_scheme: 0,
            has_document_supported_schemes: false,
            document_supported_scheme_codes: std::ptr::null(),
            document_supported_scheme_count: 0,
        },
        is_th_element: false,
        has_new_font_size: false,
        has_tree_counting_context: false,
        sibling_count: 0,
        sibling_index: 0,
        random_base_values: std::ptr::null(),
        random_base_value_count: 0,
        document_base_url: std::ptr::null(),
        document_base_url_length: 0,
        style_sheet_resource_contexts: std::ptr::null(),
        style_sheet_resource_context_count: 0,
        device_pixels_per_css_pixel: 1.0,
        initial_font_size_raw: 1024,
        default_font_size_raw: 1024,
        root_effective_zoom: 1.0,
    };
    let context = FfiLengthResolutionContext {
        viewport_width: 200.0,
        viewport_height: 100.0,
        root_font_metrics: FfiFontMetrics {
            font_size: 20.0,
            ..Default::default()
        },
        subject_inline_axis_is_horizontal: true,
        zoom: 1.0,
        ..Default::default()
    };
    crate::css::computed_values::with_default_seed(&crate::css::computed_values::DefaultGroups::default(), |seed| {
        let mut writer = crate::css::computed_values::GroupValuesWriter::seeded_for_child(
            seed,
            seed,
            [zoom].into_iter(),
            LonghandTableFlags::empty(),
            0,
        );
        let mut results = empty_longhand_driver_results();
        let mut color_scheme = -1;
        // SAFETY: All environment slices are empty and the borrowed contexts live throughout the drive.
        unsafe {
            drive_property_computation(
                &mut writer,
                &store,
                None,
                None,
                &environment,
                None,
                LONGHAND_DRIVE_PHASE_REMAINING,
                Some(&context),
                None,
                &mut results,
                &mut color_scheme,
                false,
            );
        }
        [prop::WIDTH, prop::HEIGHT, prop::MARGIN_LEFT].map(|property| match writer.get(property).unwrap().data() {
            StyleValueData::Length { value, unit } if *unit == px_length_unit() => *value,
            _ => panic!("expected absolute computed length"),
        })
    })
}

#[test]
fn svg_vertical_axis_preserves_unzoomed_lengths() {
    assert_eq!(svg_axis_case(true, true), [7.0, 20.0, 10.0]);
}
#[test]
fn html_vertical_axis_applies_zoom() {
    assert_eq!(svg_axis_case(false, true), [14.0, 40.0, 20.0]);
}
#[test]
fn svg_horizontal_axis_preserves_unzoomed_lengths() {
    assert_eq!(svg_axis_case(true, false), [7.0, 20.0, 20.0]);
}
fn font_phase_axis_case(
    declared: Option<u16>,
    parent_mode: u16,
    inherit_font: bool,
    context_horizontal: bool,
) -> [f64; 2] {
    use crate::css::property_metadata::property_id as prop;
    let zoom = (prop::ZOOM, retained_new(StyleValueData::Number { value: 1.0 }));
    let mut store = crate::css::cascaded_properties::CascadedPropertyStore::new();
    if let Some(keyword) = declared {
        store.seed_retained_property(
            prop::WRITING_MODE,
            retained_new(StyleValueData::Keyword { keyword }),
            false,
            false,
        );
    }
    if !inherit_font {
        store.seed_retained_property(
            prop::FONT_SIZE,
            retained_new(StyleValueData::Length {
                value: 10.0,
                unit: unit_code("vi") as u8,
            }),
            false,
            false,
        );
    }
    store.seed_retained_property(
        prop::LINE_HEIGHT,
        retained_new(StyleValueData::Length {
            value: 10.0,
            unit: unit_code("vb") as u8,
        }),
        false,
        false,
    );
    let parent = |property| match property {
        prop::WRITING_MODE => retained_new(StyleValueData::Keyword { keyword: parent_mode }),
        prop::FONT_SIZE => retained_new(StyleValueData::Length {
            value: 23.0,
            unit: px_length_unit(),
        }),
        prop::FONT_WEIGHT => retained_new(StyleValueData::Number { value: 400.0 }),
        _ => initial_value_handle(property).clone(),
    };
    crate::css::style::ViewLonghands::with_test_record(parent, |parent| {
        let snapshot = ParentSnapshot::from_view(parent, None, false);
        let mut box_type_input = element_adjustment_input();
        box_type_input.is_svg_element = false;
        let environment = FfiStyleComputationEnvironment {
            box_type_input,
            color_scheme_input: FfiEffectiveColorSchemeInput {
                preferred_color_scheme: 0,
                has_document_supported_schemes: false,
                document_supported_scheme_codes: std::ptr::null(),
                document_supported_scheme_count: 0,
            },
            is_th_element: false,
            has_new_font_size: false,
            has_tree_counting_context: false,
            sibling_count: 0,
            sibling_index: 0,
            random_base_values: std::ptr::null(),
            random_base_value_count: 0,
            document_base_url: std::ptr::null(),
            document_base_url_length: 0,
            style_sheet_resource_contexts: std::ptr::null(),
            style_sheet_resource_context_count: 0,
            device_pixels_per_css_pixel: 1.0,
            initial_font_size_raw: 1024,
            default_font_size_raw: 1024,
            root_effective_zoom: 1.0,
        };
        let context = FfiLengthResolutionContext {
            viewport_width: 800.0,
            viewport_height: 600.0,
            root_font_metrics: FfiFontMetrics {
                font_size: 20.0,
                ..Default::default()
            },
            subject_inline_axis_is_horizontal: context_horizontal,
            zoom: 1.0,
            ..Default::default()
        };
        crate::css::computed_values::with_default_seed(&crate::css::computed_values::DefaultGroups::default(), |seed| {
            let mut writer = crate::css::computed_values::GroupValuesWriter::seeded_for_child(
                seed,
                seed,
                [zoom].into_iter(),
                LonghandTableFlags::empty(),
                0,
            );
            let mut results = empty_longhand_driver_results();
            let mut color_scheme = -1;
            // SAFETY: All environment slices are empty and the borrowed contexts live throughout the drive.
            for phase in [LONGHAND_DRIVE_PHASE_FONT, LONGHAND_DRIVE_PHASE_LINE_HEIGHT] {
                unsafe {
                    drive_property_computation(
                        &mut writer,
                        &store,
                        Some(&snapshot),
                        None,
                        &environment,
                        None,
                        phase,
                        Some(&context),
                        None,
                        &mut results,
                        &mut color_scheme,
                        false,
                    );
                }
            }
            [prop::FONT_SIZE, prop::LINE_HEIGHT].map(|property| match writer.get(property).unwrap().data() {
                StyleValueData::Length { value, unit } if *unit == px_length_unit() => *value,
                _ => panic!("expected absolute computed length"),
            })
        })
    })
}

#[test]
fn font_and_line_height_follow_explicit_vertical_axis() {
    assert_eq!(
        font_phase_axis_case(Some(keyword::VERTICAL_RL), keyword::HORIZONTAL_TB, false, true),
        [60.0, 80.0]
    );
}
#[test]
fn font_and_line_height_follow_explicit_horizontal_axis() {
    assert_eq!(
        font_phase_axis_case(Some(keyword::HORIZONTAL_TB), keyword::VERTICAL_RL, false, false),
        [80.0, 60.0]
    );
}
#[test]
fn font_and_line_height_follow_inherited_axis() {
    assert_eq!(
        font_phase_axis_case(None, keyword::VERTICAL_RL, false, true),
        [60.0, 80.0]
    );
}
#[test]
fn inherited_computed_font_size_is_not_reinterpreted_in_child_axis() {
    assert_eq!(
        font_phase_axis_case(Some(keyword::VERTICAL_RL), keyword::HORIZONTAL_TB, true, true),
        [23.0, 80.0]
    );
}
#[test]
fn font_axis_initial_keyword_uses_initial_writing_mode() {
    assert_eq!(
        font_phase_axis_case(Some(keyword::INITIAL), keyword::VERTICAL_RL, false, false),
        [80.0, 60.0]
    );
}
