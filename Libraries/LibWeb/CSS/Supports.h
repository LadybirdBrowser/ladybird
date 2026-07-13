/*
 * Copyright (c) 2021-2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/RefCounted.h>
#include <AK/String.h>
#include <AK/Utf16FlyString.h>
#include <LibWeb/CSS/BooleanExpression.h>
#include <LibWeb/Export.h>

namespace Web::CSS {

// https://www.w3.org/TR/css-conditional-3/#at-supports
class WEB_API Supports final : public RefCounted<Supports> {
public:
    class Declaration final : public BooleanExpression {
    public:
        static NonnullOwnPtr<Declaration> create(Utf16String declaration, bool matches)
        {
            return adopt_own(*new Declaration(move(declaration), matches));
        }
        virtual ~Declaration() override = default;

        virtual MatchResult evaluate(BooleanExpressionEvaluationContext const&) const override;
        virtual void serialize_to(Utf16StringBuilder&) const override;
        virtual void dump(StringBuilder&, int indent_levels = 0) const override;

    private:
        Declaration(Utf16String declaration, bool matches)
            : m_declaration(move(declaration))
            , m_matches(matches)
        {
        }
        Utf16String m_declaration;
        bool m_matches;
    };

    class Selector final : public BooleanExpression {
    public:
        static NonnullOwnPtr<Selector> create(Utf16String selector, bool matches)
        {
            return adopt_own(*new Selector(move(selector), matches));
        }
        virtual ~Selector() override = default;

        virtual MatchResult evaluate(BooleanExpressionEvaluationContext const&) const override;
        virtual void serialize_to(Utf16StringBuilder&) const override;
        virtual void dump(StringBuilder&, int indent_levels = 0) const override;

    private:
        Selector(Utf16String selector, bool matches)
            : m_selector(move(selector))
            , m_matches(matches)
        {
        }
        Utf16String m_selector;
        bool m_matches;
    };

    class FontTech final : public BooleanExpression {
    public:
        static NonnullOwnPtr<FontTech> create(Utf16FlyString tech, bool matches)
        {
            return adopt_own(*new FontTech(move(tech), matches));
        }
        virtual ~FontTech() override = default;

        virtual MatchResult evaluate(BooleanExpressionEvaluationContext const&) const override;
        virtual void serialize_to(Utf16StringBuilder&) const override;
        virtual void dump(StringBuilder&, int indent_levels = 0) const override;

    private:
        FontTech(Utf16FlyString tech, bool matches)
            : m_tech(move(tech))
            , m_matches(matches)
        {
        }
        Utf16FlyString m_tech;
        bool m_matches;
    };

    class FontFormat final : public BooleanExpression {
    public:
        static NonnullOwnPtr<FontFormat> create(Utf16FlyString format, bool matches)
        {
            return adopt_own(*new FontFormat(move(format), matches));
        }
        virtual ~FontFormat() override = default;

        virtual MatchResult evaluate(BooleanExpressionEvaluationContext const&) const override;
        virtual void serialize_to(Utf16StringBuilder&) const override;
        virtual void dump(StringBuilder&, int indent_levels = 0) const override;

    private:
        FontFormat(Utf16FlyString format, bool matches)
            : m_format(move(format))
            , m_matches(matches)
        {
        }
        Utf16FlyString m_format;
        bool m_matches;
    };

    class Env final : public BooleanExpression {
    public:
        static NonnullOwnPtr<Env> create(Utf16FlyString variable_name, bool matches)
        {
            return adopt_own(*new Env(move(variable_name), matches));
        }
        virtual ~Env() override = default;

        virtual MatchResult evaluate(BooleanExpressionEvaluationContext const&) const override;
        virtual void serialize_to(Utf16StringBuilder&) const override;
        virtual void dump(StringBuilder&, int indent_levels = 0) const override;

    private:
        Env(Utf16FlyString variable_name, bool matches)
            : m_variable_name(move(variable_name))
            , m_matches(matches)
        {
        }
        Utf16FlyString m_variable_name;
        bool m_matches;
    };

    class AtRule final : public BooleanExpression {
    public:
        static NonnullOwnPtr<AtRule> create(Utf16FlyString name, bool matches)
        {
            return adopt_own(*new AtRule(move(name), matches));
        }
        virtual ~AtRule() override = default;

        virtual MatchResult evaluate(BooleanExpressionEvaluationContext const&) const override;
        virtual void serialize_to(Utf16StringBuilder&) const override;
        virtual void dump(StringBuilder&, int indent_levels = 0) const override;

    private:
        AtRule(Utf16FlyString name, bool matches)
            : m_name(move(name))
            , m_matches(matches)
        {
        }

        Utf16FlyString m_name;
        bool m_matches;
    };

    static NonnullRefPtr<Supports> create(NonnullOwnPtr<BooleanExpression>&& condition)
    {
        return adopt_ref(*new Supports(move(condition)));
    }

    bool matches() const { return m_matches; }
    Utf16String to_string() const;

    void dump(StringBuilder&, int indent_levels = 0) const;

private:
    Supports(NonnullOwnPtr<BooleanExpression>&&);

    NonnullOwnPtr<BooleanExpression> m_condition;
    bool m_matches { false };
};

}
