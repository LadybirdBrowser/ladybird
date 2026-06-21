/*
 * Copyright (c) 2022, mat
 * Copyright (c) 2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>

namespace Unicode {

enum class NormalizationForm {
    NFD,
    NFC,
    NFKD,
    NFKC
};
NormalizationForm normalization_form_from_string(StringView);
NormalizationForm normalization_form_from_string(Utf16View);

String normalize(StringView string, NormalizationForm form);
Utf16String normalize(Utf16View string, NormalizationForm form);

}
