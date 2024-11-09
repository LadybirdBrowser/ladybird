/*
 * Copyright (c) 2023, Simon Wanner <simon@skyrising.xyz>
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibUnicode/ICU.h>
#include <LibUnicode/IDNA.h>

#include <unicode/idna.h>

namespace Unicode::IDNA {

// https://www.unicode.org/reports/tr46/#ToASCII
ErrorOr<String> to_ascii(Utf8View domain_name, ToAsciiOptions const& options)
{
    u32 icu_options = UIDNA_DEFAULT;

    if (options.check_bidi == CheckBidi::Yes)
        icu_options |= UIDNA_CHECK_BIDI;
    if (options.check_joiners == CheckJoiners::Yes)
        icu_options |= UIDNA_CHECK_CONTEXTJ;
    if (options.use_std3_ascii_rules == UseStd3AsciiRules::Yes)
        icu_options |= UIDNA_USE_STD3_RULES;
    if (options.transitional_processing == TransitionalProcessing::No)
        icu_options |= UIDNA_NONTRANSITIONAL_TO_ASCII | UIDNA_NONTRANSITIONAL_TO_UNICODE;

    UErrorCode status = U_ZERO_ERROR;

    auto idna = adopt_own_if_nonnull(icu::IDNA::createUTS46Instance(icu_options, status));
    if (icu_failure(status))
        return Error::from_string_literal("Unable to create an IDNA instance");

    StringBuilder builder { domain_name.as_string().length() };
    icu::StringByteSink sink { &builder };

    icu::IDNAInfo info;
    idna->nameToASCII_UTF8(icu_string_piece(domain_name.as_string()), sink, info, status);

    auto errors = info.getErrors();

    if (options.check_hyphens == CheckHyphens::No) {
        errors &= ~UIDNA_ERROR_HYPHEN_3_4;
        errors &= ~UIDNA_ERROR_LEADING_HYPHEN;
        errors &= ~UIDNA_ERROR_TRAILING_HYPHEN;
    }
    if (options.verify_dns_length == VerifyDnsLength::No) {
        errors &= ~UIDNA_ERROR_EMPTY_LABEL;
        errors &= ~UIDNA_ERROR_LABEL_TOO_LONG;
        errors &= ~UIDNA_ERROR_DOMAIN_NAME_TOO_LONG;
    }

    if (icu_failure(status) || errors != 0)
        return Error::from_string_literal("Unable to convert domain to ASCII");

    return builder.to_string();
}

}
