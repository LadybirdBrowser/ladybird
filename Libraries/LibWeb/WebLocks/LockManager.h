/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/Bindings/LockManager.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Forward.h>

namespace Web::WebLocks {

class LockManager : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(LockManager, lock_manager)

public:
    static GC::Ref<LockManager> create(HTML::EnvironmentSettingsObject&);
};

}
