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

struct FontVariantEastAsian {
    enum class Variant {
        Unset,
        Jis78,
        Jis83,
        Jis90,
        Jis04,
        Simplified,
        Traditional
    };
    enum class Width {
        Unset,
        Proportional,
        FullWidth
    };

    bool ruby = false;
    Variant variant { Variant::Unset };
    Width width { Width::Unset };

    bool operator==(FontVariantEastAsian const&) const = default;
};

struct FontVariantLigatures {
    enum class Common {
        Unset,
        Common,
        NoCommon
    };
    enum class Discretionary {
        Unset,
        Discretionary,
        NoDiscretionary
    };
    enum class Historical {
        Unset,
        Historical,
        NoHistorical
    };
    enum class Contextual {
        Unset,
        Contextual,
        NoContextual
    };
    bool none = false;
    Common common { Common::Unset };
    Discretionary discretionary { Discretionary::Unset };
    Historical historical { Historical::Unset };
    Contextual contextual { Contextual::Unset };

    bool operator==(FontVariantLigatures const&) const = default;
};

struct FontVariantNumeric {
    enum class Figure {
        Unset,
        Lining,
        Oldstyle
    };
    enum class Spacing {
        Unset,
        Proportional,
        Tabular
    };
    enum class Fraction {
        Unset,
        Diagonal,
        Stacked
    };
    bool ordinal = false;
    bool slashed_zero = false;
    Figure figure { Figure::Unset };
    Spacing spacing { Spacing::Unset };
    Fraction fraction { Fraction::Unset };

    bool operator==(FontVariantNumeric const&) const = default;
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

template<>
struct Traits<Gfx::FontVariantEastAsian> : public DefaultTraits<Gfx::FontVariantEastAsian> {
    static unsigned hash(Gfx::FontVariantEastAsian const& data)
    {
        u32 hash = data.ruby ? 1 : 0;
        hash = pair_int_hash(hash, to_underlying(data.variant));
        hash = pair_int_hash(hash, to_underlying(data.width));
        return hash;
    }
};

template<>
struct Traits<Gfx::FontVariantLigatures> : public DefaultTraits<Gfx::FontVariantLigatures> {
    static unsigned hash(Gfx::FontVariantLigatures const& data)
    {
        u32 hash = data.none ? 1 : 0;
        hash = pair_int_hash(hash, to_underlying(data.common));
        hash = pair_int_hash(hash, to_underlying(data.discretionary));
        hash = pair_int_hash(hash, to_underlying(data.historical));
        hash = pair_int_hash(hash, to_underlying(data.contextual));
        return hash;
    }
};

template<>
struct Traits<Gfx::FontVariantNumeric> : public DefaultTraits<Gfx::FontVariantNumeric> {
    static unsigned hash(Gfx::FontVariantNumeric const& data)
    {
        u32 hash = data.ordinal ? 1 : 0;
        hash = pair_int_hash(hash, data.slashed_zero ? 1 : 0);
        hash = pair_int_hash(hash, to_underlying(data.figure));
        hash = pair_int_hash(hash, to_underlying(data.spacing));
        hash = pair_int_hash(hash, to_underlying(data.fraction));
        return hash;
    }
};

}
