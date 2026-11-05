/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The CSS Object Model, https://drafts.csswg.org/cssom/

pub(crate) mod animation_event;
#[allow(clippy::module_inception)]
pub(crate) mod css;
pub(crate) mod css_condition_rule;
pub(crate) mod css_container_rule;
pub(crate) mod css_counter_style_rule;
pub(crate) mod css_descriptors;
pub(crate) mod css_font_face_descriptors;
pub(crate) mod css_font_face_rule;
pub(crate) mod css_font_feature_values_map;
pub(crate) mod css_font_feature_values_rule;
pub(crate) mod css_function_declarations;
pub(crate) mod css_function_descriptors;
pub(crate) mod css_function_rule;
pub(crate) mod css_grouping_rule;
pub(crate) mod css_image_value;
pub(crate) mod css_import_rule;
pub(crate) mod css_keyframe_rule;
pub(crate) mod css_keyframes_rule;
pub(crate) mod css_keyword_value;
pub(crate) mod css_layer_block_rule;
pub(crate) mod css_layer_statement_rule;
pub(crate) mod css_margin_rule;
pub(crate) mod css_math_clamp;
pub(crate) mod css_math_invert;
pub(crate) mod css_math_max;
pub(crate) mod css_math_min;
pub(crate) mod css_math_negate;
pub(crate) mod css_math_product;
pub(crate) mod css_math_sum;
pub(crate) mod css_math_value;
pub(crate) mod css_matrix_component;
pub(crate) mod css_media_rule;
pub(crate) mod css_namespace_rule;
pub(crate) mod css_nested_declarations;
pub(crate) mod css_numeric_array;
pub(crate) mod css_numeric_value;
pub(crate) mod css_page_descriptors;
pub(crate) mod css_page_rule;
pub(crate) mod css_perspective;
pub(crate) mod css_position_try_descriptors;
pub(crate) mod css_position_try_rule;
pub(crate) mod css_property_rule;
pub(crate) mod css_rotate;
pub(crate) mod css_rule;
pub(crate) mod css_rule_list;
pub(crate) mod css_scale;
pub(crate) mod css_scope_rule;
pub(crate) mod css_skew;
pub(crate) mod css_skew_x;
pub(crate) mod css_skew_y;
pub(crate) mod css_style_declaration;
pub(crate) mod css_style_properties;
pub(crate) mod css_style_rule;
pub(crate) mod css_style_sheet;
pub(crate) mod css_style_value;
pub(crate) mod css_supports_rule;
pub(crate) mod css_transform_component;
pub(crate) mod css_transform_value;
pub(crate) mod css_translate;
pub(crate) mod css_unit_value;
pub(crate) mod css_unparsed_value;
pub(crate) mod css_variable_reference_value;
pub(crate) mod exports;
pub(crate) mod font_face_set;
pub(crate) mod font_face_set_load_event;
pub(crate) mod highlight;
pub(crate) mod highlight_registry;
pub(crate) mod media_list;
pub(crate) mod media_query_list;
pub(crate) mod media_query_list_event;
pub(crate) mod screen;
pub(crate) mod screen_orientation;
pub(crate) mod style_property_map;
pub(crate) mod style_property_map_read_only;
pub(crate) mod style_sheet;
pub(crate) mod style_sheet_list;
pub(crate) mod transition_event;
pub(crate) mod typed_om;
pub(crate) mod visual_viewport;
