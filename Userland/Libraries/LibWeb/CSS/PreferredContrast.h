/*
 * Copyright (c) 2024, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>

namespace Web::CSS {

enum class PreferredContrast {
    Auto,
    Less,
    More,
    NoPreference,
};

PreferredContrast preferred_contrast_from_string(StringView);
StringView preferred_contrast_to_string(PreferredContrast);

}
