/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/document-sequences.html#cross-origin-isolation-mode
enum class CrossOriginIsolationMode {
    None,
    Logical,
    Concrete,
};

}
