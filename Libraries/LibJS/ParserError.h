/*
 * Copyright (c) 2020, Stephan Unverwerth <s.unverwerth@serenityos.org>
 * Copyright (c) 2021-2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <LibJS/SourceRange.h>

namespace JS {

struct ParserError {
    ByteString message;
    Optional<Position> position;

    String to_string() const;
    ByteString to_byte_string() const;
    ByteString source_location_hint(StringView source, char const spacer = ' ', char const indicator = '^') const;
};

}
