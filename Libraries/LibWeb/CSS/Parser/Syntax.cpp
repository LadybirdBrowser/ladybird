/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Parser/Syntax.h>
#include <LibWeb/CSS/Serialize.h>

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

String UniversalSyntaxNode::to_string() const
{
    return "*"_string;
}

void UniversalSyntaxNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}Universal\n", "", indent);
}

NonnullOwnPtr<TypeSyntaxNode> TypeSyntaxNode::create(FlyString type_name)
{
    auto value_type = value_type_from_string(type_name);
    return adopt_own(*new TypeSyntaxNode(move(type_name), move(value_type)));
}

TypeSyntaxNode::TypeSyntaxNode(FlyString type_name, Optional<ValueType> value_type)
    : SyntaxNode(NodeType::Type)
    , m_type_name(move(type_name))
    , m_value_type(move(value_type))
{
}

TypeSyntaxNode::~TypeSyntaxNode() = default;

String TypeSyntaxNode::to_string() const
{
    return MUST(String::formatted("<{}>", m_type_name));
}

void TypeSyntaxNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}Type: {}\n", "", indent, m_type_name);
}

IdentSyntaxNode::IdentSyntaxNode(FlyString ident)
    : SyntaxNode(NodeType::Ident)
    , m_ident(move(ident))
{
}

IdentSyntaxNode::~IdentSyntaxNode() = default;

String IdentSyntaxNode::to_string() const
{
    return serialize_an_identifier(m_ident);
}

void IdentSyntaxNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}Ident: {}\n", "", indent, m_ident);
}

MultiplierSyntaxNode::MultiplierSyntaxNode(NonnullOwnPtr<SyntaxNode> child)
    : SyntaxNode(NodeType::Multiplier)
    , m_child(move(child))
{
}

MultiplierSyntaxNode::~MultiplierSyntaxNode() = default;

String MultiplierSyntaxNode::to_string() const
{
    return MUST(String::formatted("{}+", m_child->to_string()));
}

void MultiplierSyntaxNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}Multiplier:\n", "", indent);
    m_child->dump(builder, indent + 2);
}

CommaSeparatedMultiplierSyntaxNode::CommaSeparatedMultiplierSyntaxNode(NonnullOwnPtr<SyntaxNode> child)
    : SyntaxNode(NodeType::CommaSeparatedMultiplier)
    , m_child(move(child))
{
}

CommaSeparatedMultiplierSyntaxNode::~CommaSeparatedMultiplierSyntaxNode() = default;

String CommaSeparatedMultiplierSyntaxNode::to_string() const
{
    return MUST(String::formatted("{}#", m_child->to_string()));
}

void CommaSeparatedMultiplierSyntaxNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}CommaSeparatedMultiplier:\n", "", indent);
    m_child->dump(builder, indent + 2);
}

AlternativesSyntaxNode::AlternativesSyntaxNode(Vector<NonnullOwnPtr<SyntaxNode>> children)
    : SyntaxNode(NodeType::Alternatives)
    , m_children(move(children))
{
}

AlternativesSyntaxNode::~AlternativesSyntaxNode() = default;

String AlternativesSyntaxNode::to_string() const
{
    StringBuilder builder;

    bool first = true;
    for (auto const& child : m_children) {
        if (first) {
            first = false;
        } else {
            builder.append(" | "sv);
        }
        builder.append(child->to_string());
    }

    return builder.to_string_without_validation();
}

void AlternativesSyntaxNode::dump(StringBuilder& builder, int indent) const
{
    builder.appendff("{: >{}}Alternatives:\n", "", indent);
    for (auto const& child : m_children)
        child->dump(builder, indent + 2);
}

}
