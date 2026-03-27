/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <AK/RefCounted.h>
#include <LibWeb/CSS/BooleanExpression.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-conditional-5/#container-rule
class WEB_API ContainerQuery final : public RefCounted<ContainerQuery> {
public:
    static NonnullRefPtr<ContainerQuery> create(NonnullOwnPtr<BooleanExpression>&&);

    bool matches() const { return m_matches; }
    String to_string() const;

    void dump(StringBuilder&, int indent_levels = 0) const;

private:
    explicit ContainerQuery(NonnullOwnPtr<BooleanExpression>&&);

    NonnullOwnPtr<BooleanExpression> m_condition;
    bool m_matches { false };
};

}
