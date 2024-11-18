/*
 * Copyright (c) 2024, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/UnicodeCodePointView.h>
#include <AK/Utf16Mixin.h>

namespace AK {

class Wtf16View : public UnicodeCodePointViewBase<Wtf16View, u16>
    , public Utf16Mixin<Wtf16View> {
};

}
