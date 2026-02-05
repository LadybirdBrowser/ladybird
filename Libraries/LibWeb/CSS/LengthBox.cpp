/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Ben Wiederhake <BenWiederhake.GitHub@gmx.de>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LengthBox.h"

namespace Web::CSS {

LengthBox::LengthBox()
    : m_top(LengthPercentageOrAuto::make_auto())
    , m_right(LengthPercentageOrAuto::make_auto())
    , m_bottom(LengthPercentageOrAuto::make_auto())
    , m_left(LengthPercentageOrAuto::make_auto())
{
}

LengthBox::LengthBox(LengthPercentageOrAuto top, LengthPercentageOrAuto right, LengthPercentageOrAuto bottom, LengthPercentageOrAuto left)
    : m_top(move(top))
    , m_right(move(right))
    , m_bottom(move(bottom))
    , m_left(move(left))
{
}

LengthBox::~LengthBox() = default;

}
