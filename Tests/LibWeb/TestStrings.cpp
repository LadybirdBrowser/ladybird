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
    EXPECT(Web::Infra::is_code_unit_prefix("abc"_sv, "abcde"_sv));

    // Exact match
    EXPECT(Web::Infra::is_code_unit_prefix("abc"_sv, "abc"_sv));

    // Empty prefix
    EXPECT(Web::Infra::is_code_unit_prefix(""_sv, "abc"_sv));

    // Empty input string
    EXPECT(!Web::Infra::is_code_unit_prefix("abc"_sv, ""_sv));

    // Both strings empty
    EXPECT(Web::Infra::is_code_unit_prefix(""_sv, ""_sv));

    // Prefix longer than input string
    EXPECT(!Web::Infra::is_code_unit_prefix("abcdef"_sv, "abc"_sv));

    // Non-ASCII characters
    EXPECT(Web::Infra::is_code_unit_prefix("こんにちは"_sv, "こんにちは世界"_sv));
    EXPECT(!Web::Infra::is_code_unit_prefix("世界"_sv, "こんにちは世界"_sv));

    EXPECT(Web::Infra::is_code_unit_prefix("こ"_sv, "こん"_sv));
    EXPECT(!Web::Infra::is_code_unit_prefix("こん"_sv, "こ"_sv));

    // Special characters
    EXPECT(Web::Infra::is_code_unit_prefix("!@#"_sv, "!@#$%^"_sv));
    EXPECT(!Web::Infra::is_code_unit_prefix("!@#$"_sv, "!@#"_sv));

    // Case sensitivity
    EXPECT(!Web::Infra::is_code_unit_prefix("abc"_sv, "ABC"_sv));
    EXPECT(!Web::Infra::is_code_unit_prefix("ABC"_sv, "abc"_sv));
}
