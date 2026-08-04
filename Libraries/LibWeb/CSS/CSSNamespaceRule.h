/*
 * Copyright (c) 2023, Jonah Shafran <jonahshafran@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibWeb/CSS/CSSRule.h>

namespace Web::CSS {

class CSSNamespaceRule final : public CSSRule {
    WEB_WRAPPABLE(CSSNamespaceRule, CSSRule);
    GC_DECLARE_ALLOCATOR(CSSNamespaceRule);

public:
    [[nodiscard]] static GC::Ref<CSSNamespaceRule> create(Optional<Utf16FlyString> prefix, Utf16FlyString namespace_uri);

    virtual ~CSSNamespaceRule() = default;

    Utf16FlyString const& namespace_uri() const { return m_namespace_uri; }
    void set_prefix(Utf16FlyString value) { m_prefix = move(value); }
    Utf16FlyString const& prefix() const { return m_prefix; }

private:
    CSSNamespaceRule(Optional<Utf16FlyString> prefix, Utf16FlyString namespace_uri);

    virtual Utf16String serialized() const override;
    virtual void dump(StringBuilder&, int indent_levels) const override;

    Utf16FlyString m_namespace_uri;
    Utf16FlyString m_prefix;
};

}
