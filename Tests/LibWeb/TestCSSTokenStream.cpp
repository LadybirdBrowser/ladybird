/*
 * Copyright (c) 2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/FlyString.h>
#include <AK/Vector.h>
#include <LibTest/TestCase.h>
#include <LibWeb/CSS/Parser/TokenStream.h>

namespace Web::CSS::Parser {

TEST_CASE(basic)
{
    Vector<Token> tokens {
        Token::create_ident("hello"_fly_string),
    };

    TokenStream stream { tokens };
    EXPECT(!stream.is_empty());
    EXPECT(stream.has_next_token());
    EXPECT_EQ(stream.remaining_token_count(), 1u);

    // next_token() doesn't consume it
    auto const& next = stream.next_token();
    EXPECT(!stream.is_empty());
    EXPECT(stream.has_next_token());
    EXPECT_EQ(stream.remaining_token_count(), 1u);
    // Check what the token is
    EXPECT(next.is(Token::Type::Ident));
    EXPECT_EQ(next.ident(), "hello"_fly_string);

    // consume_a_token() does consume it
    auto const& consumed = stream.consume_a_token();
    EXPECT(stream.is_empty());
    EXPECT(!stream.has_next_token());
    EXPECT_EQ(stream.remaining_token_count(), 0u);
    // Check what the token is
    EXPECT(consumed.is(Token::Type::Ident));
    EXPECT_EQ(consumed.ident(), "hello"_fly_string);

    // Now, any further tokens should be EOF
    EXPECT(stream.next_token().is(Token::Type::EndOfFile));
    EXPECT(stream.consume_a_token().is(Token::Type::EndOfFile));
}

TEST_CASE(marks)
{
    Vector<Token> tokens {
        Token::create_ident("a"_fly_string),
        Token::create_ident("b"_fly_string),
        Token::create_ident("c"_fly_string),
        Token::create_ident("d"_fly_string),
        Token::create_ident("e"_fly_string),
        Token::create_ident("f"_fly_string),
        Token::create_ident("g"_fly_string),
    };
    TokenStream stream { tokens };

    stream.mark(); // 0

    EXPECT_EQ(stream.remaining_token_count(), 7u);

    stream.discard_a_token();
    stream.discard_a_token();
    stream.discard_a_token();

    EXPECT_EQ(stream.remaining_token_count(), 4u);

    stream.mark(); // 3

    stream.discard_a_token();

    EXPECT_EQ(stream.remaining_token_count(), 3u);

    stream.restore_a_mark(); // Back to 3

    EXPECT_EQ(stream.remaining_token_count(), 4u);

    stream.discard_a_token();
    stream.discard_a_token();
    stream.discard_a_token();

    EXPECT_EQ(stream.remaining_token_count(), 1u);

    stream.mark(); // 6

    stream.discard_a_mark();

    EXPECT_EQ(stream.remaining_token_count(), 1u);

    stream.restore_a_mark(); // Back to 0

    EXPECT_EQ(stream.remaining_token_count(), 7u);
}

}
