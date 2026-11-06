/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/Lock.h>
#include <LibWeb/Bindings/RustInterface.h>

namespace Web::WebLocks {

class Lock : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(Lock, lock)
};

}
