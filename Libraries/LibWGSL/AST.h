/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWGSL/Export.h>

namespace WGSL {

class WGSL_API ASTNode : public RefCounted<ASTNode> {
public:
    virtual ~ASTNode() = default;
    virtual String to_string(size_t indent = 0) const = 0;
    virtual bool operator==(ASTNode const& other) const = 0;
    bool operator!=(ASTNode const& other) const { return !(*this == other); }

protected:
    ASTNode() = default;
};

class WGSL_API Expression : public ASTNode {
public:
    ~Expression() override = default;

protected:
    Expression() = default;
};

class WGSL_API IdentifierExpression : public Expression {
public:
    explicit IdentifierExpression(String name)
        : m_name(move(name))
    {
    }
    String to_string(size_t indent = 0) const override;
    bool operator==(ASTNode const& other) const override;
    String const& name() const { return m_name; }

private:
    String m_name;
};

class WGSL_API MemberAccessExpression : public Expression {
public:
    MemberAccessExpression(NonnullRefPtr<Expression> object, String member)
        : m_object(move(object))
        , m_member(move(member))
    {
    }
    String to_string(size_t indent = 0) const override;
    bool operator==(ASTNode const& other) const override;
    NonnullRefPtr<Expression> const& object() const { return m_object; }
    String const& member() const { return m_member; }

private:
    NonnullRefPtr<Expression> m_object;
    String m_member;
};

class WGSL_API Type : public ASTNode {
public:
    ~Type() override = default;

protected:
    Type() = default;
};

class WGSL_API NamedType : public Type {
public:
    explicit NamedType(String name)
        : m_name(move(name))
    {
    }
    String to_string(size_t indent = 0) const override;
    bool operator==(ASTNode const& other) const override;
    String const& name() const { return m_name; }

private:
    String m_name;
};

class WGSL_API VectorType : public Type {
public:
    enum class Kind : u8 {
        Vec3f,
        Vec4f,
    };
    explicit VectorType(Kind kind)
        : m_kind(kind)
    {
    }
    String to_string(size_t indent = 0) const override;
    bool operator==(ASTNode const& other) const override;
    Kind kind() const { return m_kind; }

private:
    Kind m_kind;
};

class WGSL_API Attribute : public ASTNode {
public:
    ~Attribute() override = default;

protected:
    Attribute() = default;
};

class WGSL_API LocationAttribute : public Attribute {
public:
    explicit LocationAttribute(u32 value)
        : m_value(value)
    {
    }
    String to_string(size_t indent = 0) const override;
    bool operator==(ASTNode const& other) const override;
    u32 value() const { return m_value; }

private:
    u32 m_value;
};

class WGSL_API BuiltinAttribute : public Attribute {
public:
    enum class Kind : u8 {
        Position,
    };
    explicit BuiltinAttribute(Kind kind)
        : m_kind(kind)
    {
    }
    String to_string(size_t indent = 0) const override;
    bool operator==(ASTNode const& other) const override;
    Kind kind() const { return m_kind; }

private:
    Kind m_kind;
};

class WGSL_API VertexAttribute : public Attribute {
public:
    String to_string(size_t indent = 0) const override;
    bool operator==(ASTNode const& other) const override;
};

class WGSL_API FragmentAttribute : public Attribute {
public:
    String to_string(size_t indent = 0) const override;
    bool operator==(ASTNode const& other) const override;
};

struct WGSL_API StructMember : public ASTNode {
    explicit StructMember(Vector<NonnullRefPtr<Attribute>> attributes, String name, NonnullRefPtr<Type> type)
        : m_attributes(move(attributes))
        , m_name(move(name))
        , m_type(move(type))
    {
    }
    String to_string(size_t indent = 0) const override;
    bool operator==(ASTNode const& other) const override;

    Vector<NonnullRefPtr<Attribute>> const& attributes() const { return m_attributes; }
    String const& name() const { return m_name; }
    NonnullRefPtr<Type> const& type() const { return m_type; }

private:
    Vector<NonnullRefPtr<Attribute>> m_attributes;
    String m_name;
    NonnullRefPtr<Type> m_type;
};

struct WGSL_API Parameter : public ASTNode {
    explicit Parameter(String name, NonnullRefPtr<Type> type)
        : m_name(move(name))
        , m_type(move(type))
    {
    }
    String to_string(size_t indent = 0) const override;
    bool operator==(ASTNode const& other) const override;

    String const& name() const { return m_name; }
    NonnullRefPtr<Type> const& type() const { return m_type; }

private:
    String m_name;
    NonnullRefPtr<Type> m_type;
};

class WGSL_API Statement : public ASTNode {
public:
    ~Statement() override = default;

protected:
    Statement() = default;
};

class WGSL_API VariableStatement : public Statement {
public:
    explicit VariableStatement(String name,
        Optional<NonnullRefPtr<Type>> type,
        Optional<NonnullRefPtr<Expression>> initializer)
        : m_name(move(name))
        , m_type(move(type))
        , m_initializer(move(initializer))
    {
    }
    String to_string(size_t indent = 0) const override;
    bool operator==(ASTNode const& other) const override;

    String const& name() const { return m_name; }
    Optional<NonnullRefPtr<Type>> const& type() const { return m_type; }
    Optional<NonnullRefPtr<Expression>> const& initializer() const { return m_initializer; }

private:
    String m_name;
    Optional<NonnullRefPtr<Type>> m_type;
    Optional<NonnullRefPtr<Expression>> m_initializer;
};

class WGSL_API AssignmentStatement : public Statement {
public:
    AssignmentStatement(NonnullRefPtr<Expression> lhs, NonnullRefPtr<Expression> rhs)
        : m_lhs(move(lhs))
        , m_rhs(move(rhs))
    {
    }
    String to_string(size_t indent = 0) const override;
    bool operator==(ASTNode const& other) const override;
    NonnullRefPtr<Expression> const& lhs() const { return m_lhs; }
    NonnullRefPtr<Expression> const& rhs() const { return m_rhs; }

private:
    NonnullRefPtr<Expression> m_lhs;
    NonnullRefPtr<Expression> m_rhs;
};

class WGSL_API ReturnStatement : public Statement {
public:
    explicit ReturnStatement(Optional<NonnullRefPtr<Expression>> expression)
        : m_expression(move(expression))
    {
    }
    String to_string(size_t indent = 0) const override;
    bool operator==(ASTNode const& other) const override;
    Optional<NonnullRefPtr<Expression>> const& expression() const { return m_expression; }

private:
    Optional<NonnullRefPtr<Expression>> m_expression;
};

class WGSL_API Declaration : public ASTNode {
public:
    ~Declaration() override = default;

protected:
    Declaration() = default;
};

class WGSL_API StructDeclaration : public Declaration {
public:
    StructDeclaration(String name, Vector<NonnullRefPtr<StructMember>> members)
        : m_name(move(name))
        , m_members(move(members))
    {
    }
    String to_string(size_t indent = 0) const override;
    bool operator==(ASTNode const& other) const override;
    String const& name() const { return m_name; }
    Vector<NonnullRefPtr<StructMember>> const& members() const { return m_members; }

private:
    String m_name;
    Vector<NonnullRefPtr<StructMember>> m_members;
};

class WGSL_API FunctionDeclaration : public Declaration {
public:
    FunctionDeclaration(Vector<NonnullRefPtr<Attribute>> attributes, String name,
        Vector<NonnullRefPtr<Parameter>> parameters, Optional<NonnullRefPtr<Type>> return_type,
        Vector<NonnullRefPtr<Attribute>> return_attributes, Vector<NonnullRefPtr<Statement>> body)
        : m_attributes(move(attributes))
        , m_name(move(name))
        , m_parameters(move(parameters))
        , m_return_type(move(return_type))
        , m_return_attributes(move(return_attributes))
        , m_body(move(body))
    {
    }
    String to_string(size_t indent = 0) const override;
    bool operator==(ASTNode const& other) const override;
    Vector<NonnullRefPtr<Attribute>> const& attributes() const { return m_attributes; }
    String const& name() const { return m_name; }
    Vector<NonnullRefPtr<Parameter>> const& parameters() const { return m_parameters; }
    Optional<NonnullRefPtr<Type>> const& return_type() const { return m_return_type; }
    Vector<NonnullRefPtr<Attribute>> const& return_attributes() const { return m_return_attributes; }
    Vector<NonnullRefPtr<Statement>> const& body() const { return m_body; }

private:
    Vector<NonnullRefPtr<Attribute>> m_attributes;
    String m_name;
    Vector<NonnullRefPtr<Parameter>> m_parameters;
    Optional<NonnullRefPtr<Type>> m_return_type;
    Vector<NonnullRefPtr<Attribute>> m_return_attributes;
    Vector<NonnullRefPtr<Statement>> m_body;
};

struct WGSL_API Program {
    Vector<NonnullRefPtr<Declaration>> declarations;
    String to_string(size_t indent = 0) const;
    bool operator==(Program const& other) const;
};

}
