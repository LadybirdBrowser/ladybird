/*
 * Copyright (c) 2023, Emil Militzer <emil.militzer@posteo.de>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "DisplayStyleValue.h"

namespace Web::CSS {

ValueComparingNonnullRefPtr<DisplayStyleValue> DisplayStyleValue::create(Display const& display)
{
    return adopt_ref(*new (nothrow) DisplayStyleValue(display));
}

}
