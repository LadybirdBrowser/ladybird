/*
 * Copyright (c) 2021-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/IdleDeadline.h>
#include <LibWeb/Bindings/RustInterface.h>

namespace Web::RequestIdleCallback {

class IdleDeadline final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(IdleDeadline, idle_deadline)

public:
    [[nodiscard]] static GC::Ref<IdleDeadline> create(bool did_timeout = false);
};

}
