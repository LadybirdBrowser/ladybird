/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "BorderImageSliceStyleValue.h"
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS {

String BorderImageSliceStyleValue::to_string(SerializationMode mode) const
{
    StringBuilder builder;
    if (first_is_equal_to_all_of(top(), right(), bottom(), left())) {
        builder.append(top()->to_string(mode));
    } else if (top() == bottom() && right() == left()) {
        builder.appendff("{} {}", top()->to_string(mode), right()->to_string(mode));
    } else if (left() == right()) {
        builder.appendff("{} {} {}", top()->to_string(mode), right()->to_string(mode), bottom()->to_string(mode));
    } else {
        builder.appendff("{} {} {} {}", top()->to_string(mode), right()->to_string(mode), bottom()->to_string(mode), left()->to_string(mode));
    }

    if (fill())
        builder.append(" fill"sv);

    return MUST(builder.to_string());
}

}
