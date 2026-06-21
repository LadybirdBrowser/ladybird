/*
 * Copyright (c) 2021-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/EnumBits.h>
#include <AK/Span.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/Value.h>
#include <LibUnicode/Locale.h>

namespace JS::Intl {

using LocaleKey = Variant<Empty, Utf16String>;
using ResolvedLocaleKey = Variant<Empty, Utf16String>;

struct LocaleOptions {
    Value locale_matcher;
    Optional<LocaleKey> ca; // [[Calendar]]
    Optional<LocaleKey> co; // [[Collation]]
    Optional<LocaleKey> hc; // [[HourCycle]]
    Optional<LocaleKey> kf; // [[CaseFirst]]
    Optional<LocaleKey> kn; // [[Numeric]]
    Optional<LocaleKey> nu; // [[NumberingSystem]]
    Value hour12;
};

struct MatchedLocale {
    Utf16String locale;
    Optional<Unicode::Extension> extension;
};

struct ResolvedLocale {
    Utf16String locale;
    Utf16String icu_locale;
    ResolvedLocaleKey ca; // [[Calendar]]
    ResolvedLocaleKey co; // [[Collation]]
    ResolvedLocaleKey hc; // [[HourCycle]]
    ResolvedLocaleKey kf; // [[CaseFirst]]
    ResolvedLocaleKey kn; // [[Numeric]]
    ResolvedLocaleKey nu; // [[NumberingSystem]]
};

struct ResolvedOptions {
    GC::Ref<Object> options;
    ResolvedLocale resolved_locale;
    LocaleOptions resolution_options;
};

enum class SpecialBehaviors : u8 {
    None = 0,
    RequireOptions = 1 << 1,
    CoerceOptions = 1 << 2,
};
AK_ENUM_BITWISE_OPERATORS(SpecialBehaviors);

using StringOrBoolean = Variant<StringView, bool>;

bool is_well_formed_language_tag(Utf16View locale);
Utf16String canonicalize_unicode_locale_id(Utf16View locale);
bool is_well_formed_currency_code(Utf16View currency);
Vector<TimeZoneIdentifier> const& available_named_time_zone_identifiers();
Optional<TimeZoneIdentifier const&> get_available_named_time_zone_identifier(Utf16View time_zone_identifier);
bool is_well_formed_unit_identifier(Utf16View unit_identifier);
ThrowCompletionOr<Vector<Utf16String>> canonicalize_locale_list(VM&, Value locales);
Optional<MatchedLocale> lookup_matching_locale_by_prefix(ReadonlySpan<Utf16String> requested_locales);
Optional<MatchedLocale> lookup_matching_locale_by_best_fit(ReadonlySpan<Utf16String> requested_locales);
Utf16String insert_unicode_extension_and_canonicalize(Unicode::LocaleID locale_id, Vector<Utf16String> attributes, Vector<Unicode::Keyword> keywords);
ResolvedLocale resolve_locale(ReadonlySpan<Utf16String> requested_locales, LocaleOptions const& options, ReadonlySpan<Utf16View> relevant_extension_keys);
ThrowCompletionOr<ResolvedOptions> resolve_options(VM& vm, IntlObject& object, Value locales, Value options_value, SpecialBehaviors special_behaviours = SpecialBehaviors::None, Function<void(LocaleOptions&)> modify_resolution_options = {});
ThrowCompletionOr<GC::Ref<Array>> filter_locales(VM& vm, ReadonlySpan<Utf16String> requested_locales, Value options);
ThrowCompletionOr<GC::Ref<Object>> coerce_options_to_object(VM&, Value options);
ThrowCompletionOr<StringOrBoolean> get_boolean_or_string_number_format_option(VM& vm, Object const& options, PropertyKey const& property, ReadonlySpan<StringView> string_values, StringOrBoolean fallback);
ThrowCompletionOr<Optional<int>> default_number_option(VM&, Value value, int minimum, int maximum, Optional<int> fallback);
ThrowCompletionOr<Optional<int>> get_number_option(VM&, Object const& options, PropertyKey const& property, int minimum, int maximum, Optional<int> fallback);

template<size_t Size>
ThrowCompletionOr<StringOrBoolean> get_boolean_or_string_number_format_option(VM& vm, Object const& options, PropertyKey const& property, StringView const (&string_values)[Size], StringOrBoolean fallback)
{
    return get_boolean_or_string_number_format_option(vm, options, property, ReadonlySpan<StringView> { string_values }, move(fallback));
}

}
