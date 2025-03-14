/*
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/RefCounted.h>
#include <AK/String.h>
#include <LibWeb/CSS/BooleanExpression.h>

namespace Web::CSS {

// https://www.w3.org/TR/css-conditional-3/#at-supports
class Supports final : public RefCounted<Supports> {
public:
    class Declaration final : public BooleanExpression {
    public:
        static NonnullOwnPtr<Declaration> create(String declaration, bool matches)
        {
            return adopt_own(*new Declaration(move(declaration), matches));
        }
        virtual ~Declaration() override = default;

        virtual MatchResult evaluate(HTML::Window const*) const override;
        virtual String to_string() const override;
        virtual void dump(StringBuilder&, int indent_levels = 0) const override;

    private:
        Declaration(String declaration, bool matches)
            : m_declaration(move(declaration))
            , m_matches(matches)
        {
        }
        String m_declaration;
        bool m_matches;
    };

    class Selector final : public BooleanExpression {
    public:
        static NonnullOwnPtr<Selector> create(String selector, bool matches)
        {
            return adopt_own(*new Selector(move(selector), matches));
        }
        virtual ~Selector() override = default;

        virtual MatchResult evaluate(HTML::Window const*) const override;
        virtual String to_string() const override;
        virtual void dump(StringBuilder&, int indent_levels = 0) const override;

    private:
        Selector(String selector, bool matches)
            : m_selector(move(selector))
            , m_matches(matches)
        {
        }
        String m_selector;
        bool m_matches;
    };

    class FontTech final : public BooleanExpression {
    public:
        static NonnullOwnPtr<FontTech> create(FlyString tech, bool matches)
        {
            return adopt_own(*new FontTech(move(tech), matches));
        }
        virtual ~FontTech() override = default;

        virtual MatchResult evaluate(HTML::Window const*) const override;
        virtual String to_string() const override;
        virtual void dump(StringBuilder&, int indent_levels = 0) const override;

    private:
        FontTech(FlyString tech, bool matches)
            : m_tech(move(tech))
            , m_matches(matches)
        {
        }
        FlyString m_tech;
        bool m_matches;
    };

    class FontFormat final : public BooleanExpression {
    public:
        static NonnullOwnPtr<FontFormat> create(FlyString format, bool matches)
        {
            return adopt_own(*new FontFormat(move(format), matches));
        }
        virtual ~FontFormat() override = default;

        virtual MatchResult evaluate(HTML::Window const*) const override;
        virtual String to_string() const override;
        virtual void dump(StringBuilder&, int indent_levels = 0) const override;

    private:
        FontFormat(FlyString format, bool matches)
            : m_format(move(format))
            , m_matches(matches)
        {
        }
        FlyString m_format;
        bool m_matches;
    };

    static NonnullRefPtr<Supports> create(NonnullOwnPtr<BooleanExpression>&& condition)
    {
        return adopt_ref(*new Supports(move(condition)));
    }

    bool matches() const { return m_matches; }
    String to_string() const;

    void dump(StringBuilder&, int indent_levels = 0) const;

private:
    Supports(NonnullOwnPtr<BooleanExpression>&&);

    NonnullOwnPtr<BooleanExpression> m_condition;
    bool m_matches { false };
};

}
