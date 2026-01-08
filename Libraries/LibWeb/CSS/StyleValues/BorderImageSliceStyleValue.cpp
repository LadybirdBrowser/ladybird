/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "BorderImageSliceStyleValue.h"
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS {

void BorderImageSliceStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    top()->serialize(builder, mode);
    if (!first_is_equal_to_all_of(top(), right(), bottom(), left())) {
        builder.append(' ');
        right()->serialize(builder, mode);
        if (top() != bottom() || right() != left()) {
            builder.append(' ');
            bottom()->serialize(builder, mode);
            if (left() != right()) {
                builder.append(' ');
                left()->serialize(builder, mode);
            }
        }
    }

    if (fill())
        builder.append(" fill"sv);
}

}
