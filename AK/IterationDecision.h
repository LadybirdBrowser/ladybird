/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Platform.h>

namespace AK {

enum class IterationDecision {
    Continue,
    Break,
};

}

#if USING_AK_GLOBALLY
using AK::IterationDecision;
#endif
