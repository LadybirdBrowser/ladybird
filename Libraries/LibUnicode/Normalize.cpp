/*
 * Copyright (c) 2022, mat
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibUnicode/ICU.h>
#include <LibUnicode/Normalize.h>
#include <LibUnicode/TextMapping.h>

#include <unicode/normalizer2.h>

namespace Unicode {

NormalizationForm normalization_form_from_string(StringView form)
{
    if (form == "NFD"sv)
        return NormalizationForm::NFD;
    if (form == "NFC"sv)
        return NormalizationForm::NFC;
    if (form == "NFKD"sv)
        return NormalizationForm::NFKD;
    if (form == "NFKC"sv)
        return NormalizationForm::NFKC;
    VERIFY_NOT_REACHED();
}

NormalizationForm normalization_form_from_string(Utf16View form)
{
    if (form == u"NFD"sv)
        return NormalizationForm::NFD;
    if (form == u"NFC"sv)
        return NormalizationForm::NFC;
    if (form == u"NFKD"sv)
        return NormalizationForm::NFKD;
    if (form == u"NFKC"sv)
        return NormalizationForm::NFKC;
    VERIFY_NOT_REACHED();
}

static icu::Normalizer2 const* normalizer_for_form(NormalizationForm form, UErrorCode& status)
{
    switch (form) {
    case NormalizationForm::NFD:
        return icu::Normalizer2::getNFDInstance(status);
    case NormalizationForm::NFC:
        return icu::Normalizer2::getNFCInstance(status);
    case NormalizationForm::NFKD:
        return icu::Normalizer2::getNFKDInstance(status);
    case NormalizationForm::NFKC:
        return icu::Normalizer2::getNFKCInstance(status);
    }
    VERIFY_NOT_REACHED();
}

String normalize(StringView string, NormalizationForm form)
{
    UErrorCode status = U_ZERO_ERROR;
    auto const* normalizer = normalizer_for_form(form, status);

    if (icu_failure(status))
        return MUST(String::from_utf8(string));

    VERIFY(normalizer);

    StringBuilder builder { string.length() };
    icu::StringByteSink sink { &builder };

    normalizer->normalizeUTF8(0, icu_string_piece(string), sink, nullptr, status);
    if (icu_failure(status))
        return MUST(String::from_utf8(string));

    return MUST(builder.to_string());
}

Utf16String normalize(Utf16View string, NormalizationForm form)
{
    UErrorCode status = U_ZERO_ERROR;
    auto const* normalizer = normalizer_for_form(form, status);

    if (icu_failure(status))
        return Utf16String::from_utf16(string);

    VERIFY(normalizer);

    auto icu_input = icu_string(string);
    UErrorCode normalize_status = U_ZERO_ERROR;
    auto icu_output = normalizer->normalize(icu_input, normalize_status);
    if (icu_failure(normalize_status))
        return Utf16String::from_utf16(string);

    return icu_string_to_utf16_string(icu_output);
}

}

extern "C" void unicode_normalize(u16 const* text, size_t length, u8 form, UnicodeTextMappingOutput output)
{
    VERIFY(form <= to_underlying(Unicode::NormalizationForm::NFKC));
    auto normalized = Unicode::normalize(Utf16View { reinterpret_cast<char16_t const*>(text), length }, static_cast<Unicode::NormalizationForm>(form));
    auto view = normalized.utf16_view();
    auto* destination = output.allocate_text(output.context, view.length_in_code_units());
    for (size_t i = 0; i < view.length_in_code_units(); ++i)
        destination[i] = view.code_unit_at(i);
}
