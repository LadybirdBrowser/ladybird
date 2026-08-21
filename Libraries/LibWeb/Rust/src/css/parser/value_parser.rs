/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// Parsed values use the thread-confined shared graph owned by the C++ style objects.
#![allow(clippy::arc_with_non_send_sync)]

use crate::css::css_enums::{keyword, keyword_from_ascii_case_insensitive};
use crate::css::css_tokenizer::tokenize_for_parser;
use crate::css::display::FfiDisplay;
use crate::css::ffi_support::FfiUtf16View;
use crate::css::parser::component_value::{ComponentValue, consume_a_list_of_component_values};
use crate::css::property_metadata::{
    FIRST_SHORTHAND_PROPERTY_ID, LAST_LONGHAND_PROPERTY_ID, property_accepted_keywords, property_accepts_only_keywords,
    property_has_coordinating_list_multiplicity, property_id, property_is_shorthand,
    property_resolve_legacy_value_alias,
};
use crate::css::style_value::StyleValueData;
use std::collections::BTreeMap;
use std::ffi::c_void;
use std::sync::{Arc, Mutex, OnceLock};

const PROPERTY_NOT_PORTED: NotHandledReason = NotHandledReason {
    label: "property:not-ported",
    c_label: b"property:not-ported\0",
};
const COMPONENT_VALUES_INVALID: NotHandledReason = NotHandledReason {
    label: "component-values:invalid",
    c_label: b"component-values:invalid\0",
};
const INVALID_FFI_INPUT: NotHandledReason = NotHandledReason {
    label: "ffi:invalid-input",
    c_label: b"ffi:invalid-input\0",
};

pub(crate) struct NotHandledReason {
    label: &'static str,
    c_label: &'static [u8],
}

/// The C++ value-parsing contexts which affect grammar decisions.
#[repr(u8)]
#[derive(Clone, Copy)]
#[allow(dead_code)]
pub enum FfiValueParsingContextKind {
    Property,
    Function,
    Descriptor,
    Special,
    RelativeColor,
}

/// One entry in the C++ Parser's value-context stack.
#[repr(C)]
pub struct FfiValueParsingContext {
    pub kind: FfiValueParsingContextKind,
    /// PropertyID, SpecialContext, or AtRuleID, depending on `kind`.
    pub value: u16,
    /// DescriptorID when `kind` is Descriptor.
    pub secondary_value: u16,
    /// Function name when `kind` is Function.
    pub name: FfiUtf16View,
    /// The RelativeColorParseContext allowed-channel bitmap.
    pub allowed_channels: u64,
}

/// Parser state required by CSS value parsing.
#[repr(C)]
pub struct ParseContext {
    pub in_quirks_mode: bool,
    pub is_svg_presentation_attribute: bool,
    pub value_contexts: *const FfiValueParsingContext,
    pub value_context_count: usize,
    pub document_url: *const u8,
    pub document_url_length: usize,
    pub document_base_url: *const u8,
    pub document_base_url_length: usize,
}

pub(crate) enum ParseOutcome {
    Parsed(Arc<StyleValueData>),
    Invalid,
    NotHandled(&'static NotHandledReason),
}

fn single_non_whitespace_value(values: &[ComponentValue]) -> Option<&ComponentValue> {
    let mut values = values.iter().filter(|value| !value.is_whitespace());
    let value = values.next()?;
    values.next().is_none().then_some(value)
}

fn parse_builtin_value(values: &[ComponentValue]) -> Option<StyleValueData> {
    let keyword = keyword_from_ascii_case_insensitive(single_non_whitespace_value(values)?.ident()?)?;
    matches!(
        keyword,
        keyword::INHERIT | keyword::INITIAL | keyword::UNSET | keyword::REVERT | keyword::REVERT_LAYER
    )
    .then_some(StyleValueData::Keyword { keyword })
}

fn property_uses_special_keyword_parser(property: u16) -> bool {
    // NB: These properties precede the generic parse_css_value_for_property()
    //     path in the C++ parse_css_value_in_cpp() switch. Their single-keyword
    //     results can have extra grammar constraints or specialized value types.
    matches!(
        property,
        property_id::ALIGN_ITEMS
            | property_id::ALIGN_SELF
            | property_id::ANCHOR_NAME
            | property_id::ANCHOR_SCOPE
            | property_id::ASPECT_RATIO
            | property_id::BACKDROP_FILTER
            | property_id::BACKGROUND_POSITION_X
            | property_id::BACKGROUND_POSITION_Y
            | property_id::BACKGROUND_REPEAT
            | property_id::BACKGROUND_SIZE
            | property_id::BORDER_IMAGE_SLICE
            | property_id::BOX_SHADOW
            | property_id::COLOR_SCHEME
            | property_id::CONTAIN
            | property_id::CONTAINER_NAME
            | property_id::CONTAINER_TYPE
            | property_id::CONTENT
            | property_id::COUNTER_INCREMENT
            | property_id::COUNTER_RESET
            | property_id::COUNTER_SET
            | property_id::CURSOR
            | property_id::DISPLAY
            | property_id::FILTER
            | property_id::FLEX
            | property_id::FONT_FEATURE_SETTINGS
            | property_id::FONT_LANGUAGE_OVERRIDE
            | property_id::FONT_VARIATION_SETTINGS
            | property_id::GRID
            | property_id::GRID_AREA
            | property_id::GRID_AUTO_COLUMNS
            | property_id::GRID_AUTO_ROWS
            | property_id::GRID_COLUMN
            | property_id::GRID_COLUMN_END
            | property_id::GRID_COLUMN_START
            | property_id::GRID_ROW
            | property_id::GRID_ROW_END
            | property_id::GRID_ROW_START
            | property_id::GRID_TEMPLATE
            | property_id::GRID_TEMPLATE_AREAS
            | property_id::GRID_TEMPLATE_COLUMNS
            | property_id::GRID_TEMPLATE_ROWS
            | property_id::JUSTIFY_ITEMS
            | property_id::JUSTIFY_SELF
            | property_id::MASK_REPEAT
            | property_id::MASK_SIZE
            | property_id::MATH_DEPTH
            | property_id::OVERFLOW
            | property_id::PAINT_ORDER
            | property_id::POSITION
            | property_id::POSITION_ANCHOR
            | property_id::POSITION_AREA
            | property_id::POSITION_TRY_FALLBACKS
            | property_id::POSITION_VISIBILITY
            | property_id::QUOTES
            | property_id::TEXT_DECORATION_LINE
            | property_id::TEXT_INDENT
            | property_id::TEXT_SHADOW
            | property_id::TEXT_UNDERLINE_POSITION
            | property_id::TOUCH_ACTION
            | property_id::TRANSFORM
            | property_id::TRANSFORM_ORIGIN
            | property_id::TRANSITION_PROPERTY
            | property_id::WHITE_SPACE
            | property_id::WHITE_SPACE_TRIM
            | property_id::WILL_CHANGE
    )
}

fn parse_generic_property_keyword(property: u16, values: &[ComponentValue]) -> ParseOutcome {
    if !(FIRST_SHORTHAND_PROPERTY_ID..=LAST_LONGHAND_PROPERTY_ID).contains(&property)
        || property_is_shorthand(property)
        || property_has_coordinating_list_multiplicity(property)
        || property_uses_special_keyword_parser(property)
    {
        return ParseOutcome::NotHandled(&PROPERTY_NOT_PORTED);
    }

    let Some(identifier) = single_non_whitespace_value(values).and_then(ComponentValue::ident) else {
        return ParseOutcome::NotHandled(&PROPERTY_NOT_PORTED);
    };
    let parsed_keyword = keyword_from_ascii_case_insensitive(identifier);
    if let Some(parsed_keyword) = parsed_keyword
        && property_accepted_keywords(property)
            .binary_search(&parsed_keyword)
            .is_ok()
    {
        let keyword = property_resolve_legacy_value_alias(property, parsed_keyword);
        return ParseOutcome::Parsed(Arc::new(StyleValueData::Keyword { keyword }));
    }
    if property_accepts_only_keywords(property) {
        return ParseOutcome::Invalid;
    }
    ParseOutcome::NotHandled(&PROPERTY_NOT_PORTED)
}

fn parse_display_keyword(values: &[ComponentValue]) -> ParseOutcome {
    let Some(identifier) = single_non_whitespace_value(values).and_then(ComponentValue::ident) else {
        return ParseOutcome::NotHandled(&PROPERTY_NOT_PORTED);
    };
    let Some(keyword) = keyword_from_ascii_case_insensitive(identifier) else {
        return ParseOutcome::Invalid;
    };
    let Some(display) = FfiDisplay::from_single_keyword(keyword) else {
        return ParseOutcome::Invalid;
    };
    ParseOutcome::Parsed(Arc::new(StyleValueData::Display { raw: display.encoded() }))
}

/// Parse a property value using the grammars which have been ported to Rust.
///
/// `Invalid` is reserved for grammars which Rust handles completely. Until a
/// grammar is ported, C++ remains authoritative through `NotHandled`.
pub(crate) fn parse_css_value(_context: &ParseContext, property_id: u16, values: &[ComponentValue]) -> ParseOutcome {
    if let Some(value) = parse_builtin_value(values) {
        return ParseOutcome::Parsed(Arc::new(value));
    }
    if property_id == property_id::DISPLAY {
        return parse_display_keyword(values);
    }
    parse_generic_property_keyword(property_id, values)
}

/// The result category returned through the value-parser FFI.
#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FfiParseStatus {
    Parsed,
    Invalid,
    NotHandled,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord)]
enum StatisticKind {
    Parsed,
    NotHandled,
}

type StatisticKey = (StatisticKind, u16, &'static str);

fn statistics() -> &'static Mutex<BTreeMap<StatisticKey, u64>> {
    static STATISTICS: OnceLock<Mutex<BTreeMap<StatisticKey, u64>>> = OnceLock::new();
    STATISTICS.get_or_init(|| Mutex::new(BTreeMap::new()))
}

fn statistics_enabled() -> bool {
    static ENABLED: OnceLock<bool> = OnceLock::new();
    *ENABLED.get_or_init(|| std::env::var("LIBWEB_PARSE_FALLBACK_STATS").as_deref() == Ok("1"))
}

fn record_outcome(property_id: u16, outcome: &ParseOutcome) {
    if !statistics_enabled() {
        return;
    }
    let key = match outcome {
        ParseOutcome::Parsed(_) => (StatisticKind::Parsed, property_id, ""),
        ParseOutcome::Invalid => return,
        ParseOutcome::NotHandled(reason) => (StatisticKind::NotHandled, property_id, reason.label),
    };
    let mut statistics = statistics().lock().unwrap_or_else(|poisoned| poisoned.into_inner());
    *statistics.entry(key).or_default() += 1;
}

/// Tries the Rust value parser and returns a strong StyleValueData reference
/// on success. A null return is disambiguated by `out_status`.
///
/// # Safety
/// All non-null pointers must remain readable for their accompanying lengths
/// for the duration of this call. `out_status` and `out_reason` must be valid
/// writable pointers.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_parse_css_value(
    context: *const ParseContext,
    property_id: u16,
    source: FfiUtf16View,
    out_status: *mut FfiParseStatus,
    out_reason: *mut *const u8,
) -> *const c_void {
    crate::abort_on_panic(|| {
        let invalid_ffi_result = || {
            if !out_status.is_null() {
                unsafe { *out_status = FfiParseStatus::NotHandled };
            }
            if !out_reason.is_null() {
                unsafe { *out_reason = INVALID_FFI_INPUT.c_label.as_ptr() };
            }
            std::ptr::null()
        };

        if context.is_null() || out_status.is_null() || out_reason.is_null() {
            return invalid_ffi_result();
        }
        let Some(source) = (unsafe { source.units() }) else {
            return invalid_ffi_result();
        };
        let context = unsafe { &*context };
        let outcome = match consume_a_list_of_component_values(&tokenize_for_parser(source)) {
            Ok(values) => parse_css_value(context, property_id, &values),
            Err(()) => ParseOutcome::NotHandled(&COMPONENT_VALUES_INVALID),
        };
        record_outcome(property_id, &outcome);

        match outcome {
            ParseOutcome::Parsed(value) => {
                unsafe {
                    *out_status = FfiParseStatus::Parsed;
                    *out_reason = std::ptr::null();
                }
                Arc::into_raw(value).cast()
            }
            ParseOutcome::Invalid => {
                unsafe {
                    *out_status = FfiParseStatus::Invalid;
                    *out_reason = std::ptr::null();
                }
                std::ptr::null()
            }
            ParseOutcome::NotHandled(reason) => {
                unsafe {
                    *out_status = FfiParseStatus::NotHandled;
                    *out_reason = reason.c_label.as_ptr();
                }
                std::ptr::null()
            }
        }
    })
}

/// Prints the per-property Rust parse and C++ fallback counts accumulated by
/// this process. C++ calls this at process exit when statistics are enabled.
#[unsafe(no_mangle)]
pub extern "C" fn rust_parse_fallback_stats_dump() {
    if !statistics_enabled() {
        return;
    }
    let statistics = statistics().lock().unwrap_or_else(|poisoned| poisoned.into_inner());
    for (&(kind, property_id, reason), &count) in statistics.iter() {
        let property_name = crate::css::property_metadata::property_name(property_id);
        match kind {
            StatisticKind::Parsed => {
                eprintln!("LIBWEB_PARSE_VALUE parsed property={property_name} property-id={property_id} count={count}");
            }
            StatisticKind::NotHandled => {
                eprintln!(
                    "LIBWEB_PARSE_VALUE not-handled property={property_name} property-id={property_id} reason={reason} count={count}"
                );
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn context() -> ParseContext {
        ParseContext {
            in_quirks_mode: false,
            is_svg_presentation_attribute: false,
            value_contexts: std::ptr::null(),
            value_context_count: 0,
            document_url: std::ptr::null(),
            document_url_length: 0,
            document_base_url: std::ptr::null(),
            document_base_url_length: 0,
        }
    }

    fn parse(property: u16, source: &str) -> ParseOutcome {
        let values = consume_a_list_of_component_values(&tokenize_for_parser(source.as_bytes())).unwrap();
        parse_css_value(&context(), property, &values)
    }

    fn parsed_keyword(outcome: ParseOutcome) -> u16 {
        let ParseOutcome::Parsed(value) = outcome else {
            panic!("value should parse");
        };
        let StyleValueData::Keyword { keyword } = &*value else {
            panic!("value should be a keyword");
        };
        *keyword
    }

    #[test]
    fn unported_property_falls_back_to_cpp() {
        let values = consume_a_list_of_component_values(&tokenize_for_parser(b"0.5")).unwrap();
        let ParseOutcome::NotHandled(reason) = parse_css_value(&context(), 1, &values) else {
            panic!("unported value should not be authoritative");
        };
        assert_eq!(reason.label, "property:not-ported");
    }

    #[test]
    fn parses_keywords_accepted_by_generic_properties() {
        assert_eq!(parsed_keyword(parse(property_id::APPEARANCE, "none")), keyword::NONE);
        assert_eq!(parsed_keyword(parse(property_id::WIDTH, "auto")), keyword::AUTO);
        assert_eq!(parsed_keyword(parse(property_id::OVERFLOW_X, "overlay")), keyword::AUTO);
    }

    #[test]
    fn rejects_unknown_identifiers_for_keyword_only_properties() {
        assert!(matches!(parse(property_id::APPEARANCE, "bogus"), ParseOutcome::Invalid));
    }

    #[test]
    fn leaves_mixed_and_special_grammars_with_cpp() {
        assert!(matches!(parse(property_id::COLOR, "red"), ParseOutcome::NotHandled(_)));
        assert!(matches!(
            parse(property_id::ALIGN_ITEMS, "normal"),
            ParseOutcome::NotHandled(_)
        ));
        assert!(matches!(
            parse(property_id::ANIMATION_DIRECTION, "reverse"),
            ParseOutcome::NotHandled(_)
        ));
    }

    #[test]
    fn parses_single_keyword_display_values() {
        let ParseOutcome::Parsed(value) = parse(property_id::DISPLAY, "none") else {
            panic!("display keyword should parse");
        };
        assert!(matches!(&*value, StyleValueData::Display { raw } if *raw == FfiDisplay::none().encoded()));
        assert!(matches!(parse(property_id::DISPLAY, "bogus"), ParseOutcome::Invalid));
        assert!(matches!(
            parse(property_id::DISPLAY, "inline flow"),
            ParseOutcome::NotHandled(_)
        ));
    }

    #[test]
    fn parses_css_wide_keywords_as_whole_values() {
        for (source, expected_keyword) in [
            ("initial", keyword::INITIAL),
            (" InHeRiT ", keyword::INHERIT),
            ("unset", keyword::UNSET),
            ("revert", keyword::REVERT),
            ("revert-layer", keyword::REVERT_LAYER),
        ] {
            let values = consume_a_list_of_component_values(&tokenize_for_parser(source.as_bytes())).unwrap();
            let ParseOutcome::Parsed(value) = parse_css_value(&context(), 1, &values) else {
                panic!("CSS-wide keyword should parse");
            };
            assert!(matches!(&*value, StyleValueData::Keyword { keyword } if *keyword == expected_keyword));
        }
    }

    #[test]
    fn does_not_parse_css_wide_keywords_as_partial_values() {
        let values = consume_a_list_of_component_values(&tokenize_for_parser(b"inherit extra")).unwrap();
        assert!(matches!(
            parse_css_value(&context(), 1, &values),
            ParseOutcome::NotHandled(_)
        ));
    }
}
