/*
 * Copyright (c) 2021-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <AK/String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibJS/Forward.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Date.h>
#include <LibJS/Runtime/Value.h>
#include <LibUnicode/Locale.h>

namespace JS::Intl {

using LocaleKey = Variant<Empty, String>;
Optional<LocaleKey> locale_key_from_value(Value);

struct LocaleOptions {
    Value locale_matcher;
    Optional<LocaleKey> ca; // [[Calendar]]
    Optional<LocaleKey> co; // [[Collation]]
    Optional<LocaleKey> hc; // [[HourCycle]]
    Optional<LocaleKey> kf; // [[CaseFirst]]
    Optional<LocaleKey> kn; // [[Numeric]]
    Optional<LocaleKey> nu; // [[NumberingSystem]]
};

struct MatchedLocale {
    String locale;
    Optional<Unicode::Extension> extension;
};

struct ResolvedLocale {
    String locale;
    LocaleKey ca; // [[Calendar]]
    LocaleKey co; // [[Collation]]
    LocaleKey hc; // [[HourCycle]]
    LocaleKey kf; // [[CaseFirst]]
    LocaleKey kn; // [[Numeric]]
    LocaleKey nu; // [[NumberingSystem]]
};

using StringOrBoolean = Variant<StringView, bool>;

bool is_structurally_valid_language_tag(StringView locale);
String canonicalize_unicode_locale_id(StringView locale);
bool is_well_formed_currency_code(StringView currency);
Vector<TimeZoneIdentifier> const& available_named_time_zone_identifiers();
Optional<TimeZoneIdentifier const&> get_available_named_time_zone_identifier(StringView time_zone_identifier);
bool is_well_formed_unit_identifier(StringView unit_identifier);
ThrowCompletionOr<Vector<String>> canonicalize_locale_list(VM&, Value locales);
Optional<MatchedLocale> lookup_matching_locale_by_prefix(ReadonlySpan<String> requested_locales);
Optional<MatchedLocale> lookup_matching_locale_by_best_fit(ReadonlySpan<String> requested_locales);
String insert_unicode_extension_and_canonicalize(Unicode::LocaleID locale_id, Vector<String> attributes, Vector<Unicode::Keyword> keywords);
ResolvedLocale resolve_locale(ReadonlySpan<String> requested_locales, LocaleOptions const& options, ReadonlySpan<StringView> relevant_extension_keys);
ThrowCompletionOr<Array*> filter_locales(VM& vm, ReadonlySpan<String> requested_locales, Value options);
ThrowCompletionOr<Object*> coerce_options_to_object(VM&, Value options);
ThrowCompletionOr<StringOrBoolean> get_boolean_or_string_number_format_option(VM& vm, Object const& options, PropertyKey const& property, ReadonlySpan<StringView> string_values, StringOrBoolean fallback);
ThrowCompletionOr<Optional<int>> default_number_option(VM&, Value value, int minimum, int maximum, Optional<int> fallback);
ThrowCompletionOr<Optional<int>> get_number_option(VM&, Object const& options, PropertyKey const& property, int minimum, int maximum, Optional<int> fallback);

template<size_t Size>
ThrowCompletionOr<StringOrBoolean> get_boolean_or_string_number_format_option(VM& vm, Object const& options, PropertyKey const& property, StringView const (&string_values)[Size], StringOrBoolean fallback)
{
    return get_boolean_or_string_number_format_option(vm, options, property, ReadonlySpan<StringView> { string_values }, move(fallback));
}

}
