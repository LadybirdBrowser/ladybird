/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCompositing/MouseButton.h>

namespace Web::UIEvents {

using Compositing::MouseButton;
using enum Compositing::MouseButton;
using Compositing::button_code_to_mouse_button;
using Compositing::mouse_button_to_button_code;

}
