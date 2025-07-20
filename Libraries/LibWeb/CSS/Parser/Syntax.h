/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/Export.h>

namespace Web::CSS::Parser {

class WEB_API SyntaxNode {
public:
    enum class NodeType : u8 {
        Universal,
        Ident,
        Type,
        Multiplier,
        CommaSeparatedMultiplier,
        Alternatives,
    };

    NodeType type() const { return m_type; }

    virtual ~SyntaxNode() = default;
    virtual String to_string() const = 0;
    virtual void dump(StringBuilder&, int indent) const = 0;
    String dump() const;

protected:
    SyntaxNode(NodeType type)
        : m_type(type)
    {
    }

private:
    NodeType m_type;
};

// '*'
class UniversalSyntaxNode final : public SyntaxNode {
public:
    static NonnullOwnPtr<UniversalSyntaxNode> create()
    {
        return adopt_own(*new UniversalSyntaxNode());
    }

    virtual ~UniversalSyntaxNode() override;
    virtual String to_string() const override;
    virtual void dump(StringBuilder&, int indent) const override;

private:
    UniversalSyntaxNode();
};

// 'foo'
class IdentSyntaxNode final : public SyntaxNode {
public:
    static NonnullOwnPtr<IdentSyntaxNode> create(FlyString ident)
    {
        return adopt_own(*new IdentSyntaxNode(move(ident)));
    }

    virtual ~IdentSyntaxNode() override;
    FlyString const& ident() const { return m_ident; }

    virtual String to_string() const override;
    virtual void dump(StringBuilder&, int indent) const override;

private:
    IdentSyntaxNode(FlyString);
    FlyString m_ident;
};

// '<foo>'
class TypeSyntaxNode final : public SyntaxNode {
public:
    static NonnullOwnPtr<TypeSyntaxNode> create(FlyString type_name);
    virtual ~TypeSyntaxNode() override;

    FlyString const& type_name() const { return m_type_name; }
    Optional<ValueType> const& value_type() const { return m_value_type; }

    virtual String to_string() const override;
    virtual void dump(StringBuilder&, int indent) const override;

private:
    TypeSyntaxNode(FlyString type_name, Optional<ValueType> value_type);
    FlyString m_type_name;
    Optional<ValueType> m_value_type;
};

// '+'
class MultiplierSyntaxNode final : public SyntaxNode {
public:
    static NonnullOwnPtr<MultiplierSyntaxNode> create(NonnullOwnPtr<SyntaxNode> child)
    {
        return adopt_own(*new MultiplierSyntaxNode(move(child)));
    }

    virtual ~MultiplierSyntaxNode() override;
    SyntaxNode const& child() const { return *m_child; }

    virtual String to_string() const override;
    virtual void dump(StringBuilder&, int indent) const override;

private:
    MultiplierSyntaxNode(NonnullOwnPtr<SyntaxNode>);
    NonnullOwnPtr<SyntaxNode> m_child;
};

// '#'
class CommaSeparatedMultiplierSyntaxNode final : public SyntaxNode {
public:
    static NonnullOwnPtr<CommaSeparatedMultiplierSyntaxNode> create(NonnullOwnPtr<SyntaxNode> child)
    {
        return adopt_own(*new CommaSeparatedMultiplierSyntaxNode(move(child)));
    }

    virtual ~CommaSeparatedMultiplierSyntaxNode() override;
    SyntaxNode const& child() const { return *m_child; }

    virtual String to_string() const override;
    virtual void dump(StringBuilder&, int indent) const override;

private:
    CommaSeparatedMultiplierSyntaxNode(NonnullOwnPtr<SyntaxNode>);
    NonnullOwnPtr<SyntaxNode> m_child;
};

// Options separated by '|'
class AlternativesSyntaxNode final : public SyntaxNode {
public:
    static NonnullOwnPtr<AlternativesSyntaxNode> create(Vector<NonnullOwnPtr<SyntaxNode>> children)
    {
        return adopt_own(*new AlternativesSyntaxNode(move(children)));
    }

    virtual ~AlternativesSyntaxNode() override;
    ReadonlySpan<NonnullOwnPtr<SyntaxNode>> children() const { return m_children; }

    virtual String to_string() const override;
    virtual void dump(StringBuilder&, int indent) const override;

private:
    AlternativesSyntaxNode(Vector<NonnullOwnPtr<SyntaxNode>>);
    Vector<NonnullOwnPtr<SyntaxNode>> m_children;
};

}
