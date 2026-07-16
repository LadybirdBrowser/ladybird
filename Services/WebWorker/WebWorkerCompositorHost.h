/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullOwnPtr.h>
#include <LibWeb/Compositor/CompositorHost.h>
#include <WebWorker/Forward.h>

namespace WebWorker {

NonnullOwnPtr<Web::Compositor::CompositorHost> create_web_worker_compositor_host(ConnectionFromClient&);

}
