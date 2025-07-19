/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWGSL/Parser.h>

namespace WGSL {

Parser::Parser(Vector<Token> tokens)
    : m_tokens(move(tokens))
{
}

Token const& Parser::current_token() const
{
    if (m_current_index >= m_tokens.size()) {
        return m_tokens.last();
    }
    return m_tokens[m_current_index];
}

Token const& Parser::peek_token(size_t offset) const
{
    size_t index = m_current_index + offset;
    if (index >= m_tokens.size()) {
        return m_tokens.last();
    }
    return m_tokens[index];
}

bool Parser::is_at_end() const
{
    return current_token().type.has<EndOfFileToken>();
}

void Parser::advance()
{
    if (!is_at_end()) {
        m_current_index++;
    }
}

bool Parser::match(auto&& predicate)
{
    if (predicate(current_token())) {
        advance();
        return true;
    }
    return false;
}

ErrorOr<void> Parser::consume(auto&& predicate, StringView error_message)
{
    if (!predicate(current_token())) {
        return Error::from_string_view(error_message);
    }
    advance();
    return {};
}

ErrorOr<Program> Parser::parse()
{
    Vector<NonnullRefPtr<Declaration>> declarations;
    while (!is_at_end()) {
        auto declaration = TRY(parse_declaration());
        declarations.append(move(declaration));
    }
    return Program { move(declarations) };
}

ErrorOr<NonnullRefPtr<Declaration>> Parser::parse_declaration()
{
    auto attributes = TRY(parse_attributes());
    if (current_token().type.has<KeywordToken>()) {
        auto const& keyword = current_token().type.get<KeywordToken>();
        if (keyword.value == KeywordToken::Value::Struct) {
            if (!attributes.is_empty()) {
                return Error::from_string_literal("Structs cannot have attributes");
            }
            return TRY(parse_struct_declaration());
        } else if (keyword.value == KeywordToken::Value::Fn) {
            return TRY(parse_function_declaration(move(attributes)));
        }
    }
    return Error::from_string_literal("Expected struct or function declaration");
}

ErrorOr<NonnullRefPtr<Declaration>> Parser::parse_struct_declaration()
{
    TRY(consume([](Token const& token) {
        return token.type.has<KeywordToken>() && token.type.get<KeywordToken>().value == KeywordToken::Value::Struct;
    },
        "Expected 'struct'"sv));
    if (!current_token().type.has<IdentifierToken>()) {
        return Error::from_string_literal("Expected struct name");
    }
    String name = current_token().type.get<IdentifierToken>().value;
    advance();
    TRY(consume([](Token const& token) {
        return token.type.has<SyntacticToken>() && token.type.get<SyntacticToken>().value == SyntacticToken::Value::OpenBrace;
    },
        "Expected '{'"sv));
    Vector<NonnullRefPtr<StructMember>> members;
    while (!current_token().type.has<SyntacticToken>() || current_token().type.get<SyntacticToken>().value != SyntacticToken::Value::CloseBrace) {
        auto member = TRY(parse_struct_member());
        members.append(move(member));
        if (current_token().type.has<SyntacticToken>() && current_token().type.get<SyntacticToken>().value == SyntacticToken::Value::Comma) {
            advance();
        }
    }
    TRY(consume([](Token const& token) {
        return token.type.has<SyntacticToken>() && token.type.get<SyntacticToken>().value == SyntacticToken::Value::CloseBrace;
    },
        "Expected '}'"sv));
    if (current_token().type.has<SyntacticToken>() && current_token().type.get<SyntacticToken>().value == SyntacticToken::Value::Semicolon) {
        advance();
    }
    return try_make_ref_counted<StructDeclaration>(move(name), move(members));
}

ErrorOr<NonnullRefPtr<Declaration>> Parser::parse_function_declaration(Vector<NonnullRefPtr<Attribute>> attributes)
{
    TRY(consume([](Token const& token) {
        return token.type.has<KeywordToken>() && token.type.get<KeywordToken>().value == KeywordToken::Value::Fn;
    },
        "Expected 'fn'"sv));
    if (!current_token().type.has<IdentifierToken>()) {
        return Error::from_string_literal("Expected function name");
    }
    String name = current_token().type.get<IdentifierToken>().value;
    advance();
    TRY(consume([](Token const& token) {
        return token.type.has<SyntacticToken>() && token.type.get<SyntacticToken>().value == SyntacticToken::Value::OpenParen;
    },
        "Expected '('"sv));
    Vector<NonnullRefPtr<Parameter>> parameters;
    while (!current_token().type.has<SyntacticToken>() || current_token().type.get<SyntacticToken>().value != SyntacticToken::Value::CloseParen) {
        auto param = TRY(parse_parameter());
        parameters.append(move(param));
        if (current_token().type.has<SyntacticToken>() && current_token().type.get<SyntacticToken>().value == SyntacticToken::Value::Comma) {
            advance();
        }
    }
    TRY(consume([](Token const& token) {
        return token.type.has<SyntacticToken>() && token.type.get<SyntacticToken>().value == SyntacticToken::Value::CloseParen;
    },
        "Expected ')'"sv));
    Optional<NonnullRefPtr<Type>> return_type;
    Vector<NonnullRefPtr<Attribute>> return_attributes;
    if (current_token().type.has<SyntacticToken>() && current_token().type.get<SyntacticToken>().value == SyntacticToken::Value::Arrow) {
        advance();
        return_attributes = TRY(parse_attributes());
        if (!is_at_end() && !current_token().type.has<SyntacticToken>()) {
            return_type = TRY(parse_type());
        }
    }
    auto body = TRY(parse_block());
    return try_make_ref_counted<FunctionDeclaration>(
        move(attributes), move(name), move(parameters),
        move(return_type), move(return_attributes), move(body));
}

ErrorOr<Vector<NonnullRefPtr<Attribute>>> Parser::parse_attributes()
{
    Vector<NonnullRefPtr<Attribute>> attributes;
    while (current_token().type.has<SyntacticToken>() && current_token().type.get<SyntacticToken>().value == SyntacticToken::Value::At) {
        advance();
        attributes.append(TRY(parse_attribute()));
    }
    return attributes;
}

ErrorOr<NonnullRefPtr<Attribute>> Parser::parse_attribute()
{
    if (!current_token().type.has<AttributeToken>()) {
        return Error::from_string_literal("Expected attribute");
    }
    auto const& attr_token = current_token().type.get<AttributeToken>();
    advance();
    // FIXME: Share builtin enum flags with Lexer
    return attr_token.visit(
        [](BuiltinAttributeToken const&) -> ErrorOr<NonnullRefPtr<Attribute>> {
            return try_make_ref_counted<BuiltinAttribute>(BuiltinAttribute::Kind::Position);
        },
        [](LocationAttributeToken const& location) -> ErrorOr<NonnullRefPtr<Attribute>> {
            return try_make_ref_counted<LocationAttribute>(location.value);
        },
        [](VertexAttributeToken const&) -> ErrorOr<NonnullRefPtr<Attribute>> {
            return try_make_ref_counted<VertexAttribute>();
        },
        [](FragmentAttributeToken const&) -> ErrorOr<NonnullRefPtr<Attribute>> {
            return try_make_ref_counted<FragmentAttribute>();
        });
}

ErrorOr<NonnullRefPtr<Type>> Parser::parse_type()
{
    // FIXME: Share type enum with Lexer
    if (current_token().type.has<TypeToken>()) {
        auto const& type_token = current_token().type.get<TypeToken>();
        advance();
        switch (type_token.value) {
        case TypeToken::Value::vec3f:
            return try_make_ref_counted<VectorType>(VectorType::Kind::Vec3f);
        case TypeToken::Value::vec4f:
            return try_make_ref_counted<VectorType>(VectorType::Kind::Vec4f);
        }
    }
    if (current_token().type.has<IdentifierToken>()) {
        String name = current_token().type.get<IdentifierToken>().value;
        advance();
        return try_make_ref_counted<NamedType>(move(name));
    }
    return Error::from_string_literal("Expected type");
}

ErrorOr<NonnullRefPtr<StructMember>> Parser::parse_struct_member()
{
    auto attributes = TRY(parse_attributes());
    if (!current_token().type.has<IdentifierToken>()) {
        return Error::from_string_literal("Expected member name");
    }
    String name = current_token().type.get<IdentifierToken>().value;
    advance();
    TRY(consume([](Token const& token) {
        return token.type.has<SyntacticToken>() && token.type.get<SyntacticToken>().value == SyntacticToken::Value::Colon;
    },
        "Expected ':'"sv));
    auto type = TRY(parse_type());
    return try_make_ref_counted<StructMember>(move(attributes), move(name), move(type));
}

ErrorOr<NonnullRefPtr<Parameter>> Parser::parse_parameter()
{
    if (!current_token().type.has<IdentifierToken>()) {
        return Error::from_string_literal("Expected parameter name");
    }
    String name = current_token().type.get<IdentifierToken>().value;
    advance();
    TRY(consume([](Token const& token) {
        return token.type.has<SyntacticToken>() && token.type.get<SyntacticToken>().value == SyntacticToken::Value::Colon;
    },
        "Expected ':'"sv));
    auto type = TRY(parse_type());
    return try_make_ref_counted<Parameter>(move(name), move(type));
}

ErrorOr<Vector<NonnullRefPtr<Statement>>> Parser::parse_block()
{
    TRY(consume([](Token const& token) {
        return token.type.has<SyntacticToken>() && token.type.get<SyntacticToken>().value == SyntacticToken::Value::OpenBrace;
    },
        "Expected '{'"sv));
    Vector<NonnullRefPtr<Statement>> statements;
    while (!current_token().type.has<SyntacticToken>() || current_token().type.get<SyntacticToken>().value != SyntacticToken::Value::CloseBrace) {
        statements.append(TRY(parse_statement()));
    }
    TRY(consume([](Token const& token) {
        return token.type.has<SyntacticToken>() && token.type.get<SyntacticToken>().value == SyntacticToken::Value::CloseBrace;
    },
        "Expected '}'"sv));
    return statements;
}

ErrorOr<NonnullRefPtr<Statement>> Parser::parse_statement()
{
    if (current_token().type.has<KeywordToken>()) {
        auto const& keyword = current_token().type.get<KeywordToken>();
        if (keyword.value == KeywordToken::Value::Var) {
            return TRY(parse_variable_statement());
        } else if (keyword.value == KeywordToken::Value::Return) {
            return TRY(parse_return_statement());
        }
    }
    if (current_token().type.has<IdentifierToken>()) {
        return TRY(parse_assignment_statement());
    }
    return Error::from_string_literal("Expected statement");
}

ErrorOr<NonnullRefPtr<Statement>> Parser::parse_variable_statement()
{
    TRY(consume([](Token const& token) {
        return token.type.has<KeywordToken>() && token.type.get<KeywordToken>().value == KeywordToken::Value::Var;
    },
        "Expected 'var'"sv));
    if (!current_token().type.has<IdentifierToken>()) {
        return Error::from_string_literal("Expected variable name");
    }
    String name = current_token().type.get<IdentifierToken>().value;
    advance();
    Optional<NonnullRefPtr<Type>> type;
    if (current_token().type.has<SyntacticToken>() && current_token().type.get<SyntacticToken>().value == SyntacticToken::Value::Colon) {
        advance();
        type = TRY(parse_type());
    }
    Optional<NonnullRefPtr<Expression>> initializer;
    if (current_token().type.has<SyntacticToken>() && current_token().type.get<SyntacticToken>().value == SyntacticToken::Value::Equals) {
        advance();
        initializer = TRY(parse_expression());
    }
    TRY(consume([](Token const& token) {
        return token.type.has<SyntacticToken>() && token.type.get<SyntacticToken>().value == SyntacticToken::Value::Semicolon;
    },
        "Expected ';'"sv));
    return try_make_ref_counted<VariableStatement>(move(name), move(type), move(initializer));
}

ErrorOr<NonnullRefPtr<Statement>> Parser::parse_assignment_statement()
{
    auto lhs = TRY(parse_expression());
    TRY(consume([](Token const& token) {
        return token.type.has<SyntacticToken>() && token.type.get<SyntacticToken>().value == SyntacticToken::Value::Equals;
    },
        "Expected '='"sv));
    auto rhs = TRY(parse_expression());
    TRY(consume([](Token const& token) {
        return token.type.has<SyntacticToken>() && token.type.get<SyntacticToken>().value == SyntacticToken::Value::Semicolon;
    },
        "Expected ';'"sv));
    return try_make_ref_counted<AssignmentStatement>(move(lhs), move(rhs));
}

ErrorOr<NonnullRefPtr<Statement>> Parser::parse_return_statement()
{
    TRY(consume([](Token const& token) {
        return token.type.has<KeywordToken>() && token.type.get<KeywordToken>().value == KeywordToken::Value::Return;
    },
        "Expected 'return'"sv));
    Optional<NonnullRefPtr<Expression>> expression;
    if (!current_token().type.has<SyntacticToken>() || current_token().type.get<SyntacticToken>().value != SyntacticToken::Value::Semicolon) {
        expression = TRY(parse_expression());
    }
    TRY(consume([](Token const& token) {
        return token.type.has<SyntacticToken>() && token.type.get<SyntacticToken>().value == SyntacticToken::Value::Semicolon;
    },
        "Expected ';'"sv));
    return try_make_ref_counted<ReturnStatement>(move(expression));
}

ErrorOr<NonnullRefPtr<Expression>> Parser::parse_expression()
{
    auto expr = TRY(parse_primary_expression());
    while (current_token().type.has<SyntacticToken>() && current_token().type.get<SyntacticToken>().value == SyntacticToken::Value::Dot) {
        expr = TRY(parse_member_access(move(expr)));
    }
    return expr;
}

ErrorOr<NonnullRefPtr<Expression>> Parser::parse_primary_expression()
{
    if (!current_token().type.has<IdentifierToken>()) {
        return Error::from_string_literal("Expected identifier");
    }
    String name = current_token().type.get<IdentifierToken>().value;
    advance();
    return try_make_ref_counted<IdentifierExpression>(move(name));
}

ErrorOr<NonnullRefPtr<Expression>> Parser::parse_member_access(NonnullRefPtr<Expression> base)
{
    TRY(consume([](Token const& token) {
        return token.type.has<SyntacticToken>() && token.type.get<SyntacticToken>().value == SyntacticToken::Value::Dot;
    },
        "Expected '.'"sv));
    if (!current_token().type.has<IdentifierToken>()) {
        return Error::from_string_literal("Expected member name");
    }
    String member = current_token().type.get<IdentifierToken>().value;
    advance();
    return try_make_ref_counted<MemberAccessExpression>(move(base), move(member));
}

}
