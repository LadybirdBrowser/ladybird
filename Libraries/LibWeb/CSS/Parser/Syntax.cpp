/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Parser/Syntax.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/ValueType.h>

namespace Web::CSS::Parser {

String SyntaxNode::dump() const
{
    StringBuilder builder;
    dump(builder, 0);
    return builder.to_string_without_validation();
}

UniversalSyntaxNode::UniversalSyntaxNode()
    : SyntaxNode(NodeType::Universal)
{
}

UniversalSyntaxNode::~UniversalSyntaxNode() = default;

Utf16String UniversalSyntaxNode::to_string() const
{
    return "*"_utf16;
}

bool UniversalSyntaxNode::equals(SyntaxNode const& other) const
{
    return other.type() == type();
}

bool UniversalSyntaxNode::contains_value_type(ValueType) const
{
    return false;
}

void UniversalSyntaxNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}Universal\n", "", indent);
}

NonnullRefPtr<TypeSyntaxNode> TypeSyntaxNode::create(Utf16FlyString type_name)
{
    auto value_type = value_type_from_string(type_name);
    return adopt_ref(*new TypeSyntaxNode(move(type_name), move(value_type)));
}

TypeSyntaxNode::TypeSyntaxNode(Utf16FlyString type_name, Optional<ValueType> value_type)
    : SyntaxNode(NodeType::Type)
    , m_type_name(move(type_name))
    , m_value_type(move(value_type))
{
}

TypeSyntaxNode::~TypeSyntaxNode() = default;

Utf16String TypeSyntaxNode::to_string() const
{
    Utf16StringBuilder builder;
    builder.append_ascii('<');
    builder.append(m_type_name);
    builder.append_ascii('>');
    return builder.to_string();
}

bool TypeSyntaxNode::equals(SyntaxNode const& other) const
{
    if (other.type() != type())
        return false;
    auto const& other_type = static_cast<TypeSyntaxNode const&>(other);
    // No need to compare m_type_name, because it corresponds exactly to m_value_type.
    return m_value_type == other_type.m_value_type;
}

bool TypeSyntaxNode::contains_value_type(ValueType value_type) const
{
    return m_value_type == value_type;
}

void TypeSyntaxNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}Type: {}\n", "", indent, m_type_name);
}

IdentSyntaxNode::IdentSyntaxNode(Utf16FlyString ident, CaseSensitivity case_sensitivity)
    : SyntaxNode(NodeType::Ident)
    , m_ident(move(ident))
    , m_case_sensitivity(case_sensitivity)
{
}

IdentSyntaxNode::~IdentSyntaxNode() = default;

Utf16String IdentSyntaxNode::to_string() const
{
    return serialize_an_identifier_to_utf16(m_ident);
}

bool IdentSyntaxNode::equals(SyntaxNode const& other) const
{
    if (other.type() != type())
        return false;
    auto const& other_ident = static_cast<IdentSyntaxNode const&>(other);
    return m_ident == other_ident.m_ident
        && m_case_sensitivity == other_ident.m_case_sensitivity;
}

bool IdentSyntaxNode::contains_value_type(ValueType) const
{
    return false;
}

void IdentSyntaxNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}Ident: {}\n", "", indent, m_ident);
}

MultiplierSyntaxNode::MultiplierSyntaxNode(NonnullRefPtr<SyntaxNode> child)
    : SyntaxNode(NodeType::Multiplier)
    , m_child(move(child))
{
}

MultiplierSyntaxNode::~MultiplierSyntaxNode() = default;

Utf16String MultiplierSyntaxNode::to_string() const
{
    Utf16StringBuilder builder;
    builder.append(m_child->to_string());
    builder.append_ascii('+');
    return builder.to_string();
}

bool MultiplierSyntaxNode::equals(SyntaxNode const& other) const
{
    if (other.type() != type())
        return false;
    auto const& other_multiplier = static_cast<MultiplierSyntaxNode const&>(other);
    return m_child->equals(other_multiplier.child());
}

bool MultiplierSyntaxNode::contains_value_type(ValueType value_type) const
{
    return m_child->contains_value_type(value_type);
}

void MultiplierSyntaxNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}Multiplier:\n", "", indent);
    m_child->dump(builder, indent + 2);
}

CommaSeparatedMultiplierSyntaxNode::CommaSeparatedMultiplierSyntaxNode(NonnullRefPtr<SyntaxNode> child)
    : SyntaxNode(NodeType::CommaSeparatedMultiplier)
    , m_child(move(child))
{
}

CommaSeparatedMultiplierSyntaxNode::~CommaSeparatedMultiplierSyntaxNode() = default;

Utf16String CommaSeparatedMultiplierSyntaxNode::to_string() const
{
    Utf16StringBuilder builder;
    builder.append(m_child->to_string());
    builder.append_ascii('#');
    return builder.to_string();
}

bool CommaSeparatedMultiplierSyntaxNode::equals(SyntaxNode const& other) const
{
    if (other.type() != type())
        return false;
    auto const& other_multiplier = static_cast<CommaSeparatedMultiplierSyntaxNode const&>(other);
    return m_child->equals(other_multiplier.child());
}

bool CommaSeparatedMultiplierSyntaxNode::contains_value_type(ValueType value_type) const
{
    return m_child->contains_value_type(value_type);
}

void CommaSeparatedMultiplierSyntaxNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}CommaSeparatedMultiplier:\n", "", indent);
    m_child->dump(builder, indent + 2);
}

AlternativesSyntaxNode::AlternativesSyntaxNode(Vector<NonnullRefPtr<SyntaxNode>> children)
    : SyntaxNode(NodeType::Alternatives)
    , m_children(move(children))
{
}

AlternativesSyntaxNode::~AlternativesSyntaxNode() = default;

Utf16String AlternativesSyntaxNode::to_string() const
{
    Utf16StringBuilder builder;

    bool first = true;
    for (auto const& child : m_children) {
        if (first) {
            first = false;
        } else {
            builder.append(" | "sv);
        }
        builder.append(child->to_string());
    }

    return builder.to_string();
}

bool AlternativesSyntaxNode::equals(SyntaxNode const& other) const
{
    if (other.type() != type())
        return false;
    auto const& other_alternatives = static_cast<AlternativesSyntaxNode const&>(other);
    if (m_children.size() != other_alternatives.m_children.size())
        return false;
    for (size_t i = 0; i < m_children.size(); ++i) {
        if (!m_children[i]->equals(other_alternatives.m_children[i]))
            return false;
    }
    return true;
}

bool AlternativesSyntaxNode::contains_value_type(ValueType value_type) const
{
    for (auto const& child : m_children) {
        if (child->contains_value_type(value_type))
            return true;
    }
    return false;
}

void AlternativesSyntaxNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}Alternatives:\n", "", indent);
    for (auto const& child : m_children)
        child->dump(builder, indent + 2);
}

}
