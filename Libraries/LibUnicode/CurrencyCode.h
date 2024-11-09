/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/Span.h>
#include <AK/StringView.h>

namespace Unicode {

struct CurrencyCode {
    Optional<int> minor_unit {};
};

Optional<CurrencyCode> get_currency_code(StringView currency);

}
