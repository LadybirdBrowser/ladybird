/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/PercentageOr.h>

namespace Web::CSS {

class LengthBox {
public:
    LengthBox();
    LengthBox(LengthPercentageOrAuto top, LengthPercentageOrAuto right, LengthPercentageOrAuto bottom, LengthPercentageOrAuto left);
    ~LengthBox();

    LengthPercentageOrAuto& top() { return m_top; }
    LengthPercentageOrAuto& right() { return m_right; }
    LengthPercentageOrAuto& bottom() { return m_bottom; }
    LengthPercentageOrAuto& left() { return m_left; }
    LengthPercentageOrAuto const& top() const { return m_top; }
    LengthPercentageOrAuto const& right() const { return m_right; }
    LengthPercentageOrAuto const& bottom() const { return m_bottom; }
    LengthPercentageOrAuto const& left() const { return m_left; }

    bool operator==(LengthBox const&) const = default;

private:
    LengthPercentageOrAuto m_top;
    LengthPercentageOrAuto m_right;
    LengthPercentageOrAuto m_bottom;
    LengthPercentageOrAuto m_left;
};

}
