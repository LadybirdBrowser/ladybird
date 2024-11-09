/*
 * Copyright (c) 2023, Simon Wanner <simon@skyrising.xyz>
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/String.h>

namespace Unicode::IDNA {

enum class CheckHyphens {
    No,
    Yes,
};

enum class CheckBidi {
    No,
    Yes,
};

enum class CheckJoiners {
    No,
    Yes,
};

enum class UseStd3AsciiRules {
    No,
    Yes,
};

enum class TransitionalProcessing {
    No,
    Yes,
};

enum class VerifyDnsLength {
    No,
    Yes,
};

struct ToAsciiOptions {
    CheckHyphens check_hyphens { CheckHyphens::Yes };
    CheckBidi check_bidi { CheckBidi::Yes };
    CheckJoiners check_joiners { CheckJoiners::Yes };
    UseStd3AsciiRules use_std3_ascii_rules { UseStd3AsciiRules::No };
    TransitionalProcessing transitional_processing { TransitionalProcessing::No };
    VerifyDnsLength verify_dns_length { VerifyDnsLength::Yes };
};

ErrorOr<String> to_ascii(Utf8View domain_name, ToAsciiOptions const& = {});

}
