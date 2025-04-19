/*
 * Copyright (c) 2024, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>

namespace Crypto {

void fill_with_secure_random(Bytes);

template<typename T>
inline T get_secure_random()
{
    T t;
    fill_with_secure_random({ &t, sizeof(T) });
    return t;
}

}
