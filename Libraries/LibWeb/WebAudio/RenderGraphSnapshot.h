/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibWeb/WebAudio/RenderGraphDescription.h>

namespace Web::WebAudio {

class AudioNode;

// Creates a render-thread-friendly snapshot of the current audio graph, rooted at the destination node.
RenderGraphDescription snapshot_render_graph(GC::Ref<AudioNode> destination_node, double context_sample_rate);

}
