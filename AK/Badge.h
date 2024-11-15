/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/Platform.h>

namespace AK {

template<typename T>
class Badge {
    AK_MAKE_NONCOPYABLE(Badge);
    AK_MAKE_NONMOVABLE(Badge);

public:
    using Type = T;

private:
    friend T;
    constexpr Badge() = default;
};

}

#if USING_AK_GLOBALLY
using AK::Badge;
#endif
