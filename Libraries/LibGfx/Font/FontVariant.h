/*
 * Copyright (c) 2024, Johan Dahlin <jdahlin@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Optional.h>
#include <AK/StringView.h>
#include <AK/Vector.h>

#pragma once

namespace Gfx {

struct FontVariantAlternates {
    bool historical_forms { false };

    bool operator==(FontVariantAlternates const&) const = default;
};

}

namespace AK {

template<>
struct Traits<Gfx::FontVariantAlternates> : public DefaultTraits<Gfx::FontVariantAlternates> {
    static unsigned hash(Gfx::FontVariantAlternates const& data)
    {
        u32 hash = data.historical_forms ? 1 : 0;
        return hash;
    }
};

}
