/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWGSL/Lexer.h>
#include <LibWGSL/Preprocessor.h>

void test_tokens_equal(Vector<WGSL::Token> const& actual_tokens, Vector<WGSL::Token> const& expected_tokens);
void test_tokens_equal(Vector<WGSL::Token> const& actual_tokens, Vector<WGSL::Token> const& expected_tokens)
{
    size_t const num_actual_tokens = actual_tokens.size();
    size_t const num_expected_tokens = expected_tokens.size();
    if (num_actual_tokens != num_expected_tokens) {
        FAIL(String::formatted("actual token count: {}, expected token count: {}", num_actual_tokens, num_expected_tokens));
        return;
    }

    for (size_t i = 0; i < num_actual_tokens; ++i) {
        WGSL::Token const& actual_token = actual_tokens[i];
        WGSL::Token const& expected_token = expected_tokens[i];
        if (actual_token != expected_token) {
            FAIL(String::formatted("index[{}]: actual token: {}, expected token: {}", i, actual_token.to_string(), expected_token.to_string()));
        }
    }
}

TEST_CASE(keywords)
{
    constexpr auto input = "struct fn var return"sv;
    WGSL::Preprocessor preprocessor(input);

    auto tokenize = [](String const& processed_text) {
        WGSL::Lexer lexer(processed_text);

        Vector<WGSL::Token> tokens;
        while (true) {
            auto token = lexer.next_token();
            tokens.append(token);
            if (token.type.get_pointer<WGSL::EndOfFileToken>() != nullptr)
                break;
        }

        Vector<WGSL::Token> const expected = {
            { WGSL::KeywordToken { WGSL::KeywordToken::Value::Struct }, 0, 1, 1 },
            { WGSL::KeywordToken { WGSL::KeywordToken::Value::Fn }, 7, 1, 8 },
            { WGSL::KeywordToken { WGSL::KeywordToken::Value::Var }, 10, 1, 11 },
            { WGSL::KeywordToken { WGSL::KeywordToken::Value::Return }, 14, 1, 15 },
            { WGSL::EndOfFileToken {}, 20, 1, 21 },
        };

        test_tokens_equal(tokens, expected);
    };

    EXPECT_NO_DEATH("Tokenize individual keywords", [&] {
        auto processed_text = MUST(preprocessor.process());
        tokenize(processed_text);
    }());
}

TEST_CASE(identifiers)
{
    constexpr auto input = "VertexOut color vertex_main fragment_main fragData output"sv;
    WGSL::Preprocessor preprocessor(input);

    auto tokenize = [](String const& processed_text) {
        WGSL::Lexer lexer(processed_text);

        Vector<WGSL::Token> tokens;
        while (true) {
            auto token = lexer.next_token();
            tokens.append(token);
            if (token.type.get_pointer<WGSL::EndOfFileToken>() != nullptr)
                break;
        }

        Vector<WGSL::Token> const expected = {
            { WGSL::IdentifierToken { "VertexOut"_string }, 0, 1, 1 },
            { WGSL::IdentifierToken { "color"_string }, 10, 1, 11 },
            { WGSL::IdentifierToken { "vertex_main"_string }, 16, 1, 17 },
            { WGSL::IdentifierToken { "fragment_main"_string }, 28, 1, 29 },
            { WGSL::IdentifierToken { "fragData"_string }, 42, 1, 43 },
            { WGSL::IdentifierToken { "output"_string }, 51, 1, 52 },
            { WGSL::EndOfFileToken {}, 57, 1, 58 },
        };

        test_tokens_equal(tokens, expected);
    };
    EXPECT_NO_DEATH("Tokenize identifiers", [&] {
        auto processed_text = MUST(preprocessor.process());
        tokenize(processed_text);
    }());
}

TEST_CASE(types)
{
    constexpr auto input = "vec3f vec4f"sv;
    WGSL::Preprocessor preprocessor(input);

    auto tokenize = [](String const& processed_text) {
        WGSL::Lexer lexer(processed_text);

        Vector<WGSL::Token> tokens;
        while (true) {
            auto token = lexer.next_token();
            tokens.append(token);
            if (token.type.get_pointer<WGSL::EndOfFileToken>() != nullptr)
                break;
        }

        Vector<WGSL::Token> const expected = {
            { WGSL::TypeToken { WGSL::TypeToken::Value::vec3f }, 0, 1, 1 },
            { WGSL::TypeToken { WGSL::TypeToken::Value::vec4f }, 6, 1, 7 },
            { WGSL::EndOfFileToken {}, 11, 1, 12 },
        };

        test_tokens_equal(tokens, expected);
    };
    EXPECT_NO_DEATH("Tokenize types", [&] {
        auto processed_text = MUST(preprocessor.process());
        tokenize(processed_text);
    }());
}

TEST_CASE(integer_literals)
{
    constexpr auto input = "0 1 123 01"sv;
    WGSL::Preprocessor preprocessor(input);

    auto tokenize = [](String const& processed_text) {
        WGSL::Lexer lexer(processed_text);

        Vector<WGSL::Token> tokens;
        while (true) {
            auto token = lexer.next_token();
            tokens.append(token);
            if (token.type.get_pointer<WGSL::EndOfFileToken>() != nullptr)
                break;
        }

        Vector<WGSL::Token> const expected = {
            { WGSL::LiteralToken { WGSL::LiteralToken::Value::Int }, 0, 1, 1 },
            { WGSL::LiteralToken { WGSL::LiteralToken::Value::Int }, 2, 1, 3 },
            { WGSL::LiteralToken { WGSL::LiteralToken::Value::Int }, 4, 1, 5 },
            { WGSL::InvalidToken { "Leading zero in integer literal is not allowed"sv }, 8, 1, 9 },
            { WGSL::LiteralToken { WGSL::LiteralToken::Value::Int }, 9, 1, 10 },
            { WGSL::EndOfFileToken {}, 10, 1, 11 },
        };

        test_tokens_equal(tokens, expected);
    };
    EXPECT_NO_DEATH("Tokenize integer literals (0, 1, 123) and invalid case (01)", [&] {
        auto processed_text = MUST(preprocessor.process());
        tokenize(processed_text);
    }());
}

TEST_CASE(attributes)
{
    constexpr auto input = "@builtin(position) @location(0) @vertex @fragment"sv;
    WGSL::Preprocessor preprocessor(input);

    auto tokenize = [](String const& processed_text) {
        WGSL::Lexer lexer(processed_text);

        Vector<WGSL::Token> tokens;
        while (true) {
            auto token = lexer.next_token();
            tokens.append(token);
            if (token.type.get_pointer<WGSL::EndOfFileToken>() != nullptr)
                break;
        }

        Vector<WGSL::Token> const expected = {
            { WGSL::SyntacticToken { WGSL::SyntacticToken::Value::At }, 0, 1, 1 },
            { WGSL::AttributeToken { WGSL::BuiltinAttribute { WGSL::BuiltinAttribute::Flags::Position } }, 1, 1, 2 },
            { WGSL::SyntacticToken { WGSL::SyntacticToken::Value::At }, 19, 1, 20 },
            { WGSL::AttributeToken { WGSL::LocationAttribute { 0 } }, 20, 1, 21 },
            { WGSL::SyntacticToken { WGSL::SyntacticToken::Value::At }, 32, 1, 33 },
            { WGSL::AttributeToken { WGSL::VertexAttribute {} }, 33, 1, 34 },
            { WGSL::SyntacticToken { WGSL::SyntacticToken::Value::At }, 40, 1, 41 },
            { WGSL::AttributeToken { WGSL::FragmentAttribute {} }, 41, 1, 42 },
            { WGSL::EndOfFileToken {}, 49, 1, 50 },
        };

        test_tokens_equal(tokens, expected);
    };
    EXPECT_NO_DEATH("Tokenize attributes with and without arguments", [&] {
        auto processed_text = MUST(preprocessor.process());
        tokenize(processed_text);
    }());
}

TEST_CASE(syntactic_tokens)
{
    constexpr auto input = "@(){};,:.->="sv;
    WGSL::Preprocessor preprocessor(input);

    auto tokenize = [](String const& processed_text) {
        WGSL::Lexer lexer(processed_text);

        Vector<WGSL::Token> tokens;
        while (true) {
            auto token = lexer.next_token();
            tokens.append(token);
            if (token.type.get_pointer<WGSL::EndOfFileToken>() != nullptr)
                break;
        }

        Vector<WGSL::Token> const expected = {
            { WGSL::SyntacticToken { WGSL::SyntacticToken::Value::At }, 0, 1, 1 },
            { WGSL::SyntacticToken { WGSL::SyntacticToken::Value::OpenParen }, 1, 1, 2 },
            { WGSL::SyntacticToken { WGSL::SyntacticToken::Value::CloseParen }, 2, 1, 3 },
            { WGSL::SyntacticToken { WGSL::SyntacticToken::Value::OpenBrace }, 3, 1, 4 },
            { WGSL::SyntacticToken { WGSL::SyntacticToken::Value::CloseBrace }, 4, 1, 5 },
            { WGSL::SyntacticToken { WGSL::SyntacticToken::Value::Semicolon }, 5, 1, 6 },
            { WGSL::SyntacticToken { WGSL::SyntacticToken::Value::Comma }, 6, 1, 7 },
            { WGSL::SyntacticToken { WGSL::SyntacticToken::Value::Colon }, 7, 1, 8 },
            { WGSL::SyntacticToken { WGSL::SyntacticToken::Value::Dot }, 8, 1, 9 },
            { WGSL::SyntacticToken { WGSL::SyntacticToken::Value::Arrow }, 9, 1, 10 },
            { WGSL::SyntacticToken { WGSL::SyntacticToken::Value::Equals }, 11, 1, 12 },
            { WGSL::EndOfFileToken {}, 12, 1, 13 },
        };

        test_tokens_equal(tokens, expected);
    };
    EXPECT_NO_DEATH("Tokenize syntactic tokens used in the shader", [&] {
        auto processed_text = MUST(preprocessor.process());
        tokenize(processed_text);
    }());
}

TEST_CASE(whitespace_and_newlines)
{
    constexpr auto input = "struct\n  VertexOut\t{\r\n}"sv;
    WGSL::Preprocessor preprocessor(input);

    auto tokenize = [](String const& processed_text) {
        WGSL::Lexer lexer(processed_text);

        Vector<WGSL::Token> tokens;
        while (true) {
            auto token = lexer.next_token();
            tokens.append(token);
            if (token.type.get_pointer<WGSL::EndOfFileToken>() != nullptr)
                break;
        }

        Vector<WGSL::Token> const expected = {
            { WGSL::KeywordToken { WGSL::KeywordToken::Value::Struct }, 0, 1, 1 },
            { WGSL::IdentifierToken { "VertexOut"_string }, 9, 2, 3 },
            { WGSL::SyntacticToken { WGSL::SyntacticToken::Value::OpenBrace }, 19, 2, 13 },
            { WGSL::SyntacticToken { WGSL::SyntacticToken::Value::CloseBrace }, 22, 3, 1 },
            { WGSL::EndOfFileToken {}, 23, 3, 2 },
        };

        test_tokens_equal(tokens, expected);
    };
    EXPECT_NO_DEATH(" Ensure whitespace and newlines are skipped correctly", [&] {
        auto processed_text = MUST(preprocessor.process());
        tokenize(processed_text);
    }());
}

TEST_CASE(invalid_identifier_underscore)
{
    constexpr auto input = "_"sv;
    WGSL::Preprocessor preprocessor(input);

    auto tokenize = [](String const& processed_text) {
        WGSL::Lexer lexer(processed_text);

        Vector<WGSL::Token> tokens;
        while (true) {
            auto token = lexer.next_token();
            tokens.append(token);
            if (token.type.get_pointer<WGSL::EndOfFileToken>() != nullptr)
                break;
        }

        Vector<WGSL::Token> const expected = {
            { WGSL::InvalidToken { "Single underscore is not a valid identifier"sv }, 0, 1, 1 },
            { WGSL::EndOfFileToken {}, 1, 1, 2 },
        };

        test_tokens_equal(tokens, expected);
    };
    EXPECT_NO_DEATH("'_' identifier is not a valid token", [&] {
        auto processed_text = MUST(preprocessor.process());
        tokenize(processed_text);
    }());
}

TEST_CASE(invalid_identifier_double_underscore)
{
    constexpr auto input = "__abc"sv;
    WGSL::Preprocessor preprocessor(input);

    auto tokenize = [](String const& processed_text) {
        WGSL::Lexer lexer(processed_text);

        Vector<WGSL::Token> tokens;
        while (true) {
            auto token = lexer.next_token();
            tokens.append(token);
            if (token.type.get_pointer<WGSL::EndOfFileToken>() != nullptr)
                break;
        }

        Vector<WGSL::Token> const expected = {
            { WGSL::InvalidToken { "Identifiers cannot start with double underscore"sv }, 0, 1, 1 },
            { WGSL::EndOfFileToken {}, 5, 1, 6 },
        };

        test_tokens_equal(tokens, expected);
    };
    EXPECT_NO_DEATH("Identifiers starting with double underscore are invalid", [&] {
        auto processed_text = MUST(preprocessor.process());
        tokenize(processed_text);
    }());
}

TEST_CASE(unexpected_character)
{
    constexpr auto input = "struct # VertexOut"sv;
    WGSL::Preprocessor preprocessor(input);

    auto tokenize = [](String const& processed_text) {
        WGSL::Lexer lexer(processed_text);

        Vector<WGSL::Token> tokens;
        while (true) {
            auto token = lexer.next_token();
            tokens.append(token);
            if (token.type.get_pointer<WGSL::EndOfFileToken>() != nullptr)
                break;
        }

        Vector<WGSL::Token> const expected = {
            { WGSL::KeywordToken { WGSL::KeywordToken::Value::Struct }, 0, 1, 1 },
            { WGSL::InvalidToken { "Invalid token encountered: #"sv }, 7, 1, 8 },
            { WGSL::IdentifierToken { "VertexOut"_string }, 9, 1, 10 },
            { WGSL::EndOfFileToken {}, 18, 1, 19 },
        };

        test_tokens_equal(tokens, expected);
    };
    EXPECT_NO_DEATH("Unexpected character results in invalid token", [&] {
        auto processed_text = MUST(preprocessor.process());
        tokenize(processed_text);
    }());
}

TEST_CASE(simple_triangle_shader)
{
    constexpr auto input = R"(
struct VertexIn {
  @location(0) position: vec4f,
  @location(1) color: vec4f,
};

struct VertexOut {
  @builtin(position) position : vec4f,
  @location(0) color : vec4f
}

@vertex
fn vertex_main(input: VertexIn) -> VertexOut {
  var output : VertexOut;
  output.position = input.position;
  output.color = input.color;
  return output;
}

@fragment
fn fragment_main(fragData: VertexOut) -> @location(0) vec4f {
  return fragData.color;
}
)"sv;

    WGSL::Preprocessor preprocessor(input);

    auto tokenize = [](String const& processed_text) {
        WGSL::Lexer lexer(processed_text);

        Vector<WGSL::Token> tokens;
        while (true) {
            auto token = lexer.next_token();
            tokens.append(token);
            if (token.type.get_pointer<WGSL::EndOfFileToken>() != nullptr)
                break;
        }

        using WGSL::KeywordToken;
        using WGSL::LiteralToken;
        using WGSL::TypeToken;
        using WGSL::InvalidToken;
        using WGSL::IdentifierToken;
        using WGSL::SyntacticToken;
        using WGSL::AttributeToken;
        using WGSL::BuiltinAttribute;
        using WGSL::FragmentAttribute;
        using WGSL::VertexAttribute;
        using WGSL::LocationAttribute;
        using WGSL::EndOfFileToken;
        Vector<WGSL::Token> const expected = {
            { KeywordToken { KeywordToken::Value::Struct }, 1, 2, 1 },
            { IdentifierToken { "VertexIn"_string }, 8, 2, 8 },
            { SyntacticToken { SyntacticToken::Value::OpenBrace }, 17, 2, 17 },
            { SyntacticToken { SyntacticToken::Value::At }, 21, 3, 3 },
            { AttributeToken { LocationAttribute { 0 } }, 22, 3, 4 },
            { IdentifierToken { "position"_string }, 34, 3, 16 },
            { SyntacticToken { SyntacticToken::Value::Colon }, 42, 3, 24 },
            { TypeToken { TypeToken::Value::vec4f }, 44, 3, 26 },
            { SyntacticToken { SyntacticToken::Value::Comma }, 49, 3, 31 },
            { SyntacticToken { SyntacticToken::Value::At }, 53, 4, 3 },
            { AttributeToken { LocationAttribute { 1 } }, 54, 4, 4 },
            { IdentifierToken { "color"_string }, 66, 4, 16 },
            { SyntacticToken { SyntacticToken::Value::Colon }, 71, 4, 21 },
            { TypeToken { TypeToken::Value::vec4f }, 73, 4, 23 },
            { SyntacticToken { SyntacticToken::Value::Comma }, 78, 4, 28 },
            { SyntacticToken { SyntacticToken::Value::CloseBrace }, 80, 5, 1 },
            { SyntacticToken { SyntacticToken::Value::Semicolon }, 81, 5, 2 },
            { KeywordToken { KeywordToken::Value::Struct }, 84, 7, 1 },
            { IdentifierToken { "VertexOut"_string }, 91, 7, 8 },
            { SyntacticToken { SyntacticToken::Value::OpenBrace }, 101, 7, 18 },
            { SyntacticToken { SyntacticToken::Value::At }, 105, 8, 3 },
            { AttributeToken { BuiltinAttribute { BuiltinAttribute::Flags::Position } }, 106, 8, 4 },
            { IdentifierToken { "position"_string }, 124, 8, 22 },
            { SyntacticToken { SyntacticToken::Value::Colon }, 133, 8, 31 },
            { TypeToken { TypeToken::Value::vec4f }, 135, 8, 33 },
            { SyntacticToken { SyntacticToken::Value::Comma }, 140, 8, 38 },
            { SyntacticToken { SyntacticToken::Value::At }, 144, 9, 3 },
            { AttributeToken { LocationAttribute { 0 } }, 145, 9, 4 },
            { IdentifierToken { "color"_string }, 157, 9, 16 },
            { SyntacticToken { SyntacticToken::Value::Colon }, 163, 9, 22 },
            { TypeToken { TypeToken::Value::vec4f }, 165, 9, 24 },
            { SyntacticToken { SyntacticToken::Value::CloseBrace }, 171, 10, 1 },
            { SyntacticToken { SyntacticToken::Value::At }, 174, 12, 1 },
            { AttributeToken { VertexAttribute {} }, 175, 12, 2 },
            { KeywordToken { KeywordToken::Value::Fn }, 182, 13, 1 },
            { IdentifierToken { "vertex_main"_string }, 185, 13, 4 },
            { SyntacticToken { SyntacticToken::Value::OpenParen }, 196, 13, 15 },
            { IdentifierToken { "input"_string }, 197, 13, 16 },
            { SyntacticToken { SyntacticToken::Value::Colon }, 202, 13, 21 },
            { IdentifierToken { "VertexIn"_string }, 204, 13, 23 },
            { SyntacticToken { SyntacticToken::Value::CloseParen }, 212, 13, 31 },
            { SyntacticToken { SyntacticToken::Value::Arrow }, 214, 13, 33 },
            { IdentifierToken { "VertexOut"_string }, 217, 13, 36 },
            { SyntacticToken { SyntacticToken::Value::OpenBrace }, 227, 13, 46 },
            { KeywordToken { KeywordToken::Value::Var }, 231, 14, 3 },
            { IdentifierToken { "output"_string }, 235, 14, 7 },
            { SyntacticToken { SyntacticToken::Value::Colon }, 242, 14, 14 },
            { IdentifierToken { "VertexOut"_string }, 244, 14, 16 },
            { SyntacticToken { SyntacticToken::Value::Semicolon }, 253, 14, 25 },
            { IdentifierToken { "output"_string }, 257, 15, 3 },
            { SyntacticToken { SyntacticToken::Value::Dot }, 263, 15, 9 },
            { IdentifierToken { "position"_string }, 264, 15, 10 },
            { SyntacticToken { SyntacticToken::Value::Equals }, 273, 15, 19 },
            { IdentifierToken { "input"_string }, 275, 15, 21 },
            { SyntacticToken { SyntacticToken::Value::Dot }, 280, 15, 26 },
            { IdentifierToken { "position"_string }, 281, 15, 27 },
            { SyntacticToken { SyntacticToken::Value::Semicolon }, 289, 15, 35 },
            { IdentifierToken { "output"_string }, 293, 16, 3 },
            { SyntacticToken { SyntacticToken::Value::Dot }, 299, 16, 9 },
            { IdentifierToken { "color"_string }, 300, 16, 10 },
            { SyntacticToken { SyntacticToken::Value::Equals }, 306, 16, 16 },
            { IdentifierToken { "input"_string }, 308, 16, 18 },
            { SyntacticToken { SyntacticToken::Value::Dot }, 313, 16, 23 },
            { IdentifierToken { "color"_string }, 314, 16, 24 },
            { SyntacticToken { SyntacticToken::Value::Semicolon }, 319, 16, 29 },
            { KeywordToken { KeywordToken::Value::Return }, 323, 17, 3 },
            { IdentifierToken { "output"_string }, 330, 17, 10 },
            { SyntacticToken { SyntacticToken::Value::Semicolon }, 336, 17, 16 },
            { SyntacticToken { SyntacticToken::Value::CloseBrace }, 338, 18, 1 },
            { SyntacticToken { SyntacticToken::Value::At }, 341, 20, 1 },
            { AttributeToken { FragmentAttribute {} }, 342, 20, 2 },
            { KeywordToken { KeywordToken::Value::Fn }, 351, 21, 1 },
            { IdentifierToken { "fragment_main"_string }, 354, 21, 4 },
            { SyntacticToken { SyntacticToken::Value::OpenParen }, 367, 21, 17 },
            { IdentifierToken { "fragData"_string }, 368, 21, 18 },
            { SyntacticToken { SyntacticToken::Value::Colon }, 376, 21, 26 },
            { IdentifierToken { "VertexOut"_string }, 378, 21, 28 },
            { SyntacticToken { SyntacticToken::Value::CloseParen }, 387, 21, 37 },
            { SyntacticToken { SyntacticToken::Value::Arrow }, 389, 21, 39 },
            { SyntacticToken { SyntacticToken::Value::At }, 392, 21, 42 },
            { AttributeToken { LocationAttribute { 0 } }, 393, 21, 43 },
            { TypeToken { TypeToken::Value::vec4f }, 405, 21, 55 },
            { SyntacticToken { SyntacticToken::Value::OpenBrace }, 411, 21, 61 },
            { KeywordToken { KeywordToken::Value::Return }, 415, 22, 3 },
            { IdentifierToken { "fragData"_string }, 422, 22, 10 },
            { SyntacticToken { SyntacticToken::Value::Dot }, 430, 22, 18 },
            { IdentifierToken { "color"_string }, 431, 22, 19 },
            { SyntacticToken { SyntacticToken::Value::Semicolon }, 436, 22, 24 },
            { SyntacticToken { SyntacticToken::Value::CloseBrace }, 438, 23, 1 },
            { EndOfFileToken {}, 440, 24, 1 },
        };
        test_tokens_equal(tokens, expected);
    };
    EXPECT_NO_DEATH("Tokenize a simple triangle shader", [&] {
        auto processed_text = MUST(preprocessor.process());
        tokenize(processed_text);
    }());
}
