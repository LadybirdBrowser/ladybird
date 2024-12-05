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
};

}
