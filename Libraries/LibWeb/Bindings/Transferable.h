/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibIPC/Forward.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Bindings {

// https://html.spec.whatwg.org/multipage/structured-data.html#transferable-objects
class Transferable {
public:
    virtual ~Transferable() = default;

    // NOTE: It is an error to call Base::transfer_steps in your impl
    virtual WebIDL::ExceptionOr<void> transfer_steps(HTML::TransferDataHolder&) = 0;

    // NOTE: It is an error to call Base::transfer_receiving_steps in your impl
    virtual WebIDL::ExceptionOr<void> transfer_receiving_steps(HTML::TransferDataHolder&) = 0;

    virtual HTML::TransferType primary_interface() const = 0;

    bool is_detached() const { return m_detached; }
    void set_detached(bool b) { m_detached = b; }

private:
    // https://html.spec.whatwg.org/multipage/structured-data.html#detached
    bool m_detached = false;
};

}
