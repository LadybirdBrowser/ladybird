/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWGSL/AST.h>
#include <LibWGSL/Export.h>
#include <LibWGSL/Lexer.h>

namespace WGSL {

class WGSL_API Parser {
public:
    explicit Parser(Vector<Token> tokens);
    ErrorOr<Program> parse();

private:
    Vector<Token> m_tokens;
    size_t m_current_index = 0;

    Token const& current_token() const;
    Token const& peek_token(size_t offset = 1) const;
    bool is_at_end() const;
    void advance();
    bool match(auto&& predicate);
    ErrorOr<void> consume(auto&& predicate, StringView error_message);

    ErrorOr<NonnullRefPtr<Declaration>> parse_declaration();
    ErrorOr<NonnullRefPtr<Declaration>> parse_struct_declaration();
    ErrorOr<NonnullRefPtr<Declaration>> parse_function_declaration(Vector<NonnullRefPtr<Attribute>> attributes);
    ErrorOr<Vector<NonnullRefPtr<Attribute>>> parse_attributes();
    ErrorOr<NonnullRefPtr<Attribute>> parse_attribute();
    ErrorOr<NonnullRefPtr<Type>> parse_type();
    ErrorOr<NonnullRefPtr<StructMember>> parse_struct_member();
    ErrorOr<NonnullRefPtr<Parameter>> parse_parameter();
    ErrorOr<Vector<NonnullRefPtr<Statement>>> parse_block();
    ErrorOr<NonnullRefPtr<Statement>> parse_statement();
    ErrorOr<NonnullRefPtr<Statement>> parse_variable_statement();
    ErrorOr<NonnullRefPtr<Statement>> parse_assignment_statement();
    ErrorOr<NonnullRefPtr<Statement>> parse_return_statement();
    ErrorOr<NonnullRefPtr<Expression>> parse_expression();
    ErrorOr<NonnullRefPtr<Expression>> parse_primary_expression();
    ErrorOr<NonnullRefPtr<Expression>> parse_member_access(NonnullRefPtr<Expression> base);
};

}
