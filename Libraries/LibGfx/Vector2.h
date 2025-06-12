/*
 * Copyright (c) 2020, Stephan Unverwerth <s.unverwerth@serenityos.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "VectorN.h"

#include <AK/Error.h>
#include <AK/Format.h>
#include <AK/StringView.h>

namespace Gfx {

template<class T>
using Vector2 = VectorN<2, T>;
using DoubleVector2 = Vector2<double>;
using FloatVector2 = Vector2<float>;
using IntVector2 = Vector2<int>;

}

namespace AK {

template<typename T>
struct Formatter<Gfx::Vector2<T>> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Gfx::Vector2<T> const& value)
    {
        return Formatter<StringView>::format(builder, value.to_byte_string());
    }
};

}

using Gfx::DoubleVector2;
using Gfx::FloatVector2;
using Gfx::IntVector2;
using Gfx::Vector2;
