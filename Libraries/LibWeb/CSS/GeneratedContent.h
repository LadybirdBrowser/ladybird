/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::DOM {

class Node;

}

namespace Web::CSS {

bool subtree_affects_generated_content_state(DOM::Node const&);

}
