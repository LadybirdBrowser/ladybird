/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibIPC/Forward.h>

namespace Gfx {

struct ShapeFeature {
    char tag[4];
    u32 value;
    // Whether font-feature-settings set this feature, which keeps it in place over the optional ligatures that a
    // non-zero letter-spacing turns off: https://drafts.csswg.org/css-fonts-4/#feature-variation-precedence
    bool set_by_feature_settings { false };

    bool operator==(ShapeFeature const&) const = default;
    bool operator!=(ShapeFeature const&) const = default;
};

using ShapeFeatures = Vector<ShapeFeature, 4>;

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder&, Gfx::ShapeFeature const&);

template<>
ErrorOr<Gfx::ShapeFeature> decode(Decoder&);

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
            hash = pair_int_hash(hash, feature.set_by_feature_settings);
        }

        return hash;
    }
};

}
