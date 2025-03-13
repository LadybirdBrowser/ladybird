/*
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/RefCounted.h>
#include <AK/String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibWeb/CSS/GeneralEnclosed.h>

namespace Web::CSS {

// https://www.w3.org/TR/css-conditional-3/#at-supports
class Supports final : public RefCounted<Supports> {
public:
    struct Declaration {
        String declaration;
        bool matches;
        [[nodiscard]] bool evaluate() const { return matches; }
        String to_string() const;
        void dump(StringBuilder&, int indent_levels = 0) const;
    };

    struct Selector {
        String selector;
        bool matches;
        [[nodiscard]] bool evaluate() const { return matches; }
        String to_string() const;
        void dump(StringBuilder&, int indent_levels = 0) const;
    };

    struct Feature {
        Variant<Declaration, Selector> value;
        [[nodiscard]] bool evaluate() const;
        String to_string() const;
        void dump(StringBuilder&, int indent_levels = 0) const;
    };

    struct Condition;
    struct InParens {
        Variant<NonnullOwnPtr<Condition>, Feature, GeneralEnclosed> value;

        [[nodiscard]] bool evaluate() const;
        String to_string() const;
        void dump(StringBuilder&, int indent_levels = 0) const;
    };

    struct Condition {
        enum class Type {
            Not,
            And,
            Or,
        };
        Type type;
        Vector<InParens> children;

        [[nodiscard]] bool evaluate() const;
        String to_string() const;
        void dump(StringBuilder&, int indent_levels = 0) const;
    };

    static NonnullRefPtr<Supports> create(NonnullOwnPtr<Condition>&& condition)
    {
        return adopt_ref(*new Supports(move(condition)));
    }

    bool matches() const { return m_matches; }
    String to_string() const;

    void dump(StringBuilder&, int indent_levels = 0) const;

private:
    Supports(NonnullOwnPtr<Condition>&&);

    NonnullOwnPtr<Condition> m_condition;
    bool m_matches { false };
};

}

template<>
struct AK::Formatter<Web::CSS::Supports::InParens> : AK::Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::Supports::InParens const& in_parens)
    {
        return Formatter<StringView>::format(builder, in_parens.to_string());
    }
};
