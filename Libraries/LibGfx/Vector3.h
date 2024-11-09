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
using Vector3 = VectorN<3, T>;
using DoubleVector3 = Vector3<double>;
using FloatVector3 = Vector3<float>;
using IntVector3 = Vector3<int>;

}

namespace AK {

template<typename T>
struct Formatter<Gfx::Vector3<T>> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Gfx::Vector3<T> const& value)
    {
        return Formatter<StringView>::format(builder, value.to_byte_string());
    }
};

}

using Gfx::DoubleVector3;
using Gfx::FloatVector3;
using Gfx::IntVector3;
using Gfx::Vector3;
