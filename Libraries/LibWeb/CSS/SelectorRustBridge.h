/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::CSS {

class Selector;

namespace SelectorFFI {

struct RustSelector;

}

SelectorFFI::RustSelector* compile_selector_for_matching(Selector const&);

}
