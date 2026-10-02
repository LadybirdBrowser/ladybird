/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The locale data, collation, list formatting, display names and segmentation of LibUnicode that the Intl builtins
//! use, through the C exports of Libraries/LibUnicode/IntlExports.h, and the Rust form of Unicode::LocaleID.

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;

use crate::utf16::{Utf16StringBuilder, Utf16View};

/// UnicodeIntlText: a borrowed string in either of AK::Utf16View's storages.
#[repr(C)]
#[derive(Clone, Copy)]
struct UnicodeIntlText {
    ascii: *const u8,
    utf16: *const u16,
    length: usize,
}

impl UnicodeIntlText {
    fn of(view: Utf16View<'_>) -> Self {
        match view {
            Utf16View::Ascii(units) => Self {
                ascii: units.as_ptr(),
                utf16: core::ptr::null(),
                length: units.len(),
            },
            Utf16View::Utf16(units) => Self {
                ascii: core::ptr::null(),
                utf16: units.as_ptr(),
                length: units.len(),
            },
        }
    }
}

#[repr(C)]
struct UnicodeTextMappingOutput {
    context: *mut c_void,
    allocate_text: unsafe extern "C" fn(context: *mut c_void, length: usize) -> *mut u16,
    append_edit: unsafe extern "C" fn(
        context: *mut c_void,
        source_start: usize,
        source_length: usize,
        destination_start: usize,
        destination_length: usize,
    ),
}

type UnicodeAppendString = unsafe extern "C" fn(context: *mut c_void, text: *const u16, length: usize);
type UnicodeAppendLocaleIDPart = unsafe extern "C" fn(context: *mut c_void, part: u8, text: *const u16, length: usize);
type UnicodeAppendListFormatPart = unsafe extern "C" fn(
    context: *mut c_void,
    type_: *const u16,
    type_length: usize,
    value: *const u16,
    value_length: usize,
);

unsafe extern "C" {
    fn unicode_parse_unicode_locale_id(
        locale: UnicodeIntlText,
        context: *mut c_void,
        append_part: UnicodeAppendLocaleIDPart,
    ) -> bool;
    fn unicode_is_unicode_language_id(language: UnicodeIntlText) -> bool;
    fn unicode_is_type_identifier(identifier: UnicodeIntlText) -> bool;
    fn unicode_canonicalize_unicode_locale_id(locale: UnicodeIntlText, output: UnicodeTextMappingOutput) -> bool;
    fn unicode_canonicalize_unicode_extension_values(
        key: UnicodeIntlText,
        value: UnicodeIntlText,
        output: UnicodeTextMappingOutput,
    );
    fn unicode_is_locale_available(locale: UnicodeIntlText) -> bool;
    fn unicode_add_likely_subtags(locale: UnicodeIntlText, output: UnicodeTextMappingOutput) -> bool;
    fn unicode_remove_likely_subtags(locale: UnicodeIntlText, output: UnicodeTextMappingOutput) -> bool;
    fn unicode_is_locale_character_ordering_right_to_left(locale: UnicodeIntlText) -> bool;
    fn unicode_default_locale(output: UnicodeTextMappingOutput);

    fn unicode_available_keyword_values(
        locale: UnicodeIntlText,
        key: UnicodeIntlText,
        context: *mut c_void,
        append: UnicodeAppendString,
    );
    fn unicode_available_calendars(context: *mut c_void, append: UnicodeAppendString);
    fn unicode_available_calendars_of_locale(
        locale: UnicodeIntlText,
        context: *mut c_void,
        append: UnicodeAppendString,
    );
    fn unicode_available_currencies(context: *mut c_void, append: UnicodeAppendString);
    fn unicode_available_collations(context: *mut c_void, append: UnicodeAppendString);
    fn unicode_available_collations_of_locale(
        locale: UnicodeIntlText,
        context: *mut c_void,
        append: UnicodeAppendString,
    );
    fn unicode_available_hour_cycles_of_locale(
        locale: UnicodeIntlText,
        context: *mut c_void,
        append: UnicodeAppendString,
    );
    fn unicode_available_number_systems(context: *mut c_void, append: UnicodeAppendString);
    fn unicode_available_number_systems_of_locale(
        locale: UnicodeIntlText,
        context: *mut c_void,
        append: UnicodeAppendString,
    );
    fn unicode_available_time_zones_in_region(
        region: UnicodeIntlText,
        context: *mut c_void,
        append: UnicodeAppendString,
    );

    fn unicode_week_info_of_locale(
        locale: UnicodeIntlText,
        has_first_day_of_week: *mut bool,
        first_day_of_week: *mut u8,
        weekend_days: *mut u8,
        weekend_day_count: *mut usize,
    );

    #[allow(
        clippy::too_many_arguments,
        reason = "the export takes the arguments of Unicode::Collator::create"
    )]
    fn unicode_collator_create(
        locale: UnicodeIntlText,
        usage: u8,
        collation: UnicodeIntlText,
        has_sensitivity: bool,
        sensitivity: u8,
        case_first: u8,
        numeric: bool,
        has_ignore_punctuation: bool,
        ignore_punctuation: bool,
    ) -> *mut c_void;
    fn unicode_collator_compare(collator: *const c_void, lhs: UnicodeIntlText, rhs: UnicodeIntlText) -> u8;
    fn unicode_collator_sensitivity(collator: *const c_void) -> u8;
    fn unicode_collator_ignore_punctuation(collator: *const c_void) -> bool;
    fn unicode_collator_destroy(collator: *mut c_void);

    fn unicode_list_format_create(locale: UnicodeIntlText, type_: u8, style: u8) -> *mut c_void;
    fn unicode_list_format_format(
        list_format: *const c_void,
        list: *const UnicodeIntlText,
        list_size: usize,
        output: UnicodeTextMappingOutput,
    );
    fn unicode_list_format_format_to_parts(
        list_format: *const c_void,
        list: *const UnicodeIntlText,
        list_size: usize,
        context: *mut c_void,
        append_part: UnicodeAppendListFormatPart,
    );
    fn unicode_list_format_destroy(list_format: *mut c_void);

    fn unicode_language_display_name(
        locale: UnicodeIntlText,
        language: UnicodeIntlText,
        language_display: u8,
        output: UnicodeTextMappingOutput,
    ) -> bool;
    fn unicode_region_display_name(
        locale: UnicodeIntlText,
        region: UnicodeIntlText,
        output: UnicodeTextMappingOutput,
    ) -> bool;
    fn unicode_script_display_name(
        locale: UnicodeIntlText,
        script: UnicodeIntlText,
        output: UnicodeTextMappingOutput,
    ) -> bool;
    fn unicode_currency_display_name(
        locale: UnicodeIntlText,
        currency: UnicodeIntlText,
        style: u8,
        output: UnicodeTextMappingOutput,
    ) -> bool;
    fn unicode_calendar_display_name(
        locale: UnicodeIntlText,
        calendar: UnicodeIntlText,
        output: UnicodeTextMappingOutput,
    ) -> bool;
    fn unicode_date_time_field_display_name(
        locale: UnicodeIntlText,
        field: UnicodeIntlText,
        style: u8,
        output: UnicodeTextMappingOutput,
    ) -> bool;

    fn unicode_segmenter_create(locale: UnicodeIntlText, granularity: u8) -> *mut c_void;
    fn unicode_segmenter_clone(segmenter: *const c_void) -> *mut c_void;
    fn unicode_segmenter_set_segmented_text(segmenter: *mut c_void, text: UnicodeIntlText);
    fn unicode_segmenter_current_boundary(segmenter: *mut c_void) -> usize;
    fn unicode_segmenter_previous_boundary(
        segmenter: *mut c_void,
        index: usize,
        inclusive: bool,
        boundary: *mut usize,
    ) -> bool;
    fn unicode_segmenter_next_boundary(
        segmenter: *mut c_void,
        index: usize,
        inclusive: bool,
        boundary: *mut usize,
    ) -> bool;
    fn unicode_segmenter_is_current_boundary_word_like(segmenter: *const c_void) -> bool;
    fn unicode_segmenter_destroy(segmenter: *mut c_void);
}

unsafe extern "C" fn allocate_text(context: *mut c_void, length: usize) -> *mut u16 {
    // SAFETY: The context is the output buffer the caller passed, which outlives the call.
    let output = unsafe { &mut *context.cast::<Vec<u16>>() };
    output.resize(length, 0);
    output.as_mut_ptr()
}

unsafe extern "C" fn ignore_edit(_: *mut c_void, _: usize, _: usize, _: usize, _: usize) {}

/// Calls an export that writes one string through a UnicodeTextMappingOutput, and returns what it returns with the
/// string it wrote.
fn with_text_output<R>(write: impl FnOnce(UnicodeTextMappingOutput) -> R) -> (R, Utf16String) {
    let mut output: Vec<u16> = Vec::new();
    let result = write(UnicodeTextMappingOutput {
        context: (&raw mut output).cast(),
        allocate_text,
        append_edit: ignore_edit,
    });
    (result, Utf16String::from_utf16(&output))
}

fn text_output(write: impl FnOnce(UnicodeTextMappingOutput)) -> Utf16String {
    with_text_output(write).1
}

fn optional_text_output(write: impl FnOnce(UnicodeTextMappingOutput) -> bool) -> Option<Utf16String> {
    let (written, text) = with_text_output(write);
    written.then_some(text)
}

unsafe extern "C" fn append_string(context: *mut c_void, text: *const u16, length: usize) {
    // SAFETY: The context is the list the caller passed, and the export passes `length` valid code units.
    let (list, text) = unsafe {
        (
            &mut *context.cast::<Vec<Utf16String>>(),
            core::slice::from_raw_parts(text, length),
        )
    };
    list.push(Utf16String::from_utf16(text));
}

/// Calls an export that appends the strings of a list one at a time, and collects them.
fn string_list_output(append: impl FnOnce(*mut c_void, UnicodeAppendString)) -> Vec<Utf16String> {
    let mut list: Vec<Utf16String> = Vec::new();
    append((&raw mut list).cast(), append_string);
    list
}

/// Unicode::LanguageID.
#[derive(Clone, Default, PartialEq, Eq)]
pub struct LanguageID {
    pub is_root: bool,
    pub language: Option<Utf16String>,
    pub script: Option<Utf16String>,
    pub region: Option<Utf16String>,
    pub variants: Vec<Utf16String>,
}

/// Unicode::Keyword.
#[derive(Clone, Default, PartialEq, Eq)]
pub struct Keyword {
    pub key: Utf16String,
    pub value: Utf16String,
}

/// Unicode::LocaleExtension.
#[derive(Clone, Default, PartialEq, Eq)]
pub struct LocaleExtension {
    pub attributes: Vec<Utf16String>,
    pub keywords: Vec<Keyword>,
}

/// Unicode::TransformedField.
#[derive(Clone, Default, PartialEq, Eq)]
pub struct TransformedField {
    pub key: Utf16String,
    pub value: Utf16String,
}

/// Unicode::TransformedExtension.
#[derive(Clone, Default, PartialEq, Eq)]
pub struct TransformedExtension {
    pub language: Option<LanguageID>,
    pub fields: Vec<TransformedField>,
}

/// Unicode::OtherExtension.
#[derive(Clone, Default, PartialEq, Eq)]
pub struct OtherExtension {
    pub key: u8,
    pub value: Utf16String,
}

/// Unicode::Extension.
#[derive(Clone, PartialEq, Eq)]
pub enum Extension {
    Locale(LocaleExtension),
    Transformed(TransformedExtension),
    Other(OtherExtension),
}

/// Unicode::LocaleID.
#[derive(Clone, Default, PartialEq, Eq)]
pub struct LocaleID {
    pub language_id: LanguageID,
    pub extensions: Vec<Extension>,
    pub private_use_extensions: Vec<Utf16String>,
}

/// The parts UnicodeLocaleIDPart names, which unicode_parse_unicode_locale_id() passes in the order it parsed them.
#[derive(Clone, Copy, PartialEq, Eq)]
#[repr(u8)]
enum LocaleIDPart {
    Root,
    Language,
    Script,
    Region,
    Variant,
    LocaleExtension,
    LocaleExtensionAttribute,
    LocaleExtensionKeywordKey,
    LocaleExtensionKeywordValue,
    TransformedExtension,
    TransformedLanguage,
    TransformedFieldKey,
    TransformedFieldValue,
    OtherExtensionKey,
    OtherExtensionValue,
    PrivateUseExtension,
}

impl LocaleIDPart {
    fn from_u8(part: u8) -> Self {
        const PARTS: [LocaleIDPart; 16] = [
            LocaleIDPart::Root,
            LocaleIDPart::Language,
            LocaleIDPart::Script,
            LocaleIDPart::Region,
            LocaleIDPart::Variant,
            LocaleIDPart::LocaleExtension,
            LocaleIDPart::LocaleExtensionAttribute,
            LocaleIDPart::LocaleExtensionKeywordKey,
            LocaleIDPart::LocaleExtensionKeywordValue,
            LocaleIDPart::TransformedExtension,
            LocaleIDPart::TransformedLanguage,
            LocaleIDPart::TransformedFieldKey,
            LocaleIDPart::TransformedFieldValue,
            LocaleIDPart::OtherExtensionKey,
            LocaleIDPart::OtherExtensionValue,
            LocaleIDPart::PrivateUseExtension,
        ];
        PARTS[usize::from(part)]
    }
}

/// Builds a LocaleID from the parts unicode_parse_unicode_locale_id() passes.
#[derive(Default)]
struct LocaleIDBuilder {
    locale_id: LocaleID,
    in_transformed_language: bool,
}

impl LocaleIDBuilder {
    fn language_id(&mut self) -> &mut LanguageID {
        if self.in_transformed_language
            && let Some(Extension::Transformed(extension)) = self.locale_id.extensions.last_mut()
        {
            return extension.language.get_or_insert_with(LanguageID::default);
        }
        &mut self.locale_id.language_id
    }

    fn append(&mut self, part: LocaleIDPart, text: Utf16String) {
        match part {
            LocaleIDPart::Root => self.language_id().is_root = true,
            LocaleIDPart::Language => self.language_id().language = Some(text),
            LocaleIDPart::Script => self.language_id().script = Some(text),
            LocaleIDPart::Region => self.language_id().region = Some(text),
            LocaleIDPart::Variant => self.language_id().variants.push(text),
            LocaleIDPart::LocaleExtension => {
                self.in_transformed_language = false;
                self.locale_id
                    .extensions
                    .push(Extension::Locale(LocaleExtension::default()));
            }
            LocaleIDPart::TransformedExtension => {
                self.in_transformed_language = false;
                self.locale_id
                    .extensions
                    .push(Extension::Transformed(TransformedExtension::default()));
            }
            LocaleIDPart::TransformedLanguage => {
                self.in_transformed_language = true;
                if let Some(Extension::Transformed(extension)) = self.locale_id.extensions.last_mut() {
                    extension.language = Some(LanguageID::default());
                }
            }
            LocaleIDPart::LocaleExtensionAttribute => {
                if let Some(Extension::Locale(extension)) = self.locale_id.extensions.last_mut() {
                    extension.attributes.push(text);
                }
            }
            LocaleIDPart::LocaleExtensionKeywordKey => {
                if let Some(Extension::Locale(extension)) = self.locale_id.extensions.last_mut() {
                    extension.keywords.push(Keyword {
                        key: text,
                        value: Utf16String::default(),
                    });
                }
            }
            LocaleIDPart::LocaleExtensionKeywordValue => {
                if let Some(Extension::Locale(extension)) = self.locale_id.extensions.last_mut()
                    && let Some(keyword) = extension.keywords.last_mut()
                {
                    keyword.value = text;
                }
            }
            LocaleIDPart::TransformedFieldKey => {
                self.in_transformed_language = false;
                if let Some(Extension::Transformed(extension)) = self.locale_id.extensions.last_mut() {
                    extension.fields.push(TransformedField {
                        key: text,
                        value: Utf16String::default(),
                    });
                }
            }
            LocaleIDPart::TransformedFieldValue => {
                if let Some(Extension::Transformed(extension)) = self.locale_id.extensions.last_mut()
                    && let Some(field) = extension.fields.last_mut()
                {
                    field.value = text;
                }
            }
            LocaleIDPart::OtherExtensionKey => {
                self.in_transformed_language = false;
                let key = Utf16View::of_string(&text).code_unit_at(0);
                self.locale_id.extensions.push(Extension::Other(OtherExtension {
                    key: u8::try_from(key).expect("an extension singleton is ASCII"),
                    value: Utf16String::default(),
                }));
            }
            LocaleIDPart::OtherExtensionValue => {
                if let Some(Extension::Other(extension)) = self.locale_id.extensions.last_mut() {
                    extension.value = text;
                }
            }
            LocaleIDPart::PrivateUseExtension => {
                self.in_transformed_language = false;
                self.locale_id.private_use_extensions.push(text);
            }
        }
    }
}

unsafe extern "C" fn append_locale_id_part(context: *mut c_void, part: u8, text: *const u16, length: usize) {
    // SAFETY: The context is the builder the caller passed, and the export passes `length` valid code units.
    let (builder, text) = unsafe {
        (
            &mut *context.cast::<LocaleIDBuilder>(),
            core::slice::from_raw_parts(text, length),
        )
    };
    builder.append(LocaleIDPart::from_u8(part), Utf16String::from_utf16(text));
}

/// Unicode::parse_unicode_locale_id.
pub fn parse_unicode_locale_id(locale: Utf16View<'_>) -> Option<LocaleID> {
    let mut builder = LocaleIDBuilder::default();
    // SAFETY: The locale is valid for its length, and the callback appends to the builder.
    let parsed = unsafe {
        unicode_parse_unicode_locale_id(
            UnicodeIntlText::of(locale),
            (&raw mut builder).cast(),
            append_locale_id_part,
        )
    };
    parsed.then_some(builder.locale_id)
}

/// Whether Unicode::parse_unicode_language_id parses `language`.
pub fn is_unicode_language_id(language: Utf16View<'_>) -> bool {
    // SAFETY: The language is valid for its length.
    unsafe { unicode_is_unicode_language_id(UnicodeIntlText::of(language)) }
}

fn all_code_units(view: Utf16View<'_>, predicate: impl Fn(u16) -> bool) -> bool {
    view.code_units().all(predicate)
}

fn is_ascii_alpha(code_unit: u16) -> bool {
    u8::try_from(code_unit).is_ok_and(|byte| byte.is_ascii_alphabetic())
}

fn is_ascii_digit(code_unit: u16) -> bool {
    u8::try_from(code_unit).is_ok_and(|byte| byte.is_ascii_digit())
}

fn is_ascii_alphanumeric(code_unit: u16) -> bool {
    u8::try_from(code_unit).is_ok_and(|byte| byte.is_ascii_alphanumeric())
}

/// Unicode::is_unicode_language_subtag.
pub fn is_unicode_language_subtag(subtag: Utf16View<'_>) -> bool {
    // unicode_language_subtag = alpha{2,3} | alpha{5,8}
    let length = subtag.length_in_code_units();
    if !(2..=8).contains(&length) || length == 4 {
        return false;
    }
    all_code_units(subtag, is_ascii_alpha)
}

/// Unicode::is_unicode_script_subtag.
pub fn is_unicode_script_subtag(subtag: Utf16View<'_>) -> bool {
    // unicode_script_subtag = alpha{4}
    if subtag.length_in_code_units() != 4 {
        return false;
    }
    all_code_units(subtag, is_ascii_alpha)
}

/// Unicode::is_unicode_region_subtag.
pub fn is_unicode_region_subtag(subtag: Utf16View<'_>) -> bool {
    // unicode_region_subtag = (alpha{2} | digit{3})
    match subtag.length_in_code_units() {
        2 => all_code_units(subtag, is_ascii_alpha),
        3 => all_code_units(subtag, is_ascii_digit),
        _ => false,
    }
}

/// Unicode::is_unicode_variant_subtag.
pub fn is_unicode_variant_subtag(subtag: Utf16View<'_>) -> bool {
    // unicode_variant_subtag = (alphanum{5,8} | digit alphanum{3})
    let length = subtag.length_in_code_units();
    if (5..=8).contains(&length) {
        return all_code_units(subtag, is_ascii_alphanumeric);
    }
    if length == 4 {
        return is_ascii_digit(subtag.code_unit_at(0))
            && all_code_units(subtag.substring_view(1, 3), is_ascii_alphanumeric);
    }
    false
}

/// Unicode::is_type_identifier(Utf16View).
pub fn is_type_identifier(identifier: Utf16View<'_>) -> bool {
    // SAFETY: The identifier is valid for its length.
    unsafe { unicode_is_type_identifier(UnicodeIntlText::of(identifier)) }
}

/// Unicode::canonicalize_unicode_locale_id(Utf16View).
pub fn canonicalize_unicode_locale_id(locale: Utf16View<'_>) -> Option<Utf16String> {
    // SAFETY: The locale is valid for its length, and the output writes into a Vec.
    optional_text_output(|output| unsafe {
        unicode_canonicalize_unicode_locale_id(UnicodeIntlText::of(locale), output)
    })
}

/// Unicode::canonicalize_unicode_extension_values.
pub fn canonicalize_unicode_extension_values(key: Utf16View<'_>, value: Utf16View<'_>) -> Utf16String {
    // SAFETY: The key and value are valid for their lengths, and the output writes into a Vec.
    text_output(|output| unsafe {
        unicode_canonicalize_unicode_extension_values(UnicodeIntlText::of(key), UnicodeIntlText::of(value), output);
    })
}

/// Unicode::default_locale.
pub fn default_locale() -> Utf16String {
    // SAFETY: The output writes into a Vec.
    text_output(|output| unsafe { unicode_default_locale(output) })
}

/// Unicode::is_locale_available.
pub fn is_locale_available(locale: Utf16View<'_>) -> bool {
    // SAFETY: The locale is valid for its length.
    unsafe { unicode_is_locale_available(UnicodeIntlText::of(locale)) }
}

/// Unicode::add_likely_subtags.
pub fn add_likely_subtags(locale: Utf16View<'_>) -> Option<Utf16String> {
    // SAFETY: The locale is valid for its length, and the output writes into a Vec.
    optional_text_output(|output| unsafe { unicode_add_likely_subtags(UnicodeIntlText::of(locale), output) })
}

/// Unicode::remove_likely_subtags.
pub fn remove_likely_subtags(locale: Utf16View<'_>) -> Option<Utf16String> {
    // SAFETY: The locale is valid for its length, and the output writes into a Vec.
    optional_text_output(|output| unsafe { unicode_remove_likely_subtags(UnicodeIntlText::of(locale), output) })
}

/// Unicode::is_locale_character_ordering_right_to_left.
pub fn is_locale_character_ordering_right_to_left(locale: Utf16View<'_>) -> bool {
    // SAFETY: The locale is valid for its length.
    unsafe { unicode_is_locale_character_ordering_right_to_left(UnicodeIntlText::of(locale)) }
}

/// Unicode::available_keyword_values.
pub fn available_keyword_values(locale: Utf16View<'_>, key: Utf16View<'_>) -> Vec<Utf16String> {
    // SAFETY: The locale and key are valid for their lengths, and the callback appends to a Vec.
    string_list_output(|context, append| unsafe {
        unicode_available_keyword_values(UnicodeIntlText::of(locale), UnicodeIntlText::of(key), context, append);
    })
}

/// Unicode::available_calendars().
pub fn available_calendars() -> Vec<Utf16String> {
    // SAFETY: The callback appends to a Vec.
    string_list_output(|context, append| unsafe { unicode_available_calendars(context, append) })
}

/// Unicode::available_calendars(locale).
pub fn available_calendars_of_locale(locale: Utf16View<'_>) -> Vec<Utf16String> {
    // SAFETY: The locale is valid for its length, and the callback appends to a Vec.
    string_list_output(|context, append| unsafe {
        unicode_available_calendars_of_locale(UnicodeIntlText::of(locale), context, append);
    })
}

/// Unicode::available_currencies().
pub fn available_currencies() -> Vec<Utf16String> {
    // SAFETY: The callback appends to a Vec.
    string_list_output(|context, append| unsafe { unicode_available_currencies(context, append) })
}

/// Unicode::available_collations().
pub fn available_collations() -> Vec<Utf16String> {
    // SAFETY: The callback appends to a Vec.
    string_list_output(|context, append| unsafe { unicode_available_collations(context, append) })
}

/// Unicode::available_collations(locale).
pub fn available_collations_of_locale(locale: Utf16View<'_>) -> Vec<Utf16String> {
    // SAFETY: The locale is valid for its length, and the callback appends to a Vec.
    string_list_output(|context, append| unsafe {
        unicode_available_collations_of_locale(UnicodeIntlText::of(locale), context, append);
    })
}

/// Unicode::available_hour_cycles(locale).
pub fn available_hour_cycles_of_locale(locale: Utf16View<'_>) -> Vec<Utf16String> {
    // SAFETY: The locale is valid for its length, and the callback appends to a Vec.
    string_list_output(|context, append| unsafe {
        unicode_available_hour_cycles_of_locale(UnicodeIntlText::of(locale), context, append);
    })
}

/// Unicode::available_number_systems().
pub fn available_number_systems() -> Vec<Utf16String> {
    // SAFETY: The callback appends to a Vec.
    string_list_output(|context, append| unsafe { unicode_available_number_systems(context, append) })
}

/// Unicode::available_number_systems(locale).
pub fn available_number_systems_of_locale(locale: Utf16View<'_>) -> Vec<Utf16String> {
    // SAFETY: The locale is valid for its length, and the callback appends to a Vec.
    string_list_output(|context, append| unsafe {
        unicode_available_number_systems_of_locale(UnicodeIntlText::of(locale), context, append);
    })
}

/// Unicode::available_time_zones_in_region.
pub fn available_time_zones_in_region(region: Utf16View<'_>) -> Vec<Utf16String> {
    // SAFETY: The region is valid for its length, and the callback appends to a Vec.
    string_list_output(|context, append| unsafe {
        unicode_available_time_zones_in_region(UnicodeIntlText::of(region), context, append);
    })
}

/// Unicode::Weekday.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Weekday {
    Sunday,
    Monday,
    Tuesday,
    Wednesday,
    Thursday,
    Friday,
    Saturday,
}

impl Weekday {
    fn from_u8(weekday: u8) -> Self {
        const WEEKDAYS: [Weekday; 7] = [
            Weekday::Sunday,
            Weekday::Monday,
            Weekday::Tuesday,
            Weekday::Wednesday,
            Weekday::Thursday,
            Weekday::Friday,
            Weekday::Saturday,
        ];
        WEEKDAYS[usize::from(weekday)]
    }
}

/// The fields of Unicode::WeekInfo the Intl builtins use.
pub struct WeekInfo {
    pub first_day_of_week: Option<Weekday>,
    pub weekend_days: Vec<Weekday>,
}

/// Unicode::week_info_of_locale.
pub fn week_info_of_locale(locale: Utf16View<'_>) -> WeekInfo {
    let mut has_first_day_of_week = false;
    let mut first_day_of_week = 0u8;
    let mut weekend_days = [0u8; 7];
    let mut weekend_day_count = 0usize;
    // SAFETY: The locale is valid for its length, and the export writes at most seven weekend days.
    unsafe {
        unicode_week_info_of_locale(
            UnicodeIntlText::of(locale),
            &raw mut has_first_day_of_week,
            &raw mut first_day_of_week,
            weekend_days.as_mut_ptr(),
            &raw mut weekend_day_count,
        );
    }
    WeekInfo {
        first_day_of_week: has_first_day_of_week.then(|| Weekday::from_u8(first_day_of_week)),
        weekend_days: weekend_days[..weekend_day_count]
            .iter()
            .map(|&day| Weekday::from_u8(day))
            .collect(),
    }
}

/// Unicode::Style.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Style {
    Long,
    Short,
    Narrow,
}

/// Unicode::style_from_string.
pub fn style_from_string(style: Utf16View<'_>) -> Style {
    if style == "narrow" {
        return Style::Narrow;
    }
    if style == "short" {
        return Style::Short;
    }
    if style == "long" {
        return Style::Long;
    }
    unreachable!("the style is one of the values GetOption allows")
}

/// Unicode::style_to_string.
pub fn style_to_string(style: Style) -> &'static str {
    match style {
        Style::Narrow => "narrow",
        Style::Short => "short",
        Style::Long => "long",
    }
}

/// Unicode::Usage.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Usage {
    Sort,
    Search,
}

/// Unicode::usage_from_string.
pub fn usage_from_string(usage: Utf16View<'_>) -> Usage {
    if usage == "sort" {
        return Usage::Sort;
    }
    if usage == "search" {
        return Usage::Search;
    }
    unreachable!("the usage is one of the values GetOption allows")
}

/// Unicode::usage_to_string.
pub fn usage_to_string(usage: Usage) -> &'static str {
    match usage {
        Usage::Sort => "sort",
        Usage::Search => "search",
    }
}

/// Unicode::Sensitivity.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum Sensitivity {
    Base,
    Accent,
    Case,
    Variant,
}

impl Sensitivity {
    fn from_u8(sensitivity: u8) -> Self {
        const SENSITIVITIES: [Sensitivity; 4] = [
            Sensitivity::Base,
            Sensitivity::Accent,
            Sensitivity::Case,
            Sensitivity::Variant,
        ];
        SENSITIVITIES[usize::from(sensitivity)]
    }
}

/// Unicode::sensitivity_from_string.
pub fn sensitivity_from_string(sensitivity: Utf16View<'_>) -> Sensitivity {
    if sensitivity == "base" {
        return Sensitivity::Base;
    }
    if sensitivity == "accent" {
        return Sensitivity::Accent;
    }
    if sensitivity == "case" {
        return Sensitivity::Case;
    }
    if sensitivity == "variant" {
        return Sensitivity::Variant;
    }
    unreachable!("the sensitivity is one of the values GetOption allows")
}

/// Unicode::sensitivity_to_string.
pub fn sensitivity_to_string(sensitivity: Sensitivity) -> &'static str {
    match sensitivity {
        Sensitivity::Base => "base",
        Sensitivity::Accent => "accent",
        Sensitivity::Case => "case",
        Sensitivity::Variant => "variant",
    }
}

/// Unicode::CaseFirst.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum CaseFirst {
    Upper,
    Lower,
    False,
}

/// Unicode::case_first_from_string.
pub fn case_first_from_string(case_first: Utf16View<'_>) -> CaseFirst {
    if case_first == "upper" {
        return CaseFirst::Upper;
    }
    if case_first == "lower" {
        return CaseFirst::Lower;
    }
    if case_first == "false" {
        return CaseFirst::False;
    }
    unreachable!("the case first value is one of the values ResolveLocale allows")
}

/// Unicode::case_first_to_string.
pub fn case_first_to_string(case_first: CaseFirst) -> &'static str {
    match case_first {
        CaseFirst::Upper => "upper",
        CaseFirst::Lower => "lower",
        CaseFirst::False => "false",
    }
}

/// Unicode::Collator::Order.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CollatorOrder {
    Before,
    Equal,
    After,
}

/// A Unicode::Collator, which this owns.
pub struct Collator {
    collator: NonNull<c_void>,
}

impl Collator {
    /// Unicode::Collator::create.
    pub fn create(
        locale: Utf16View<'_>,
        usage: Usage,
        collation: Utf16View<'_>,
        sensitivity: Option<Sensitivity>,
        case_first: CaseFirst,
        numeric: bool,
        ignore_punctuation: Option<bool>,
    ) -> Self {
        // SAFETY: The locale and collation are valid for their lengths.
        let collator = unsafe {
            unicode_collator_create(
                UnicodeIntlText::of(locale),
                usage as u8,
                UnicodeIntlText::of(collation),
                sensitivity.is_some(),
                sensitivity.map_or(0, |sensitivity| sensitivity as u8),
                case_first as u8,
                numeric,
                ignore_punctuation.is_some(),
                ignore_punctuation.unwrap_or(false),
            )
        };
        Self {
            collator: NonNull::new(collator).expect("Unicode::Collator::create returns a collator"),
        }
    }

    pub fn compare(&self, lhs: Utf16View<'_>, rhs: Utf16View<'_>) -> CollatorOrder {
        // SAFETY: The collator is alive, and the strings are valid for their lengths.
        let order = unsafe {
            unicode_collator_compare(
                self.collator.as_ptr(),
                UnicodeIntlText::of(lhs),
                UnicodeIntlText::of(rhs),
            )
        };
        match order {
            0 => CollatorOrder::Before,
            1 => CollatorOrder::Equal,
            _ => CollatorOrder::After,
        }
    }

    pub fn sensitivity(&self) -> Sensitivity {
        // SAFETY: The collator is alive.
        Sensitivity::from_u8(unsafe { unicode_collator_sensitivity(self.collator.as_ptr()) })
    }

    pub fn ignore_punctuation(&self) -> bool {
        // SAFETY: The collator is alive.
        unsafe { unicode_collator_ignore_punctuation(self.collator.as_ptr()) }
    }
}

impl Drop for Collator {
    fn drop(&mut self) {
        // SAFETY: This owns the collator, which nothing uses after it is dropped.
        unsafe { unicode_collator_destroy(self.collator.as_ptr()) };
    }
}

/// Unicode::ListFormatType.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum ListFormatType {
    Conjunction,
    Disjunction,
    Unit,
}

/// Unicode::list_format_type_from_string.
pub fn list_format_type_from_string(list_format_type: Utf16View<'_>) -> ListFormatType {
    if list_format_type == "conjunction" {
        return ListFormatType::Conjunction;
    }
    if list_format_type == "disjunction" {
        return ListFormatType::Disjunction;
    }
    if list_format_type == "unit" {
        return ListFormatType::Unit;
    }
    unreachable!("the list format type is one of the values GetOption allows")
}

/// Unicode::list_format_type_to_string.
pub fn list_format_type_to_string(list_format_type: ListFormatType) -> &'static str {
    match list_format_type {
        ListFormatType::Conjunction => "conjunction",
        ListFormatType::Disjunction => "disjunction",
        ListFormatType::Unit => "unit",
    }
}

/// Unicode::ListFormat::Partition.
pub struct ListFormatPartition {
    pub type_: Utf16String,
    pub value: Utf16String,
}

unsafe extern "C" fn append_list_format_part(
    context: *mut c_void,
    type_: *const u16,
    type_length: usize,
    value: *const u16,
    value_length: usize,
) {
    // SAFETY: The context is the list the caller passed, and the export passes valid code units for the lengths.
    let (parts, type_, value) = unsafe {
        (
            &mut *context.cast::<Vec<ListFormatPartition>>(),
            core::slice::from_raw_parts(type_, type_length),
            core::slice::from_raw_parts(value, value_length),
        )
    };
    parts.push(ListFormatPartition {
        type_: Utf16String::from_utf16(type_),
        value: Utf16String::from_utf16(value),
    });
}

/// A Unicode::ListFormat, which this owns.
pub struct ListFormat {
    list_format: NonNull<c_void>,
}

impl ListFormat {
    /// Unicode::ListFormat::create.
    pub fn create(locale: Utf16View<'_>, type_: ListFormatType, style: Style) -> Self {
        // SAFETY: The locale is valid for its length.
        let list_format = unsafe { unicode_list_format_create(UnicodeIntlText::of(locale), type_ as u8, style as u8) };
        Self {
            list_format: NonNull::new(list_format).expect("Unicode::ListFormat::create returns a list format"),
        }
    }

    pub fn format(&self, list: &[Utf16String]) -> Utf16String {
        let texts: Vec<UnicodeIntlText> = list
            .iter()
            .map(|string| UnicodeIntlText::of(Utf16View::of_string(string)))
            .collect();
        // SAFETY: The list format is alive, the texts borrow strings that outlive the call, and the output writes
        //         into a Vec.
        text_output(|output| unsafe {
            unicode_list_format_format(self.list_format.as_ptr(), texts.as_ptr(), texts.len(), output);
        })
    }

    pub fn format_to_parts(&self, list: &[Utf16String]) -> Vec<ListFormatPartition> {
        let texts: Vec<UnicodeIntlText> = list
            .iter()
            .map(|string| UnicodeIntlText::of(Utf16View::of_string(string)))
            .collect();
        let mut parts: Vec<ListFormatPartition> = Vec::new();
        // SAFETY: The list format is alive, the texts borrow strings that outlive the call, and the callback appends
        //         to a Vec.
        unsafe {
            unicode_list_format_format_to_parts(
                self.list_format.as_ptr(),
                texts.as_ptr(),
                texts.len(),
                (&raw mut parts).cast(),
                append_list_format_part,
            );
        }
        parts
    }
}

impl Drop for ListFormat {
    fn drop(&mut self) {
        // SAFETY: This owns the list format, which nothing uses after it is dropped.
        unsafe { unicode_list_format_destroy(self.list_format.as_ptr()) };
    }
}

/// Unicode::LanguageDisplay.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum LanguageDisplay {
    Standard,
    Dialect,
}

/// Unicode::language_display_from_string.
pub fn language_display_from_string(language_display: Utf16View<'_>) -> LanguageDisplay {
    if language_display == "standard" {
        return LanguageDisplay::Standard;
    }
    if language_display == "dialect" {
        return LanguageDisplay::Dialect;
    }
    unreachable!("the language display is one of the values GetOption allows")
}

/// Unicode::language_display_to_string.
pub fn language_display_to_string(language_display: LanguageDisplay) -> &'static str {
    match language_display {
        LanguageDisplay::Standard => "standard",
        LanguageDisplay::Dialect => "dialect",
    }
}

/// Unicode::language_display_name.
pub fn language_display_name(
    locale: Utf16View<'_>,
    language: Utf16View<'_>,
    language_display: LanguageDisplay,
) -> Option<Utf16String> {
    // SAFETY: The locale and language are valid for their lengths, and the output writes into a Vec.
    optional_text_output(|output| unsafe {
        unicode_language_display_name(
            UnicodeIntlText::of(locale),
            UnicodeIntlText::of(language),
            language_display as u8,
            output,
        )
    })
}

/// Unicode::region_display_name.
pub fn region_display_name(locale: Utf16View<'_>, region: Utf16View<'_>) -> Option<Utf16String> {
    // SAFETY: The locale and region are valid for their lengths, and the output writes into a Vec.
    optional_text_output(|output| unsafe {
        unicode_region_display_name(UnicodeIntlText::of(locale), UnicodeIntlText::of(region), output)
    })
}

/// Unicode::script_display_name.
pub fn script_display_name(locale: Utf16View<'_>, script: Utf16View<'_>) -> Option<Utf16String> {
    // SAFETY: The locale and script are valid for their lengths, and the output writes into a Vec.
    optional_text_output(|output| unsafe {
        unicode_script_display_name(UnicodeIntlText::of(locale), UnicodeIntlText::of(script), output)
    })
}

/// Unicode::currency_display_name.
pub fn currency_display_name(locale: Utf16View<'_>, currency: Utf16View<'_>, style: Style) -> Option<Utf16String> {
    // SAFETY: The locale and currency are valid for their lengths, and the output writes into a Vec.
    optional_text_output(|output| unsafe {
        unicode_currency_display_name(
            UnicodeIntlText::of(locale),
            UnicodeIntlText::of(currency),
            style as u8,
            output,
        )
    })
}

/// Unicode::calendar_display_name.
pub fn calendar_display_name(locale: Utf16View<'_>, calendar: Utf16View<'_>) -> Option<Utf16String> {
    // SAFETY: The locale and calendar are valid for their lengths, and the output writes into a Vec.
    optional_text_output(|output| unsafe {
        unicode_calendar_display_name(UnicodeIntlText::of(locale), UnicodeIntlText::of(calendar), output)
    })
}

/// Unicode::date_time_field_display_name.
pub fn date_time_field_display_name(locale: Utf16View<'_>, field: Utf16View<'_>, style: Style) -> Option<Utf16String> {
    // SAFETY: The locale and field are valid for their lengths, and the output writes into a Vec.
    optional_text_output(|output| unsafe {
        unicode_date_time_field_display_name(
            UnicodeIntlText::of(locale),
            UnicodeIntlText::of(field),
            style as u8,
            output,
        )
    })
}

/// Unicode::SegmenterGranularity.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum SegmenterGranularity {
    Grapheme,
    Line,
    Sentence,
    Word,
}

/// Unicode::segmenter_granularity_from_string.
pub fn segmenter_granularity_from_string(segmenter_granularity: Utf16View<'_>) -> SegmenterGranularity {
    if segmenter_granularity == "grapheme" {
        return SegmenterGranularity::Grapheme;
    }
    if segmenter_granularity == "line" {
        return SegmenterGranularity::Line;
    }
    if segmenter_granularity == "sentence" {
        return SegmenterGranularity::Sentence;
    }
    if segmenter_granularity == "word" {
        return SegmenterGranularity::Word;
    }
    unreachable!("the granularity is one of the values GetOption allows")
}

/// Unicode::segmenter_granularity_to_string.
pub fn segmenter_granularity_to_string(segmenter_granularity: SegmenterGranularity) -> &'static str {
    match segmenter_granularity {
        SegmenterGranularity::Grapheme => "grapheme",
        SegmenterGranularity::Line => "line",
        SegmenterGranularity::Sentence => "sentence",
        SegmenterGranularity::Word => "word",
    }
}

/// Unicode::Segmenter::Inclusive.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Inclusive {
    No,
    Yes,
}

/// A Unicode::Segmenter, which this owns. It keeps its own copy of the text it segments.
pub struct Segmenter {
    segmenter: NonNull<c_void>,
    segmenter_granularity: SegmenterGranularity,
}

impl Segmenter {
    /// Unicode::Segmenter::create(locale, granularity).
    pub fn create(locale: Utf16View<'_>, segmenter_granularity: SegmenterGranularity) -> Self {
        // SAFETY: The locale is valid for its length.
        let segmenter = unsafe { unicode_segmenter_create(UnicodeIntlText::of(locale), segmenter_granularity as u8) };
        Self {
            segmenter: NonNull::new(segmenter).expect("Unicode::Segmenter::create returns a segmenter"),
            segmenter_granularity,
        }
    }

    pub fn segmenter_granularity(&self) -> SegmenterGranularity {
        self.segmenter_granularity
    }

    pub fn clone_segmenter(&self) -> Self {
        // SAFETY: The segmenter is alive.
        let segmenter = unsafe { unicode_segmenter_clone(self.segmenter.as_ptr()) };
        Self {
            segmenter: NonNull::new(segmenter).expect("Unicode::Segmenter::clone returns a segmenter"),
            segmenter_granularity: self.segmenter_granularity,
        }
    }

    pub fn set_segmented_text(&mut self, text: Utf16View<'_>) {
        // SAFETY: The segmenter is alive, and the text is valid for its length; the segmenter copies it.
        unsafe { unicode_segmenter_set_segmented_text(self.segmenter.as_ptr(), UnicodeIntlText::of(text)) };
    }

    pub fn current_boundary(&mut self) -> usize {
        // SAFETY: The segmenter is alive.
        unsafe { unicode_segmenter_current_boundary(self.segmenter.as_ptr()) }
    }

    pub fn previous_boundary(&mut self, index: usize, inclusive: Inclusive) -> Option<usize> {
        let mut boundary = 0usize;
        // SAFETY: The segmenter is alive.
        let found = unsafe {
            unicode_segmenter_previous_boundary(
                self.segmenter.as_ptr(),
                index,
                inclusive == Inclusive::Yes,
                &raw mut boundary,
            )
        };
        found.then_some(boundary)
    }

    pub fn next_boundary(&mut self, index: usize, inclusive: Inclusive) -> Option<usize> {
        let mut boundary = 0usize;
        // SAFETY: The segmenter is alive.
        let found = unsafe {
            unicode_segmenter_next_boundary(
                self.segmenter.as_ptr(),
                index,
                inclusive == Inclusive::Yes,
                &raw mut boundary,
            )
        };
        found.then_some(boundary)
    }

    pub fn is_current_boundary_word_like(&self) -> bool {
        // SAFETY: The segmenter is alive.
        unsafe { unicode_segmenter_is_current_boundary_word_like(self.segmenter.as_ptr()) }
    }
}

impl Drop for Segmenter {
    fn drop(&mut self) {
        // SAFETY: This owns the segmenter, which nothing uses after it is dropped.
        unsafe { unicode_segmenter_destroy(self.segmenter.as_ptr()) };
    }
}

fn append_language_id_to_builder(builder: &mut Utf16StringBuilder, language_id: &LanguageID) {
    fn append_segment(builder: &mut Utf16StringBuilder, segment: &Utf16String) {
        if !builder.is_empty() {
            builder.append_ascii("-");
        }
        builder.append(Utf16View::of_string(segment));
    }

    for segment in [&language_id.language, &language_id.script, &language_id.region]
        .into_iter()
        .flatten()
    {
        append_segment(builder, segment);
    }
    for variant in &language_id.variants {
        append_segment(builder, variant);
    }
}

impl LanguageID {
    /// Unicode::LanguageID::to_utf16_string.
    pub fn to_utf16_string(&self) -> Utf16String {
        let mut builder = Utf16StringBuilder::new();
        append_language_id_to_builder(&mut builder, self);
        builder.to_utf16_string()
    }
}

impl LocaleID {
    /// Unicode::LocaleID::to_utf16_string.
    pub fn to_utf16_string(&self) -> Utf16String {
        fn append_segment(builder: &mut Utf16StringBuilder, segment: &Utf16String) {
            if segment.is_empty() {
                return;
            }
            if !builder.is_empty() {
                builder.append_ascii("-");
            }
            builder.append(Utf16View::of_string(segment));
        }

        let mut builder = Utf16StringBuilder::new();
        append_language_id_to_builder(&mut builder, &self.language_id);

        for extension in &self.extensions {
            match extension {
                Extension::Locale(extension) => {
                    builder.append_ascii("-u");
                    for attribute in &extension.attributes {
                        append_segment(&mut builder, attribute);
                    }
                    for keyword in &extension.keywords {
                        append_segment(&mut builder, &keyword.key);
                        append_segment(&mut builder, &keyword.value);
                    }
                }
                Extension::Transformed(extension) => {
                    builder.append_ascii("-t");
                    if let Some(language) = &extension.language {
                        append_language_id_to_builder(&mut builder, language);
                    }
                    for field in &extension.fields {
                        append_segment(&mut builder, &field.key);
                        append_segment(&mut builder, &field.value);
                    }
                }
                Extension::Other(extension) => {
                    builder.append_ascii("-");
                    builder.append_code_unit(u16::from(extension.key));
                    append_segment(&mut builder, &extension.value);
                }
            }
        }

        if !self.private_use_extensions.is_empty() {
            builder.append_ascii("-x");
            for extension in &self.private_use_extensions {
                append_segment(&mut builder, extension);
            }
        }

        builder.to_utf16_string()
    }

    /// Unicode::LocaleID::remove_extension_type<LocaleExtension>.
    pub fn remove_locale_extensions(&mut self) -> Vec<LocaleExtension> {
        let mut removed_extensions = Vec::new();
        let extensions = core::mem::take(&mut self.extensions);
        for extension in extensions {
            match extension {
                Extension::Locale(extension) => removed_extensions.push(extension),
                extension => self.extensions.push(extension),
            }
        }
        removed_extensions
    }

    /// The first extension for_each_extension_of_type<LocaleExtension> visits.
    pub fn first_locale_extension_mut(&mut self) -> Option<&mut LocaleExtension> {
        self.extensions.iter_mut().find_map(|extension| match extension {
            Extension::Locale(extension) => Some(extension),
            _ => None,
        })
    }
}
