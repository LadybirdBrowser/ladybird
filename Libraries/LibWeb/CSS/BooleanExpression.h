/*
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWeb/Forward.h>

namespace Web::CSS {

// Corresponds to Kleene 3-valued logic.
enum class MatchResult {
    False,
    True,
    Unknown,
};

inline MatchResult as_match_result(bool value)
{
    return value ? MatchResult::True : MatchResult::False;
}

inline MatchResult negate(MatchResult value)
{
    switch (value) {
    case MatchResult::False:
        return MatchResult::True;
    case MatchResult::True:
        return MatchResult::False;
    case MatchResult::Unknown:
        return MatchResult::Unknown;
    }
    VERIFY_NOT_REACHED();
}

// The contents of this file implement the `<boolean-expr>` concept.
// https://drafts.csswg.org/css-values-5/#typedef-boolean-expr
class BooleanExpression {
public:
    virtual ~BooleanExpression() = default;

    bool evaluate_to_boolean(HTML::Window const*) const;
    static void indent(StringBuilder& builder, int levels);

    virtual MatchResult evaluate(HTML::Window const*) const = 0;
    virtual String to_string() const = 0;
    virtual void dump(StringBuilder&, int indent_levels = 0) const = 0;
};

// https://www.w3.org/TR/mediaqueries-4/#typedef-general-enclosed
class GeneralEnclosed final : public BooleanExpression {
public:
    static NonnullOwnPtr<GeneralEnclosed> create(String serialized_contents, MatchResult matches = MatchResult::Unknown)
    {
        return adopt_own(*new GeneralEnclosed(move(serialized_contents), matches));
    }
    virtual ~GeneralEnclosed() override = default;

    virtual MatchResult evaluate(HTML::Window const*) const override { return m_matches; }
    virtual String to_string() const override { return m_serialized_contents; }
    virtual void dump(StringBuilder&, int indent_levels = 0) const override;

private:
    GeneralEnclosed(String serialized_contents, MatchResult matches)
        : m_serialized_contents(move(serialized_contents))
        , m_matches(matches)
    {
    }

    String m_serialized_contents;
    MatchResult m_matches;
};

class BooleanNotExpression final : public BooleanExpression {
public:
    static NonnullOwnPtr<BooleanNotExpression> create(NonnullOwnPtr<BooleanExpression>&& child)
    {
        return adopt_own(*new BooleanNotExpression(move(child)));
    }
    virtual ~BooleanNotExpression() override = default;

    virtual MatchResult evaluate(HTML::Window const*) const override;
    virtual String to_string() const override;
    virtual void dump(StringBuilder&, int indent_levels = 0) const override;

private:
    BooleanNotExpression(NonnullOwnPtr<BooleanExpression>&& child)
        : m_child(move(child))
    {
    }

    NonnullOwnPtr<BooleanExpression> m_child;
};

class BooleanExpressionInParens final : public BooleanExpression {
public:
    static NonnullOwnPtr<BooleanExpressionInParens> create(NonnullOwnPtr<BooleanExpression>&& child)
    {
        return adopt_own(*new BooleanExpressionInParens(move(child)));
    }
    virtual ~BooleanExpressionInParens() override = default;

    virtual MatchResult evaluate(HTML::Window const*) const override;
    virtual String to_string() const override;
    virtual void dump(StringBuilder&, int indent_levels = 0) const override;

private:
    BooleanExpressionInParens(NonnullOwnPtr<BooleanExpression>&& child)
        : m_child(move(child))
    {
    }

    NonnullOwnPtr<BooleanExpression> m_child;
};

class BooleanAndExpression final : public BooleanExpression {
public:
    static NonnullOwnPtr<BooleanAndExpression> create(Vector<NonnullOwnPtr<BooleanExpression>>&& children)
    {
        return adopt_own(*new BooleanAndExpression(move(children)));
    }
    virtual ~BooleanAndExpression() override = default;

    virtual MatchResult evaluate(HTML::Window const*) const override;
    virtual String to_string() const override;
    virtual void dump(StringBuilder&, int indent_levels = 0) const override;

private:
    BooleanAndExpression(Vector<NonnullOwnPtr<BooleanExpression>>&& children)
        : m_children(move(children))
    {
    }

    Vector<NonnullOwnPtr<BooleanExpression>> m_children;
};

class BooleanOrExpression final : public BooleanExpression {
public:
    static NonnullOwnPtr<BooleanOrExpression> create(Vector<NonnullOwnPtr<BooleanExpression>>&& children)
    {
        return adopt_own(*new BooleanOrExpression(move(children)));
    }
    virtual ~BooleanOrExpression() override = default;

    virtual MatchResult evaluate(HTML::Window const*) const override;
    virtual String to_string() const override;
    virtual void dump(StringBuilder&, int indent_levels = 0) const override;

private:
    BooleanOrExpression(Vector<NonnullOwnPtr<BooleanExpression>>&& children)
        : m_children(move(children))
    {
    }

    Vector<NonnullOwnPtr<BooleanExpression>> m_children;
};

}

template<>
struct AK::Formatter<Web::CSS::BooleanExpression> : AK::Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::BooleanExpression const& expression)
    {
        return Formatter<StringView>::format(builder, expression.to_string());
    }
};
