/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Web::DOM {

class Element;

}

namespace Web::CSS::Invalidation {

// A query container's size moved, so every element whose size query or container-relative unit
// resolved against it has to be recomputed. Which elements those are is not a selector question:
// the container is chosen by walking the flat tree upwards, so the dependents are its flat-tree
// descendants, and a container no query ever selected has none at all.
void invalidate_descendant_styles_depending_on_size_container_query(DOM::Element& query_container);

}
