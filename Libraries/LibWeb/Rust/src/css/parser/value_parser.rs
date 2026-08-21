/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_tokenizer::tokenize_for_parser;
use crate::css::ffi_support::FfiUtf16View;
use crate::css::parser::component_value::{ComponentValue, consume_a_list_of_component_values};
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

#[allow(dead_code)]
pub(crate) enum ParseOutcome {
    Parsed(Arc<StyleValueData>),
    Invalid,
    NotHandled(&'static NotHandledReason),
}

/// Parse a property value using the grammars which have been ported to Rust.
///
/// `Invalid` is reserved for grammars which Rust handles completely. Until a
/// grammar is ported, C++ remains authoritative through `NotHandled`.
pub(crate) fn parse_css_value(_context: &ParseContext, _property_id: u16, _values: &[ComponentValue]) -> ParseOutcome {
    ParseOutcome::NotHandled(&PROPERTY_NOT_PORTED)
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

    #[test]
    fn unported_property_falls_back_to_cpp() {
        let values = consume_a_list_of_component_values(&tokenize_for_parser(b"0.5")).unwrap();
        let ParseOutcome::NotHandled(reason) = parse_css_value(&context(), 1, &values) else {
            panic!("unported value should not be authoritative");
        };
        assert_eq!(reason.label, "property:not-ported");
    }
}
