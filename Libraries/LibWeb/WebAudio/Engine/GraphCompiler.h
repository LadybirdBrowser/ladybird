/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebAudio/Engine/GraphExecutor.h>
#include <LibWeb/WebAudio/Engine/GraphResources.h>

namespace Web::WebAudio::Render {

class GraphCompiler {
public:
    static Render::GraphUpdateKind classify_update(Render::GraphDescription const& old_description, Render::GraphDescription const& new_description);

    static void build_nodes(GraphExecutor&, GraphResourceResolver const& resources);
    static GraphExecutor::Topology* build_topology(GraphExecutor&, Render::GraphDescription const&);

    static void rebuild_processing_order(GraphExecutor&, GraphExecutor::Topology&, Render::GraphDescription const&);
    static void rebuild_output_cache_capacity(GraphExecutor&);

    static Render::GraphUpdateKind classify_node_update(Render::GraphNodeDescription const& old_desc, Render::GraphNodeDescription const& new_desc);
};

}
