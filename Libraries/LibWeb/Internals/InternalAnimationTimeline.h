/*
 * Copyright (c) 2024, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/InternalAnimationTimeline.h>
#include <LibWeb/Bindings/RustInterface.h>

namespace Web::Internals {

// The cell of an InternalAnimationTimeline, which Bindings/Rust/src/internals implements.
class InternalAnimationTimeline final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(InternalAnimationTimeline, internal_animation_timeline)
};

}
