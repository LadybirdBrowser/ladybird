/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, Ben Wiederhake <BenWiederhake.GitHub@gmx.de>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "LengthBox.h"

namespace Web::CSS {

LengthBox::LengthBox()
    : m_top(Length::make_auto())
    , m_right(Length::make_auto())
    , m_bottom(Length::make_auto())
    , m_left(Length::make_auto())
{
}

LengthBox::LengthBox(LengthPercentage top, LengthPercentage right, LengthPercentage bottom, LengthPercentage left)
    : m_top(top)
    , m_right(right)
    , m_bottom(bottom)
    , m_left(left)
{
}

LengthBox::~LengthBox() = default;

}
