/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::DOM {

class Element;

}

namespace Web::CSS {

class InvalidationSet;

}

namespace Web::CSS::Invalidation {

bool element_matches_any_invalidation_set_property(DOM::Element const&, InvalidationSet const&);

}
