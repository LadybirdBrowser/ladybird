/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Performance.h>
#include <LibWeb/Bindings/RustInterface.h>

namespace Web::HighResolutionTime {

class Performance final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(Performance, performance)
};

}
