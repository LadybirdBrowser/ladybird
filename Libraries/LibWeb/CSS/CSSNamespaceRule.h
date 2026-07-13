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
    WEB_PLATFORM_OBJECT(CSSNamespaceRule, CSSRule);
    GC_DECLARE_ALLOCATOR(CSSNamespaceRule);

public:
    [[nodiscard]] static GC::Ref<CSSNamespaceRule> create(JS::Realm&, Optional<Utf16FlyString> prefix, Utf16FlyString namespace_uri);

    virtual ~CSSNamespaceRule() = default;

    void set_namespace_uri(Utf16FlyString value) { m_namespace_uri = move(value); }
    Utf16FlyString const& namespace_uri() const { return m_namespace_uri; }
    void set_prefix(Utf16FlyString value) { m_prefix = move(value); }
    Utf16FlyString const& prefix() const { return m_prefix; }

private:
    CSSNamespaceRule(JS::Realm&, Optional<Utf16FlyString> prefix, Utf16FlyString namespace_uri);

    virtual void initialize(JS::Realm&) override;
    virtual Utf16String serialized() const override;
    virtual void dump(StringBuilder&, int indent_levels) const override;

    Utf16FlyString m_namespace_uri;
    Utf16FlyString m_prefix;
};

}
