/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "Fuzzy.h"

#include <AK/Enumerate.h>
#include <AK/Format.h>
#include <LibGfx/Bitmap.h>

namespace TestWeb {

// https://web-platform-tests.org/writing-tests/reftests.html#fuzzy-matching
bool fuzzy_screenshot_match(URL::URL const& test_url, URL::URL const& reference, Gfx::Bitmap const& bitmap_a, Gfx::Bitmap const& bitmap_b,
    Vector<FuzzyMatch> const& fuzzy_matches)
{
    if (bitmap_a.width() != bitmap_b.width() || bitmap_a.height() != bitmap_b.height())
        return false;

    // If the bitmaps are identical, we don't perform fuzzy matching.
    auto diff = bitmap_a.diff(bitmap_b);
    if (diff.identical)
        return true;

    // Find a single fuzzy config to apply.
    auto fuzzy_match = fuzzy_matches.first_matching([&reference](FuzzyMatch const& fuzzy_match) {
        if (fuzzy_match.reference.has_value())
            return fuzzy_match.reference.value().equals(reference);
        return true;
    });
    if (!fuzzy_match.has_value()) {
        warnln("{}: Screenshot mismatch: pixel error count {}, with maximum error {}. (No fuzzy config defined)", test_url, diff.pixel_error_count, diff.maximum_error);
        return false;
    }

    // Apply fuzzy matching.
    auto color_error_matches = fuzzy_match->color_value_error.contains(diff.maximum_error);
    if (!color_error_matches)
        warnln("{}: Fuzzy mismatch: maximum error {} is outside {}", test_url, diff.maximum_error, fuzzy_match->color_value_error);

    auto pixel_error_matches = fuzzy_match->pixel_error_count.contains(diff.pixel_error_count);
    if (!pixel_error_matches)
        warnln("{}: Fuzzy mismatch: pixel error count {} is outside {}", test_url, diff.pixel_error_count, fuzzy_match->pixel_error_count);

    return color_error_matches && pixel_error_matches;
}

// https://web-platform-tests.org/writing-tests/reftests.html#fuzzy-matching
ErrorOr<FuzzyRange> parse_fuzzy_range(String const& fuzzy_range)
{
    auto range_parts = MUST(fuzzy_range.split('-'));
    if (range_parts.is_empty() || range_parts.size() > 2)
        return Error::from_string_view("Invalid fuzzy range format"sv);

    auto parse_value = [](String const& value) -> ErrorOr<u64> {
        auto maybe_value = value.to_number<u64>();
        if (!maybe_value.has_value())
            return Error::from_string_view("Fuzzy range value is not a valid integer"sv);
        return maybe_value.release_value();
    };
    auto minimum_value = TRY(parse_value(range_parts[0]));
    auto maximum_value = minimum_value;
    if (range_parts.size() == 2)
        maximum_value = TRY(parse_value(range_parts[1]));

    if (minimum_value > maximum_value)
        return Error::from_string_view("Fuzzy range minimum is higher than its maximum"sv);

    return FuzzyRange { .minimum_value = minimum_value, .maximum_value = maximum_value };
}

// https://web-platform-tests.org/writing-tests/reftests.html#fuzzy-matching
ErrorOr<FuzzyMatch> parse_fuzzy_match(Optional<URL::URL const&> reference, String const& content)
{
    // Match configuration values. Two formats are supported:
    // * Named: "maxDifference=(#X-)#Y;totalPixels=(#X-)#Y"
    // * Unnamed: "(#X-)#Y;(#X-)#Y" (maxDifference and totalPixels are assumed in this order)
    auto config_parts = MUST(content.split(';'));
    if (config_parts.size() != 2)
        return Error::from_string_view("Fuzzy configuration must have exactly two parameters"sv);

    Optional<FuzzyRange> color_value_error;
    Optional<FuzzyRange> pixel_error_count;
    for (auto [i, config_part] : enumerate(config_parts)) {
        auto named_parts = MUST(MUST(config_part.trim_ascii_whitespace()).split_limit('=', 2));
        if (named_parts.is_empty())
            return Error::from_string_view("Fuzzy configuration value cannot be empty"sv);

        if (named_parts.size() == 2) {
            if (named_parts[0] == "maxDifference"sv && !color_value_error.has_value())
                color_value_error = TRY(parse_fuzzy_range(named_parts[1]));
            else if (named_parts[0] == "totalPixels"sv && !pixel_error_count.has_value())
                pixel_error_count = TRY(parse_fuzzy_range(named_parts[1]));
            else
                return Error::from_string_view("Invalid fuzzy configuration parameter"sv);
        } else {
            if (i == 0 && !color_value_error.has_value())
                color_value_error = TRY(parse_fuzzy_range(config_part));
            else if (i == 1 && !pixel_error_count.has_value())
                pixel_error_count = TRY(parse_fuzzy_range(config_part));
            else
                return Error::from_string_view("Invalid fuzzy configuration parameter"sv);
        }
    }

    return FuzzyMatch {
        .reference = reference.map([](URL::URL const& reference) { return reference; }),
        .color_value_error = color_value_error.release_value(),
        .pixel_error_count = pixel_error_count.release_value(),
    };
}

}
