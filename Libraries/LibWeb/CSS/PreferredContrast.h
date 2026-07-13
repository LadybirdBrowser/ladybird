/*
 * Copyright (c) 2024-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16View.h>

namespace Web::CSS {

enum class PreferredContrast {
    Auto,
    Less,
    More,
    NoPreference,
};

PreferredContrast preferred_contrast_from_string(Utf16View);

}
