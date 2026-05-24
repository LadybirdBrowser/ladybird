/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/RefPtr.h>
#include <LibWeb/Compositor/CompositorHost.h>
#include <WebContent/Forward.h>

namespace WebContent {

NonnullOwnPtr<Web::Compositor::CompositorHost> create_web_content_compositor_host(ConnectionFromClient&);

}
