/*
 * Copyright (c) 2021, Stephan Unverwerth <s.unverwerth@serenityos.org>
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
using Vector4 = VectorN<4, T>;
using DoubleVector4 = Vector4<double>;
using FloatVector4 = Vector4<float>;
using IntVector4 = Vector4<int>;

}

namespace AK {

template<typename T>
struct Formatter<Gfx::Vector4<T>> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Gfx::Vector4<T> const& value)
    {
        return Formatter<StringView>::format(builder, value.to_byte_string());
    }
};

}

using Gfx::DoubleVector4;
using Gfx::FloatVector4;
using Gfx::IntVector4;
using Gfx::Vector4;
