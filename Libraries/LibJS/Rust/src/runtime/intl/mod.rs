/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The Intl abstract operations and built-in objects.

pub mod abstract_operations;
pub mod collator;
pub mod collator_compare_function;
pub mod collator_constructor;
pub mod collator_prototype;
pub mod date_time_format;
pub mod date_time_format_constructor;
pub mod date_time_format_function;
pub mod date_time_format_prototype;
pub mod display_names;
pub mod display_names_constructor;
pub mod display_names_prototype;
pub mod duration_format;
pub mod duration_format_constructor;
pub mod duration_format_prototype;
#[allow(
    clippy::module_inception,
    reason = "the module defines the Intl object of the Intl namespace"
)]
pub mod intl;
pub mod intl_object;
pub mod list_format;
pub mod list_format_constructor;
pub mod list_format_prototype;
pub mod locale;
pub mod locale_constructor;
pub mod locale_prototype;
pub mod mathematical_value;
pub mod number_format;
pub mod number_format_constructor;
pub mod number_format_function;
pub mod number_format_prototype;
pub mod plural_rules;
pub mod plural_rules_constructor;
pub mod plural_rules_prototype;
pub mod relative_time_format;
pub mod relative_time_format_constructor;
pub mod relative_time_format_prototype;
pub mod segment_iterator;
pub mod segment_iterator_prototype;
pub mod segmenter;
pub mod segmenter_constructor;
pub mod segmenter_prototype;
pub mod segments;
pub mod segments_prototype;
pub mod single_unit_identifiers;
