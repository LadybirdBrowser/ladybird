/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWeb/Infra/Strings.h>

TEST_CASE(is_code_unit_prefix)
{

    // Basic prefix match
    EXPECT(Web::Infra::is_code_unit_prefix("abc"sv, "abcde"sv));

    // Exact match
    EXPECT(Web::Infra::is_code_unit_prefix("abc"sv, "abc"sv));

    // Empty prefix
    EXPECT(Web::Infra::is_code_unit_prefix(""sv, "abc"sv));

    // Empty input string
    EXPECT(!Web::Infra::is_code_unit_prefix("abc"sv, ""sv));

    // Both strings empty
    EXPECT(Web::Infra::is_code_unit_prefix(""sv, ""sv));

    // Prefix longer than input string
    EXPECT(!Web::Infra::is_code_unit_prefix("abcdef"sv, "abc"sv));

    // Non-ASCII characters
    EXPECT(Web::Infra::is_code_unit_prefix("こんにちは"sv, "こんにちは世界"sv));
    EXPECT(!Web::Infra::is_code_unit_prefix("世界"sv, "こんにちは世界"sv));

    EXPECT(Web::Infra::is_code_unit_prefix("こ"sv, "こん"sv));
    EXPECT(!Web::Infra::is_code_unit_prefix("こん"sv, "こ"sv));

    // Special characters
    EXPECT(Web::Infra::is_code_unit_prefix("!@#"sv, "!@#$%^"sv));
    EXPECT(!Web::Infra::is_code_unit_prefix("!@#$"sv, "!@#"sv));

    // Case sensitivity
    EXPECT(!Web::Infra::is_code_unit_prefix("abc"sv, "ABC"sv));
    EXPECT(!Web::Infra::is_code_unit_prefix("ABC"sv, "abc"sv));
}
