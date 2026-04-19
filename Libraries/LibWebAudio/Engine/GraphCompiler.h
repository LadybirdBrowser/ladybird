/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWebAudio/Engine/GraphDescription.h>
#include <LibWebAudio/Engine/GraphResources.h>

namespace Web::WebAudio::Render {

class GraphCompiler {
public:
    static Render::GraphUpdateKind classify_update(Render::GraphDescription const& old_description,
        Render::GraphDescription const& new_description);

    static RenderNodeSet compile_nodes(Render::GraphDescription const&, size_t quantum_size,
        GraphResources const& resources);
    static GraphTopology* build_topology(RenderNodeSet const&, Render::GraphDescription const&,
        size_t quantum_size);

    static Render::GraphUpdateKind classify_node_update(Render::GraphNodeDescription const& old_desc,
        Render::GraphNodeDescription const& new_desc);
};

} // namespace Web::WebAudio::Render
