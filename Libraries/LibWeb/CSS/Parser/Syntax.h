/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::CSS::Parser {

class WEB_API SyntaxNode : public RefCounted<SyntaxNode> {
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
    virtual Utf16String to_string() const = 0;

    virtual bool equals(SyntaxNode const& other) const = 0;
    virtual bool contains_value_type(ValueType) const = 0;
    bool operator==(SyntaxNode const& other) const
    {
        return this->equals(other);
    }

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
    static NonnullRefPtr<UniversalSyntaxNode> create()
    {
        return adopt_ref(*new UniversalSyntaxNode());
    }

    virtual ~UniversalSyntaxNode() override;
    virtual Utf16String to_string() const override;
    virtual bool equals(SyntaxNode const& other) const override;
    virtual bool contains_value_type(ValueType) const override;
    virtual void dump(StringBuilder&, int indent) const override;

private:
    UniversalSyntaxNode();
};

// 'foo'
class IdentSyntaxNode final : public SyntaxNode {
public:
    static NonnullRefPtr<IdentSyntaxNode> create(Utf16FlyString ident, CaseSensitivity case_sensitivity)
    {
        return adopt_ref(*new IdentSyntaxNode(move(ident), case_sensitivity));
    }

    virtual ~IdentSyntaxNode() override;
    Utf16FlyString const& ident() const { return m_ident; }
    CaseSensitivity case_sensitivity() const { return m_case_sensitivity; }

    virtual Utf16String to_string() const override;
    virtual bool equals(SyntaxNode const& other) const override;
    virtual bool contains_value_type(ValueType) const override;
    virtual void dump(StringBuilder&, int indent) const override;

private:
    IdentSyntaxNode(Utf16FlyString, CaseSensitivity);
    Utf16FlyString m_ident;
    CaseSensitivity m_case_sensitivity;
};

// '<foo>'
class TypeSyntaxNode final : public SyntaxNode {
public:
    static NonnullRefPtr<TypeSyntaxNode> create(Utf16FlyString type_name);
    virtual ~TypeSyntaxNode() override;

    Utf16FlyString const& type_name() const { return m_type_name; }
    Optional<ValueType> const& value_type() const { return m_value_type; }

    virtual Utf16String to_string() const override;
    virtual bool equals(SyntaxNode const& other) const override;
    virtual bool contains_value_type(ValueType) const override;
    virtual void dump(StringBuilder&, int indent) const override;

private:
    TypeSyntaxNode(Utf16FlyString type_name, Optional<ValueType> value_type);
    Utf16FlyString m_type_name;
    Optional<ValueType> m_value_type;
};

// '+'
class MultiplierSyntaxNode final : public SyntaxNode {
public:
    static NonnullRefPtr<MultiplierSyntaxNode> create(NonnullRefPtr<SyntaxNode> child)
    {
        return adopt_ref(*new MultiplierSyntaxNode(move(child)));
    }

    virtual ~MultiplierSyntaxNode() override;
    SyntaxNode const& child() const { return *m_child; }

    virtual Utf16String to_string() const override;
    virtual bool equals(SyntaxNode const& other) const override;
    virtual bool contains_value_type(ValueType) const override;
    virtual void dump(StringBuilder&, int indent) const override;

private:
    MultiplierSyntaxNode(NonnullRefPtr<SyntaxNode>);
    NonnullRefPtr<SyntaxNode> m_child;
};

// '#'
class CommaSeparatedMultiplierSyntaxNode final : public SyntaxNode {
public:
    static NonnullRefPtr<CommaSeparatedMultiplierSyntaxNode> create(NonnullRefPtr<SyntaxNode> child)
    {
        return adopt_ref(*new CommaSeparatedMultiplierSyntaxNode(move(child)));
    }

    virtual ~CommaSeparatedMultiplierSyntaxNode() override;
    SyntaxNode const& child() const { return *m_child; }

    virtual Utf16String to_string() const override;
    virtual bool equals(SyntaxNode const& other) const override;
    virtual bool contains_value_type(ValueType) const override;
    virtual void dump(StringBuilder&, int indent) const override;

private:
    CommaSeparatedMultiplierSyntaxNode(NonnullRefPtr<SyntaxNode>);
    NonnullRefPtr<SyntaxNode> m_child;
};

// Options separated by '|'
class AlternativesSyntaxNode final : public SyntaxNode {
public:
    static NonnullRefPtr<AlternativesSyntaxNode> create(Vector<NonnullRefPtr<SyntaxNode>> children)
    {
        return adopt_ref(*new AlternativesSyntaxNode(move(children)));
    }

    virtual ~AlternativesSyntaxNode() override;
    ReadonlySpan<NonnullRefPtr<SyntaxNode>> children() const { return m_children; }

    virtual Utf16String to_string() const override;
    virtual bool equals(SyntaxNode const& other) const override;
    virtual bool contains_value_type(ValueType) const override;
    virtual void dump(StringBuilder&, int indent) const override;

private:
    AlternativesSyntaxNode(Vector<NonnullRefPtr<SyntaxNode>>);
    Vector<NonnullRefPtr<SyntaxNode>> m_children;
};

}
