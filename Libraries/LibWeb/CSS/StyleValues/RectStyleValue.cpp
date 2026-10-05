/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "RectStyleValue.h"

namespace Web::CSS {

ValueComparingNonnullRefPtr<RectStyleValue const> RectStyleValue::create(NonnullRefPtr<StyleValue const> top, NonnullRefPtr<StyleValue const> right, NonnullRefPtr<StyleValue const> bottom, NonnullRefPtr<StyleValue const> left)
{
    return adopt_ref(*new (nothrow) RectStyleValue(move(top), move(right), move(bottom), move(left)));
}

}
