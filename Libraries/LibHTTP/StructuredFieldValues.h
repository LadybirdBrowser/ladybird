/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Variant.h>
#include <AK/Vector.h>

// Structured Field Values for HTTP, https://www.rfc-editor.org/rfc/rfc8941
namespace HTTP::StructuredFieldValues {

// https://www.rfc-editor.org/rfc/rfc8941#section-3.3.4
struct Token {
    String value;
    bool operator==(Token const&) const = default;
};

// https://www.rfc-editor.org/rfc/rfc8941#section-3.3.5
struct ByteSequence {
    ByteBuffer value;
    bool operator==(ByteSequence const&) const = default;
};

// https://www.rfc-editor.org/rfc/rfc8941#section-3.3
using BareItem = Variant<i64, double, String, Token, ByteSequence, bool>;

// https://www.rfc-editor.org/rfc/rfc8941#section-3.1.2
struct Parameter {
    String key;
    BareItem value;
};

struct Parameters {
    Vector<Parameter> entries;

    Optional<BareItem const&> get(StringView key) const;
    void set(String key, BareItem value);
};

// https://www.rfc-editor.org/rfc/rfc8941#section-3.3
struct Item {
    BareItem value;
    Parameters parameters;

    // Convenience for the common case of a header whose value is expected to be a token.
    Optional<StringView> token() const;
};

// https://www.rfc-editor.org/rfc/rfc8941#section-4.2
// Parses `input` as a structured field value of type "item", returning nothing on failure.
Optional<Item> parse_item(StringView input);

}
