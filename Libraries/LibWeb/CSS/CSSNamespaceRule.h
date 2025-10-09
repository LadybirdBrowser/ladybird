/*
 * Copyright (c) 2023, Jonah Shafran <jonahshafran@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSRule.h>

namespace Web::CSS {

class CSSNamespaceRule final : public CSSRule {
    WEB_PLATFORM_OBJECT(CSSNamespaceRule, CSSRule);
    GC_DECLARE_ALLOCATOR(CSSNamespaceRule);

public:
    [[nodiscard]] static GC::Ref<CSSNamespaceRule> create(JS::Realm&, Optional<FlyString> const& prefix, FlyString namespace_uri);

    virtual ~CSSNamespaceRule() = default;

    void set_namespace_uri(FlyString value) { m_namespace_uri = move(value); }
    FlyString const& namespace_uri() const { return m_namespace_uri; }
    void set_prefix(FlyString value) { m_prefix = move(value); }
    FlyString const& prefix() const { return m_prefix; }

private:
    CSSNamespaceRule(JS::Realm&, Optional<FlyString> const& prefix, FlyString namespace_uri);

    virtual void initialize(JS::Realm&) override;

    virtual String serialized() const override;
    FlyString m_namespace_uri;
    FlyString m_prefix;
};

}
