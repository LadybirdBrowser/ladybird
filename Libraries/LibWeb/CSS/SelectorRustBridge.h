/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibWeb/CSS/PseudoElement.h>

namespace Web::CSS {

class Selector;

namespace SelectorFFI {

struct RustSelector;

}

SelectorFFI::RustSelector* compile_selector_for_matching(Selector const&);
u8 pseudo_element_to_ffi(Optional<PseudoElement>);
Optional<PseudoElement> pseudo_element_from_ffi(u8);

}
