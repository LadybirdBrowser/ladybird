/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>

namespace WebView {

struct ReportDetail {
    String label;
    String value;
    // Values such as a commit hash or a source location, which read best in a fixed-width font.
    bool is_code { false };
};

}
