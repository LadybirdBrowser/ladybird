/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/PercentageOr.h>

namespace Web::CSS {

class LengthBox {
public:
    LengthBox();
    LengthBox(LengthPercentage top, LengthPercentage right, LengthPercentage bottom, LengthPercentage left);
    ~LengthBox();

    // Length (and thus LengthPercentage) includes a RefPtr<CSSMathValue> member, but we can't include the header CSSStyleValue.h as it includes
    // this file already. To break the cyclic dependency, we must initialize these members in the constructor.
    LengthPercentage& top() { return m_top; }
    LengthPercentage& right() { return m_right; }
    LengthPercentage& bottom() { return m_bottom; }
    LengthPercentage& left() { return m_left; }
    LengthPercentage const& top() const { return m_top; }
    LengthPercentage const& right() const { return m_right; }
    LengthPercentage const& bottom() const { return m_bottom; }
    LengthPercentage const& left() const { return m_left; }

private:
    LengthPercentage m_top;
    LengthPercentage m_right;
    LengthPercentage m_bottom;
    LengthPercentage m_left;
};

}
