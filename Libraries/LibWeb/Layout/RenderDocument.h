/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/RefCounted.h>
#include <LibWeb/Layout/TreeBuilderRustFFI.h>

namespace Web::Layout {

// A document's render state as the host holds it: the DocumentHost that names the state. The state owns the document's
// style engine and layout arena, so the style engine bridge and the layout node arena both keep it alive, and it goes
// when the last of them does.
class RenderDocument : public RefCounted<RenderDocument> {
    AK_MAKE_NONCOPYABLE(RenderDocument);
    AK_MAKE_NONMOVABLE(RenderDocument);

public:
    static NonnullRefPtr<RenderDocument> create(u8 device_class)
    {
        return adopt_ref(*new RenderDocument(device_class));
    }

    ~RenderDocument()
    {
        RustFFI::document_host_destroy(m_host);
    }

    RustFFI::DocumentHost* host() const { return m_host; }

private:
    explicit RenderDocument(u8 device_class)
        : m_host(RustFFI::document_host_create(device_class))
    {
    }

    RustFFI::DocumentHost* m_host { nullptr };
};

}
