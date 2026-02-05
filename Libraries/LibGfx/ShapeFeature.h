/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>

namespace Gfx {

struct ShapeFeature {
    char tag[4];
    u32 value;

    bool operator==(ShapeFeature const&) const = default;
    bool operator!=(ShapeFeature const&) const = default;
};

using ShapeFeatures = Vector<ShapeFeature, 4>;

}

namespace AK {

template<>
struct Traits<Gfx::ShapeFeatures> : public DefaultTraits<Gfx::ShapeFeatures> {
    static unsigned hash(Gfx::ShapeFeatures const& feature)
    {
        u32 hash = 0;

        for (auto const& feature : feature) {
            hash = pair_int_hash(hash, feature.tag[0]);
            hash = pair_int_hash(hash, feature.tag[1]);
            hash = pair_int_hash(hash, feature.tag[2]);
            hash = pair_int_hash(hash, feature.tag[3]);
            hash = pair_int_hash(hash, feature.value);
        }

        return hash;
    }
};

}
