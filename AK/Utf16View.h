/*
 * Copyright (c) 2024, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/UnicodeCodePointView.h>
#include <AK/Utf16Mixin.h>
#include <AK/Wtf16View.h>

namespace AK {

class Utf16View : public UnicodeCodePointViewBase<Utf16View, u16, Wtf16View>
    , public Utf16Mixin<Utf16View> {
};

}
