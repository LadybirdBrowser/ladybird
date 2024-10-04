/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>

namespace AK {

template<typename T>
class Badge {
public:
    using Type = T;

private:
    friend T;
    constexpr Badge() = default;

    Badge(Badge const&) = delete;
    Badge& operator=(Badge const&) = delete;

    Badge(Badge&&) = delete;
    Badge& operator=(Badge&&) = delete;
};

}

#if USING_AK_GLOBALLY
using AK::Badge;
#endif
