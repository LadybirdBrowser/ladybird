/*
 * Copyright (c) 2021, Max Wipfli <max.wipfli@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <LibCore/File.h>
#include <LibWeb/HTML/Parser/HTMLTokenizer.h>

using Tokenizer = Web::HTML::HTMLTokenizer;
using Token = Web::HTML::HTMLToken;

#define BEGIN_ENUMERATION(tokens)          \
    auto current_token = (tokens).begin(); \
    [[maybe_unused]] Token* last_token;

#define END_ENUMERATION() \
    EXPECT(current_token.is_end());

#define NEXT_TOKEN()              \
    last_token = &*current_token; \
    ++current_token;

#define EXPECT_START_TAG_TOKEN(_tag_name, start_column, end_column)  \
    EXPECT_EQ(current_token->type(), Token::Type::StartTag);         \
    EXPECT_EQ(current_token->tag_name(), #_tag_name);                \
    EXPECT_EQ(current_token->start_position().column, start_column); \
    EXPECT_EQ(current_token->end_position().column, end_column);     \
    NEXT_TOKEN();

#define EXPECT_END_TAG_TOKEN(_tag_name, start_column, end_column)    \
    EXPECT_EQ(current_token->type(), Token::Type::EndTag);           \
    EXPECT_EQ(current_token->tag_name(), #_tag_name);                \
    EXPECT_EQ(current_token->start_position().column, start_column); \
    EXPECT_EQ(current_token->end_position().column, end_column);     \
    NEXT_TOKEN();

#define EXPECT_END_OF_FILE_TOKEN()                            \
    EXPECT_EQ(current_token->type(), Token::Type::EndOfFile); \
    NEXT_TOKEN();

#define EXPECT_CHARACTER_TOKEN(character)                     \
    EXPECT_EQ(current_token->type(), Token::Type::Character); \
    EXPECT_EQ(current_token->code_point(), (u32)(character)); \
    NEXT_TOKEN();

#define EXPECT_CHARACTER_TOKENS(string) \
    for (auto c : #string##sv) {        \
        EXPECT_CHARACTER_TOKEN(c);      \
    }

#define EXPECT_COMMENT_TOKEN()                              \
    EXPECT_EQ(current_token->type(), Token::Type::Comment); \
    NEXT_TOKEN();

#define EXPECT_DOCTYPE_TOKEN()                              \
    EXPECT_EQ(current_token->type(), Token::Type::DOCTYPE); \
    NEXT_TOKEN();

#define EXPECT_TAG_TOKEN_ATTRIBUTE(name, attribute_value, name_start_column, name_end_column, value_start_column, value_end_column) \
    VERIFY(last_token);                                                                                                             \
    auto name##_attr = last_token->raw_attribute(#name##_fly_string);                                                               \
    VERIFY(name##_attr.has_value());                                                                                                \
    EXPECT_EQ(name##_attr->value, attribute_value);                                                                                 \
    EXPECT_EQ(name##_attr->name_start_position.column, name_start_column);                                                          \
    EXPECT_EQ(name##_attr->name_end_position.column, name_end_column);                                                              \
    EXPECT_EQ(name##_attr->value_start_position.column, value_start_column);                                                        \
    EXPECT_EQ(name##_attr->value_end_position.column, value_end_column);

#define EXPECT_TAG_TOKEN_ATTRIBUTE_COUNT(count) \
    VERIFY(last_token);                         \
    EXPECT_EQ(last_token->attribute_count(), (size_t)(count));

static Vector<Token> run_tokenizer(StringView input)
{
    Vector<Token> tokens;
    Tokenizer tokenizer { input, "UTF-8"sv };
    while (true) {
        auto maybe_token = tokenizer.next_token();
        if (!maybe_token.has_value())
            break;
        tokens.append(maybe_token.release_value());
    }
    return tokens;
}

// FIXME: It's not very nice to rely on the format of HTMLToken::to_string() to stay the same.
static u32 hash_tokens(Vector<Token> const& tokens)
{
    StringBuilder builder;
    for (auto& token : tokens)
        builder.append(token.to_string());
    return (u32)builder.string_view().hash();
}

TEST_CASE(empty)
{
    auto tokens = run_tokenizer(""sv);
    BEGIN_ENUMERATION(tokens);
    EXPECT_END_OF_FILE_TOKEN();
    END_ENUMERATION();
}

TEST_CASE(basic)
{
    auto tokens = run_tokenizer("<html><head></head><body></body></html>"sv);
    BEGIN_ENUMERATION(tokens);
    EXPECT_START_TAG_TOKEN(html, 1u, 5u);
    EXPECT_START_TAG_TOKEN(head, 7u, 11u);
    EXPECT_END_TAG_TOKEN(head, 14u, 18u);
    EXPECT_START_TAG_TOKEN(body, 20u, 24u);
    EXPECT_END_TAG_TOKEN(body, 27u, 31u);
    EXPECT_END_TAG_TOKEN(html, 34u, 38u);
    EXPECT_END_OF_FILE_TOKEN();
    END_ENUMERATION();
}

TEST_CASE(basic_with_text)
{
    auto tokens = run_tokenizer("<p>This is some text.</p>"sv);
    BEGIN_ENUMERATION(tokens);
    EXPECT_START_TAG_TOKEN(p, 1u, 2u);
    EXPECT_CHARACTER_TOKENS(This is some text.);
    EXPECT_END_TAG_TOKEN(p, 23u, 24u);
    EXPECT_END_OF_FILE_TOKEN();
    END_ENUMERATION();
}

TEST_CASE(unquoted_attributes)
{
    auto tokens = run_tokenizer("<p foo=bar>"sv);
    BEGIN_ENUMERATION(tokens);
    EXPECT_START_TAG_TOKEN(p, 1u, 10u);
    EXPECT_TAG_TOKEN_ATTRIBUTE_COUNT(1);
    EXPECT_TAG_TOKEN_ATTRIBUTE(foo, "bar", 3u, 6u, 7u, 10u);
    EXPECT_END_OF_FILE_TOKEN();
    END_ENUMERATION();
}

TEST_CASE(single_quoted_attributes)
{
    auto tokens = run_tokenizer("<p foo='bar'>"sv);
    BEGIN_ENUMERATION(tokens);
    EXPECT_START_TAG_TOKEN(p, 1u, 12u);
    EXPECT_TAG_TOKEN_ATTRIBUTE_COUNT(1);
    EXPECT_TAG_TOKEN_ATTRIBUTE(foo, "bar", 3u, 6u, 7u, 12u);
    EXPECT_END_OF_FILE_TOKEN();
    END_ENUMERATION();
}

TEST_CASE(double_quoted_attributes)
{
    auto tokens = run_tokenizer("<p foo=\"bar\">"sv);
    BEGIN_ENUMERATION(tokens);
    EXPECT_START_TAG_TOKEN(p, 1u, 12u);
    EXPECT_TAG_TOKEN_ATTRIBUTE_COUNT(1);
    EXPECT_TAG_TOKEN_ATTRIBUTE(foo, "bar", 3u, 6u, 7u, 12u);
    EXPECT_END_OF_FILE_TOKEN();
    END_ENUMERATION();
}

TEST_CASE(valueless_attribute)
{
    auto tokens = run_tokenizer("<p foo>"sv);
    BEGIN_ENUMERATION(tokens);
    EXPECT_START_TAG_TOKEN(p, 1u, 6u);
    EXPECT_TAG_TOKEN_ATTRIBUTE_COUNT(1);
    EXPECT_TAG_TOKEN_ATTRIBUTE(foo, "", 3u, 6u, 0u, 0u);
    EXPECT_END_OF_FILE_TOKEN();
    END_ENUMERATION();
}

TEST_CASE(multiple_attributes)
{
    auto tokens = run_tokenizer("<p foo=\"bar\" baz=foobar biz foo2=\"bar2\">"sv);
    BEGIN_ENUMERATION(tokens);
    EXPECT_START_TAG_TOKEN(p, 1u, 39u);
    EXPECT_TAG_TOKEN_ATTRIBUTE_COUNT(4);
    EXPECT_TAG_TOKEN_ATTRIBUTE(foo, "bar", 3u, 6u, 7u, 12u);
    EXPECT_TAG_TOKEN_ATTRIBUTE(baz, "foobar", 13u, 16u, 17u, 23u);
    EXPECT_TAG_TOKEN_ATTRIBUTE(biz, "", 24u, 27u, 0u, 0u);
    EXPECT_TAG_TOKEN_ATTRIBUTE(foo2, "bar2", 28u, 32u, 33u, 39u);
    EXPECT_END_OF_FILE_TOKEN();
    END_ENUMERATION();
}

TEST_CASE(character_reference_in_attribute)
{
    auto tokens = run_tokenizer("<p foo=a&amp;b bar='a&#38;b' baz=\"a&#x26;b\">"sv);
    BEGIN_ENUMERATION(tokens);
    EXPECT_START_TAG_TOKEN(p, 1u, 43u);
    EXPECT_TAG_TOKEN_ATTRIBUTE_COUNT(3);
    EXPECT_TAG_TOKEN_ATTRIBUTE(foo, "a&b", 3u, 6u, 7u, 14u);
    EXPECT_TAG_TOKEN_ATTRIBUTE(bar, "a&b", 15u, 18u, 19u, 28u);
    EXPECT_TAG_TOKEN_ATTRIBUTE(baz, "a&b", 29u, 32u, 33u, 43u);
    EXPECT_END_OF_FILE_TOKEN();
    END_ENUMERATION();
}

TEST_CASE(comment)
{
    auto tokens = run_tokenizer("<p><!-- This is a comment --></p>"sv);
    BEGIN_ENUMERATION(tokens);
    EXPECT_START_TAG_TOKEN(p, 1u, 2u);
    EXPECT_COMMENT_TOKEN();
    EXPECT_END_TAG_TOKEN(p, 31u, 32u);
    EXPECT_END_OF_FILE_TOKEN();
    END_ENUMERATION();
}

TEST_CASE(doctype)
{
    auto tokens = run_tokenizer("<!DOCTYPE html><html></html>"sv);
    BEGIN_ENUMERATION(tokens);
    EXPECT_DOCTYPE_TOKEN();
    EXPECT_START_TAG_TOKEN(html, 16u, 20u);
    EXPECT_END_TAG_TOKEN(html, 23u, 27u);
}

// NOTE: This relies on the format of HTMLToken::to_string() staying the same.
//       If that changes, or something is added to the test HTML, the hash needs to be adjusted.
TEST_CASE(regression)
{
    StringView path = "tokenizer-test.html"sv;

    auto file = MUST(Core::File::open(path, Core::File::OpenMode::Read));
    auto file_size = MUST(file->size());
    auto content = MUST(ByteBuffer::create_uninitialized(file_size));
    MUST(file->read_until_filled(content.bytes()));
    ByteString file_contents { content.bytes() };
    auto tokens = run_tokenizer(file_contents);
    u32 hash = hash_tokens(tokens);
    EXPECT_EQ(hash, 3657343287u);
}
