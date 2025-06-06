/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWGSL/Lexer.h>
#include <LibWGSL/Parser.h>
#include <LibWGSL/Preprocessor.h>

void test_ast_equal(WGSL::Program const& actual_ast, WGSL::Program const& expected_ast);
void test_ast_equal(WGSL::Program const& actual_ast, WGSL::Program const& expected_ast)
{
    if (!(actual_ast == expected_ast)) {
        FAIL(String::formatted("\nActual {}\nExpected {}", actual_ast.to_string(), expected_ast.to_string()));
    }
}

TEST_CASE(simple_struct)
{
    constexpr auto input = R"(
        struct Vertex {
            position: vec4f,
            @location(0) color: vec3f,
        };
    )"sv;
    WGSL::Preprocessor preprocessor(input);

    auto parse = [](String const& processed_text) {
        WGSL::Lexer lexer(processed_text);
        Vector<WGSL::Token> tokens;
        while (true) {
            auto token = lexer.next_token();
            tokens.append(token);
            if (token.type.has<WGSL::EndOfFileToken>())
                break;
        }

        WGSL::Parser parser(move(tokens));
        auto program = parser.parse();
        if (program.is_error()) {
            FAIL(String::formatted("Parse error: {}", program.error().string_literal()));
            return;
        }
        auto actual = program.value();

        WGSL::Program expected;
        expected.declarations.append(make_ref_counted<WGSL::StructDeclaration>(
            "Vertex"_string,
            Vector {
                make_ref_counted<WGSL::StructMember>(
                    Vector<NonnullRefPtr<WGSL::Attribute>> {},
                    "position"_string,
                    make_ref_counted<WGSL::VectorType>(WGSL::VectorType::Kind::Vec4f)),
                make_ref_counted<WGSL::StructMember>(
                    Vector<NonnullRefPtr<WGSL::Attribute>> { make_ref_counted<WGSL::LocationAttribute>(0) },
                    "color"_string,
                    make_ref_counted<WGSL::VectorType>(WGSL::VectorType::Kind::Vec3f)) }));

        test_ast_equal(actual, expected);
    };

    EXPECT_NO_DEATH("Parse simple struct declaration", [&] {
        auto processed_text = MUST(preprocessor.process());
        parse(processed_text);
    }());
}

TEST_CASE(simple_function)
{
    constexpr auto input = R"(
        @vertex
        fn vertex_main(input: vec3f) -> vec4f {
            return input;
        }
    )"sv;
    WGSL::Preprocessor preprocessor(input);

    auto parse = [](String const& processed_text) {
        WGSL::Lexer lexer(processed_text);
        Vector<WGSL::Token> tokens;
        while (true) {
            auto token = lexer.next_token();
            tokens.append(token);
            if (token.type.has<WGSL::EndOfFileToken>())
                break;
        }

        WGSL::Parser parser(move(tokens));
        auto program = parser.parse();
        if (program.is_error()) {
            FAIL(String::formatted("Parse error: {}", program.error().string_literal()));
            return;
        }
        auto actual = program.value();

        WGSL::Program expected;
        expected.declarations.append(make_ref_counted<WGSL::FunctionDeclaration>(
            Vector<NonnullRefPtr<WGSL::Attribute>> { make_ref_counted<WGSL::VertexAttribute>() },
            "vertex_main"_string,
            Vector {
                make_ref_counted<WGSL::Parameter>(
                    "input"_string,
                    make_ref_counted<WGSL::VectorType>(WGSL::VectorType::Kind::Vec3f)) },
            make_ref_counted<WGSL::VectorType>(WGSL::VectorType::Kind::Vec4f),
            Vector<NonnullRefPtr<WGSL::Attribute>> {},
            Vector<NonnullRefPtr<WGSL::Statement>> {
                make_ref_counted<WGSL::ReturnStatement>(
                    make_ref_counted<WGSL::IdentifierExpression>("input"_string)) }));

        test_ast_equal(actual, expected);
    };

    EXPECT_NO_DEATH("Parse simple function with vertex attribute", [&] {
        auto processed_text = MUST(preprocessor.process());
        parse(processed_text);
    }());
}

TEST_CASE(variable_statement)
{
    constexpr auto input = R"(
        fn main() {
            var x: vec3f = y;
        }
    )"sv;
    WGSL::Preprocessor preprocessor(input);

    auto parse = [](String const& processed_text) {
        WGSL::Lexer lexer(processed_text);
        Vector<WGSL::Token> tokens;
        while (true) {
            auto token = lexer.next_token();
            tokens.append(token);
            if (token.type.has<WGSL::EndOfFileToken>())
                break;
        }

        WGSL::Parser parser(move(tokens));
        auto program = parser.parse();
        if (program.is_error()) {
            FAIL(String::formatted("Parse error: {}", program.error().string_literal()));
            return;
        }
        auto actual = program.value();

        WGSL::Program expected;
        expected.declarations.append(make_ref_counted<WGSL::FunctionDeclaration>(
            Vector<NonnullRefPtr<WGSL::Attribute>> {},
            "main"_string,
            Vector<NonnullRefPtr<WGSL::Parameter>> {},
            Optional<NonnullRefPtr<WGSL::Type>> {},
            Vector<NonnullRefPtr<WGSL::Attribute>> {},
            Vector<NonnullRefPtr<WGSL::Statement>> {
                make_ref_counted<WGSL::VariableStatement>(
                    "x"_string,
                    make_ref_counted<WGSL::VectorType>(WGSL::VectorType::Kind::Vec3f),
                    make_ref_counted<WGSL::IdentifierExpression>("y"_string)) }));

        test_ast_equal(actual, expected);
    };

    EXPECT_NO_DEATH("Parse variable statement with initializer", [&] {
        auto processed_text = MUST(preprocessor.process());
        parse(processed_text);
    }());
}

TEST_CASE(assignment_statement)
{
    constexpr auto input = R"(
        fn main() {
            output.color = input.color;
        }
    )"sv;
    WGSL::Preprocessor preprocessor(input);

    auto parse = [](String const& processed_text) {
        WGSL::Lexer lexer(processed_text);
        Vector<WGSL::Token> tokens;
        while (true) {
            auto token = lexer.next_token();
            tokens.append(token);
            if (token.type.has<WGSL::EndOfFileToken>())
                break;
        }

        WGSL::Parser parser(move(tokens));
        auto program = parser.parse();
        if (program.is_error()) {
            FAIL(String::formatted("Parse error: {}", program.error().string_literal()));
            return;
        }

        auto actual = program.value();

        WGSL::Program expected;
        expected.declarations.append(make_ref_counted<WGSL::FunctionDeclaration>(
            Vector<NonnullRefPtr<WGSL::Attribute>> {},
            "main"_string,
            Vector<NonnullRefPtr<WGSL::Parameter>> {},
            Optional<NonnullRefPtr<WGSL::Type>> {},
            Vector<NonnullRefPtr<WGSL::Attribute>> {},
            Vector<NonnullRefPtr<WGSL::Statement>> { make_ref_counted<WGSL::AssignmentStatement>(
                make_ref_counted<WGSL::MemberAccessExpression>(
                    make_ref_counted<WGSL::IdentifierExpression>("output"_string),
                    "color"_string),
                make_ref_counted<WGSL::MemberAccessExpression>(
                    make_ref_counted<WGSL::IdentifierExpression>("input"_string),
                    "color"_string)) }));

        test_ast_equal(actual, expected);
    };

    EXPECT_NO_DEATH("Parse assignment statement with member access", [&] {
        auto processed_text = MUST(preprocessor.process());
        parse(processed_text);
    }());
}

TEST_CASE(invalid_struct_missing_member_name)
{
    constexpr auto input = R"(
        struct Vertex {
            : vec4f
        };
    )"sv;
    WGSL::Preprocessor preprocessor(input);

    auto parse = [](String const& processed_text) {
        WGSL::Lexer lexer(processed_text);
        Vector<WGSL::Token> tokens;
        while (true) {
            auto token = lexer.next_token();
            tokens.append(token);
            if (token.type.has<WGSL::EndOfFileToken>())
                break;
        }

        WGSL::Parser parser(move(tokens));
        auto program = parser.parse();
        if (!program.is_error()) {
            FAIL("Unexpected successful parse of struct member with no identifier");
            return;
        }
        EXPECT_EQ(program.error().string_literal(), "Expected member name");
    };

    EXPECT_NO_DEATH("Parse invalid struct missing closing brace", [&] {
        auto processed_text = MUST(preprocessor.process());
        parse(processed_text);
    }());
}

// FIXME: Add tests for remaining invalid scenarios

TEST_CASE(simple_triangle_shader)
{
    constexpr auto input = R"(
        struct VertexIn {
            @location(0) position: vec4f,
            @location(1) color: vec4f,
        };

        struct VertexOut {
            @builtin(position) position: vec4f,
            @location(0) color: vec4f,
        };

        @vertex
        fn vertex_main(input: VertexIn) -> VertexOut {
            var output: VertexOut;
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

    auto parse = [](String const& processed_text) {
        WGSL::Lexer lexer(processed_text);
        Vector<WGSL::Token> tokens;
        while (true) {
            auto token = lexer.next_token();
            tokens.append(token);
            if (token.type.has<WGSL::EndOfFileToken>())
                break;
        }

        WGSL::Parser parser(move(tokens));
        auto program = parser.parse();
        if (program.is_error()) {
            FAIL(String::formatted("Parse error: {}", program.error().string_literal()));
            return;
        }
        auto actual = program.value();

        WGSL::Program expected;
        expected.declarations.append(make_ref_counted<WGSL::StructDeclaration>(
            "VertexIn"_string,
            Vector {
                make_ref_counted<WGSL::StructMember>(
                    Vector<NonnullRefPtr<WGSL::Attribute>> { make_ref_counted<WGSL::LocationAttribute>(0) },
                    "position"_string,
                    make_ref_counted<WGSL::VectorType>(WGSL::VectorType::Kind::Vec4f)),
                make_ref_counted<WGSL::StructMember>(
                    Vector<NonnullRefPtr<WGSL::Attribute>> { make_ref_counted<WGSL::LocationAttribute>(1) },
                    "color"_string,
                    make_ref_counted<WGSL::VectorType>(WGSL::VectorType::Kind::Vec4f)) }));
        expected.declarations.append(make_ref_counted<WGSL::StructDeclaration>(
            "VertexOut"_string,
            Vector {
                make_ref_counted<WGSL::StructMember>(
                    Vector<NonnullRefPtr<WGSL::Attribute>> { make_ref_counted<WGSL::BuiltinAttribute>(WGSL::BuiltinAttribute::Kind::Position) },
                    "position"_string,
                    make_ref_counted<WGSL::VectorType>(WGSL::VectorType::Kind::Vec4f)),
                make_ref_counted<WGSL::StructMember>(
                    Vector<NonnullRefPtr<WGSL::Attribute>> { make_ref_counted<WGSL::LocationAttribute>(0) },
                    "color"_string,
                    make_ref_counted<WGSL::VectorType>(WGSL::VectorType::Kind::Vec4f)) }));
        expected.declarations.append(make_ref_counted<WGSL::FunctionDeclaration>(
            Vector<NonnullRefPtr<WGSL::Attribute>> { make_ref_counted<WGSL::VertexAttribute>() },
            "vertex_main"_string,
            Vector { make_ref_counted<WGSL::Parameter>("input"_string, make_ref_counted<WGSL::NamedType>("VertexIn"_string)) },
            make_ref_counted<WGSL::NamedType>("VertexOut"_string),
            Vector<NonnullRefPtr<WGSL::Attribute>> {},
            Vector<NonnullRefPtr<WGSL::Statement>> {
                make_ref_counted<WGSL::VariableStatement>(
                    "output"_string,
                    make_ref_counted<WGSL::NamedType>("VertexOut"_string),
                    Optional<NonnullRefPtr<WGSL::Expression>> {}),
                make_ref_counted<WGSL::AssignmentStatement>(
                    make_ref_counted<WGSL::MemberAccessExpression>(
                        make_ref_counted<WGSL::IdentifierExpression>("output"_string),
                        "position"_string),
                    make_ref_counted<WGSL::MemberAccessExpression>(
                        make_ref_counted<WGSL::IdentifierExpression>("input"_string),
                        "position"_string)),
                make_ref_counted<WGSL::AssignmentStatement>(
                    make_ref_counted<WGSL::MemberAccessExpression>(
                        make_ref_counted<WGSL::IdentifierExpression>("output"_string),
                        "color"_string),
                    make_ref_counted<WGSL::MemberAccessExpression>(
                        make_ref_counted<WGSL::IdentifierExpression>("input"_string),
                        "color"_string)),
                make_ref_counted<WGSL::ReturnStatement>(
                    make_ref_counted<WGSL::IdentifierExpression>("output"_string)) }));
        expected.declarations.append(make_ref_counted<WGSL::FunctionDeclaration>(
            Vector<NonnullRefPtr<WGSL::Attribute>> { make_ref_counted<WGSL::FragmentAttribute>() },
            "fragment_main"_string,
            Vector { make_ref_counted<WGSL::Parameter>("fragData"_string, make_ref_counted<WGSL::NamedType>("VertexOut"_string)) },
            make_ref_counted<WGSL::VectorType>(WGSL::VectorType::Kind::Vec4f),
            Vector<NonnullRefPtr<WGSL::Attribute>> { make_ref_counted<WGSL::LocationAttribute>(0) },
            Vector<NonnullRefPtr<WGSL::Statement>> {
                make_ref_counted<WGSL::ReturnStatement>(make_ref_counted<WGSL::MemberAccessExpression>(
                    make_ref_counted<WGSL::IdentifierExpression>("fragData"_string),
                    "color"_string)) }));

        test_ast_equal(actual, expected);
    };

    EXPECT_NO_DEATH("Parse simple triangle shader", [&] {
        auto processed_text = MUST(preprocessor.process());
        parse(processed_text);
    }());
}
