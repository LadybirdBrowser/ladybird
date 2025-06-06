/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWGSL/AST.h>

namespace WGSL {

static String indent(size_t level)
{
    return MUST(String::repeated(' ', level * 2));
}

String IdentifierExpression::to_string(size_t indent) const
{
    return MUST(String::formatted("{}IdentifierExpression: name={}", WGSL::indent(indent), m_name));
}

bool IdentifierExpression::operator==(ASTNode const& other) const
{
    if (auto* other_ptr = dynamic_cast<IdentifierExpression const*>(&other)) {
        return m_name == other_ptr->m_name;
    }
    return false;
}

String MemberAccessExpression::to_string(size_t indent) const
{
    StringBuilder builder;
    builder.appendff("{}MemberAccessExpression:\n", WGSL::indent(indent));
    builder.appendff("{}object:\n", WGSL::indent(indent + 1));
    builder.appendff("{}{}", WGSL::indent(indent + 2), m_object->to_string(indent + 2));
    builder.appendff("\n{}member={}", WGSL::indent(indent + 1), m_member);
    return builder.to_string().value();
}

bool MemberAccessExpression::operator==(ASTNode const& other) const
{
    if (auto* other_ptr = dynamic_cast<MemberAccessExpression const*>(&other)) {
        return *m_object == *other_ptr->m_object && m_member == other_ptr->m_member;
    }
    return false;
}

String NamedType::to_string(size_t indent) const
{
    return MUST(String::formatted("{}NamedType: name={}", WGSL::indent(indent), m_name));
}

bool NamedType::operator==(ASTNode const& other) const
{
    if (auto* other_ptr = dynamic_cast<NamedType const*>(&other)) {
        return m_name == other_ptr->m_name;
    }
    return false;
}

String VectorType::to_string(size_t indent) const
{
    String kind;
    switch (m_kind) {
    case Kind::Vec3f:
        kind = "vec3f"_string;
        break;
    case Kind::Vec4f:
        kind = "vec4f"_string;
        break;
    }
    return MUST(String::formatted("{}VectorType: kind={}", WGSL::indent(indent), kind));
}

bool VectorType::operator==(ASTNode const& other) const
{
    if (auto* other_ptr = dynamic_cast<VectorType const*>(&other)) {
        return m_kind == other_ptr->m_kind;
    }
    return false;
}

String LocationAttribute::to_string(size_t indent) const
{
    return MUST(String::formatted("{}LocationAttribute: value={}", WGSL::indent(indent), m_value));
}

bool LocationAttribute::operator==(ASTNode const& other) const
{
    if (auto* other_ptr = dynamic_cast<LocationAttribute const*>(&other)) {
        return m_value == other_ptr->m_value;
    }
    return false;
}

String BuiltinAttribute::to_string(size_t indent) const
{
    String kind;
    switch (m_kind) {
    case Kind::Position:
        kind = "position"_string;
        break;
    }
    return MUST(String::formatted("{}BuiltinAttribute: kind={}", WGSL::indent(indent), kind));
}

bool BuiltinAttribute::operator==(ASTNode const& other) const
{
    if (auto* other_ptr = dynamic_cast<BuiltinAttribute const*>(&other)) {
        return m_kind == other_ptr->m_kind;
    }
    return false;
}

String VertexAttribute::to_string(size_t indent) const
{
    return MUST(String::formatted("{}VertexAttribute", WGSL::indent(indent)));
}

bool VertexAttribute::operator==(ASTNode const& other) const
{
    return dynamic_cast<VertexAttribute const*>(&other) != nullptr;
}

String FragmentAttribute::to_string(size_t indent) const
{
    return MUST(String::formatted("{}FragmentAttribute", WGSL::indent(indent)));
}

bool FragmentAttribute::operator==(ASTNode const& other) const
{
    return dynamic_cast<FragmentAttribute const*>(&other) != nullptr;
}

String StructMember::to_string(size_t indent) const
{
    StringBuilder builder;
    builder.appendff("{}StructMember: name={}", WGSL::indent(indent), m_name);
    if (!m_attributes.is_empty()) {
        builder.appendff("\n{}attributes:", WGSL::indent(indent + 1));
        for (size_t i = 0; i < m_attributes.size(); ++i) {
            builder.appendff("\n{}{}", WGSL::indent(indent + 2), m_attributes[i]->to_string(indent + 2));
        }
    }
    builder.appendff("\n{}type: {}", WGSL::indent(indent + 1), m_type->to_string(indent + 2));
    return builder.to_string().value();
}

bool StructMember::operator==(ASTNode const& other) const
{
    if (auto* other_ptr = dynamic_cast<StructMember const*>(&other)) {
        if (m_name != other_ptr->m_name || m_attributes.size() != other_ptr->m_attributes.size()) {
            return false;
        }
        for (size_t i = 0; i < m_attributes.size(); ++i) {
            if (*m_attributes[i] != *other_ptr->m_attributes[i]) {
                return false;
            }
        }
        return *m_type == *other_ptr->m_type;
    }
    return false;
}

String Parameter::to_string(size_t indent) const
{
    StringBuilder builder;
    builder.appendff("{}Parameter: name={}", WGSL::indent(indent), m_name);
    builder.appendff("\n{}type: {}", WGSL::indent(indent + 1), m_type->to_string(indent + 2));
    return builder.to_string().value();
}

bool Parameter::operator==(ASTNode const& other) const
{
    if (auto* other_ptr = dynamic_cast<Parameter const*>(&other)) {
        return m_name == other_ptr->m_name && *m_type == *other_ptr->m_type;
    }
    return false;
}

String VariableStatement::to_string(size_t indent) const
{
    StringBuilder builder;
    builder.appendff("{}VariableStatement:\n", WGSL::indent(indent));
    builder.appendff("{}VariableDeclaration: name={}", WGSL::indent(indent + 1), m_name);
    if (m_type.has_value()) {
        builder.appendff("\n{}type: {}", WGSL::indent(indent + 1), m_type.value()->to_string(indent + 2));
    }
    if (m_initializer.has_value()) {
        builder.appendff("\n{}initializer: {}", WGSL::indent(indent + 1), m_initializer.value()->to_string(indent + 2));
    }
    return builder.to_string().value();
}

bool VariableStatement::operator==(ASTNode const& other) const
{
    if (auto* other_ptr = dynamic_cast<VariableStatement const*>(&other)) {
        if (m_name != other_ptr->m_name || m_type.has_value() != other_ptr->m_type.has_value() || m_initializer.has_value() != other_ptr->m_initializer.has_value()) {
            return false;
        }
        if (m_type.has_value() && *m_type.value() != *other_ptr->m_type.value()) {
            return false;
        }
        if (m_initializer.has_value() && *m_initializer.value() != *other_ptr->m_initializer.value()) {
            return false;
        }
        return true;
    }
    return false;
}

String AssignmentStatement::to_string(size_t indent) const
{
    StringBuilder builder;
    builder.appendff("{}AssignmentStatement:\n", WGSL::indent(indent));
    builder.appendff("{}lhs: {}\n", WGSL::indent(indent + 1), m_lhs->to_string(indent + 2));
    builder.appendff("{}rhs: {}", WGSL::indent(indent + 1), m_rhs->to_string(indent + 2));
    return builder.to_string().value();
}

bool AssignmentStatement::operator==(ASTNode const& other) const
{
    if (auto* other_ptr = dynamic_cast<AssignmentStatement const*>(&other)) {
        return *m_lhs == *other_ptr->m_lhs && *m_rhs == *other_ptr->m_rhs;
    }
    return false;
}

String ReturnStatement::to_string(size_t indent) const
{
    StringBuilder builder;
    builder.appendff("{}ReturnStatement:", WGSL::indent(indent));
    if (m_expression.has_value()) {
        builder.appendff("\n{}expression: {}", WGSL::indent(indent + 1), m_expression.value()->to_string(indent + 2));
    }
    return builder.to_string().value();
}

bool ReturnStatement::operator==(ASTNode const& other) const
{
    if (auto* other_ptr = dynamic_cast<ReturnStatement const*>(&other)) {
        if (m_expression.has_value() != other_ptr->m_expression.has_value()) {
            return false;
        }
        if (m_expression.has_value()) {
            return *m_expression.value() == *other_ptr->m_expression.value();
        }
        return true;
    }
    return false;
}

String StructDeclaration::to_string(size_t indent) const
{
    StringBuilder builder;
    builder.appendff("{}StructDeclaration: name={}", WGSL::indent(indent), m_name);
    if (!m_members.is_empty()) {
        builder.appendff("\n{}members:", WGSL::indent(indent + 1));
        for (size_t i = 0; i < m_members.size(); ++i) {
            builder.appendff("\n{}{}", WGSL::indent(indent + 2), m_members[i]->to_string(indent + 2));
        }
    }
    return builder.to_string().value();
}

bool StructDeclaration::operator==(ASTNode const& other) const
{
    if (auto* other_ptr = dynamic_cast<StructDeclaration const*>(&other)) {
        if (m_name != other_ptr->m_name || m_members.size() != other_ptr->m_members.size()) {
            return false;
        }
        for (size_t i = 0; i < m_members.size(); ++i) {
            if (*m_members[i] != *other_ptr->m_members[i]) {
                return false;
            }
        }
        return true;
    }
    return false;
}

String FunctionDeclaration::to_string(size_t indent) const
{
    StringBuilder builder;
    builder.appendff("{}FunctionDeclaration: name={}", WGSL::indent(indent), m_name);
    if (!m_attributes.is_empty()) {
        builder.appendff("\n{}attributes:", WGSL::indent(indent + 1));
        for (size_t i = 0; i < m_attributes.size(); ++i) {
            builder.appendff("\n{}{}", WGSL::indent(indent + 2), m_attributes[i]->to_string(indent + 2));
        }
    }
    if (!m_parameters.is_empty()) {
        builder.appendff("\n{}parameters:", WGSL::indent(indent + 1));
        for (size_t i = 0; i < m_parameters.size(); ++i) {
            builder.appendff("\n{}{}", WGSL::indent(indent + 2), m_parameters[i]->to_string(indent + 2));
        }
    }
    if (m_return_type.has_value() || !m_return_attributes.is_empty()) {
        builder.appendff("\n{}return:", WGSL::indent(indent + 1));
        if (!m_return_attributes.is_empty()) {
            builder.appendff("\n{}attributes:", WGSL::indent(indent + 2));
            for (size_t i = 0; i < m_return_attributes.size(); ++i) {
                builder.appendff("\n{}{}", WGSL::indent(indent + 3), m_return_attributes[i]->to_string(indent + 3));
            }
        }
        if (m_return_type.has_value()) {
            builder.appendff("\n{}type: {}", WGSL::indent(indent + 2), m_return_type.value()->to_string(indent + 3));
        }
    }
    if (!m_body.is_empty()) {
        builder.appendff("\n{}body:", WGSL::indent(indent + 1));
        for (size_t i = 0; i < m_body.size(); ++i) {
            builder.appendff("\n{}{}", WGSL::indent(indent + 2), m_body[i]->to_string(indent + 2));
        }
    }
    return builder.to_string().value();
}

bool FunctionDeclaration::operator==(ASTNode const& other) const
{
    if (auto* other_ptr = dynamic_cast<FunctionDeclaration const*>(&other)) {
        if (m_attributes.size() != other_ptr->m_attributes.size() || m_parameters.size() != other_ptr->m_parameters.size() || m_return_attributes.size() != other_ptr->m_return_attributes.size() || m_body.size() != other_ptr->m_body.size() || m_name != other_ptr->m_name || m_return_type.has_value() != other_ptr->m_return_type.has_value()) {
            return false;
        }
        for (size_t i = 0; i < m_attributes.size(); ++i) {
            if (*m_attributes[i] != *other_ptr->m_attributes[i]) {
                return false;
            }
        }
        for (size_t i = 0; i < m_parameters.size(); ++i) {
            if (*m_parameters[i] != *other_ptr->m_parameters[i]) {
                return false;
            }
        }
        if (m_return_type.has_value() && *m_return_type.value() != *other_ptr->m_return_type.value()) {
            return false;
        }
        for (size_t i = 0; i < m_return_attributes.size(); ++i) {
            if (*m_return_attributes[i] != *other_ptr->m_return_attributes[i]) {
                return false;
            }
        }
        for (size_t i = 0; i < m_body.size(); ++i) {
            if (*m_body[i] != *other_ptr->m_body[i]) {
                return false;
            }
        }
        return true;
    }
    return false;
}

String Program::to_string(size_t indent) const
{
    StringBuilder builder;
    builder.appendff("{}Program:", WGSL::indent(indent));
    if (!declarations.is_empty()) {
        builder.appendff("\n{}declarations:", WGSL::indent(indent + 1));
        for (size_t i = 0; i < declarations.size(); ++i) {
            builder.appendff("\n{}{}", WGSL::indent(indent + 2), declarations[i]->to_string(indent + 2));
        }
    }
    return builder.to_string().value();
}

bool Program::operator==(Program const& other) const
{
    if (declarations.size() != other.declarations.size()) {
        return false;
    }
    for (size_t i = 0; i < declarations.size(); ++i) {
        if (*declarations[i] != *other.declarations[i]) {
            return false;
        }
    }
    return true;
}

}
