/*
 * Copyright (c) 2026, The Ladybird Developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/StringBuilder.h>
#include <AK/StringView.h>
#include <AK/Types.h>

namespace TestWeb {

ByteBuffer strip_sgr_sequences(StringView);

}
