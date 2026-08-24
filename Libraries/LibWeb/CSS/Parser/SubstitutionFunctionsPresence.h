/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::CSS::Parser {

struct SubstitutionFunctionsPresence {
    bool attr { false };
    bool dashed_function { false };
    bool env { false };
    bool if_ { false };
    bool inherit { false };
    bool var { false };

    bool has_any() const { return attr || dashed_function || env || if_ || inherit || var; }
};

}
