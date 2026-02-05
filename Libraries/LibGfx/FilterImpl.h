/*
 * Copyright (c) 2024-2025, Lucien Fiorini <lucienfiorini@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <core/SkColorFilter.h>
#include <effects/SkImageFilters.h>

namespace Gfx {

struct FilterImpl {
    sk_sp<SkImageFilter> filter;

    static NonnullOwnPtr<FilterImpl> create(sk_sp<SkImageFilter> filter)
    {
        return adopt_own(*new FilterImpl(move(filter)));
    }

    NonnullOwnPtr<FilterImpl> clone() const
    {
        return adopt_own(*new FilterImpl(filter));
    }
};

}
