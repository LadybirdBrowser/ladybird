/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

class QWidget;
class QWindow;

namespace Ladybird {

// Embeds a native window in a host widget. The native window only presents what is on screen, while the host takes all
// of its input and keeps the keyboard focus.
QWidget* create_native_window_container(QWindow& native_window, QWidget& host);

}
