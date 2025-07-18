/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSRule.h>

namespace Web::CSS {

class CSSCounterStyleRule : public CSSRule {
    WEB_PLATFORM_OBJECT(CSSCounterStyleRule, CSSRule);

public:
    virtual ~CSSCounterStyleRule() = default;

protected:
    CSSCounterStyleRule(JS::Realm&, Type);

    virtual void initialize(JS::Realm&) override;
};

}
