/*
 * Copyright (c) 2021, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2024, Tim Ledbetter <timledbetter@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Function.h>
#include <LibGC/RootVector.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/AbortSignal.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::DOM {

class WEB_API AbortSignal final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(AbortSignal, abort_signal)

public:
    static GC::Ref<AbortSignal> create();

    using AbortAlgorithmID = u64;
    Optional<AbortAlgorithmID> add_abort_algorithm(GC::Ref<GC::Function<void()>>);
    void remove_abort_algorithm(AbortAlgorithmID);

    bool aborted() const;
    JS::Value reason() const;
    void signal_abort(JS::Value reason, JS::Object& relevant_global_object);

    static WebIDL::ExceptionOr<GC::Ref<AbortSignal>> create_dependent_abort_signal(GC::RootVector<GC::Ref<AbortSignal>> const&);
};

}
