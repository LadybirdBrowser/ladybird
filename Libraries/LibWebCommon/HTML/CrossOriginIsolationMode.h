/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/document-sequences.html#cross-origin-isolation-mode
enum class CrossOriginIsolationMode : u8 {
    None,
    Logical,
    Concrete,
};

}
