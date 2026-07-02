/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

class QWidget;

namespace Ladybird {

// Shows or hides a container widget that embeds a native window inside a host widget — keeping keyboard focus
// consistent: The container is the host's focus proxy only while it's visible.
void set_native_window_container_visible(QWidget& host, QWidget& container, bool visible);

}
