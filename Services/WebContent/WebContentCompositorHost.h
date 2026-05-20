/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <LibWeb/Compositor/CompositorHost.h>

namespace WebContent {

NonnullOwnPtr<Web::Compositor::CompositorHost> create_web_content_compositor_host();

}
