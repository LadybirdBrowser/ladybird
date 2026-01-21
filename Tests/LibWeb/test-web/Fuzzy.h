/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Types.h>
#include <LibGfx/Forward.h>
#include <LibURL/URL.h>

namespace TestWeb {

struct FuzzyRange {
    u64 minimum_value;
    u64 maximum_value;

    bool contains(u64 value) const { return value >= minimum_value && value <= maximum_value; }
};

struct FuzzyMatch {
    Optional<URL::URL> reference;
    FuzzyRange color_value_error;
    FuzzyRange pixel_error_count;
};

bool fuzzy_screenshot_match(URL::URL const& test_url, URL::URL const& reference, Gfx::Bitmap const&,
    Gfx::Bitmap const&, ReadonlySpan<FuzzyMatch>, bool should_match);
ErrorOr<FuzzyMatch> parse_fuzzy_match(Optional<URL::URL const&> reference, String const&);
ErrorOr<FuzzyRange> parse_fuzzy_range(String const&);

}

namespace AK {

template<>
struct Formatter<TestWeb::FuzzyRange> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, TestWeb::FuzzyRange const& value)
    {
        return Formatter<FormatString>::format(builder, "FuzzyRange [{}-{}]"sv, value.minimum_value, value.maximum_value);
    }
};

}
