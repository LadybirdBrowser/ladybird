/*
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace AK {

template<class T>
struct DefaultDelete {
    constexpr DefaultDelete() = default;

    constexpr void operator()(T* t)
    {
        delete t;
    }
};

template<class T>
struct DefaultDelete<T[]> {
    constexpr DefaultDelete() = default;

    constexpr void operator()(T* t)
    {
        delete[] t;
    }
};

}

#ifdef USING_AK_GLOBALLY
using AK::DefaultDelete;
#endif
