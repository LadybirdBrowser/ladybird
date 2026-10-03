/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/RefCounted.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Layout/TreeBuilderRustFFI.h>

namespace Web::Layout {

// A document's render state as the host holds it: the DocumentHost that names the state. The state owns the document's
// style engine and layout arena, so the style engine bridge and the layout node arena both keep it alive, and it goes
// when the last of them does.
class WEB_API RenderDocument : public RefCounted<RenderDocument> {
    AK_MAKE_NONCOPYABLE(RenderDocument);
    AK_MAKE_NONMOVABLE(RenderDocument);

public:
    static NonnullRefPtr<RenderDocument> create(u8 device_class)
    {
        return adopt_ref(*new RenderDocument(device_class));
    }

    ~RenderDocument();

    RustFFI::DocumentHost* host() const { return m_host; }

    // Whether the host waits for the document's frame: it flies beside the host, or the layout round it flew with is
    // not paid yet. The host keeps it up to date, so asking costs a load.
    bool waits_for_frame() const { return *m_read_scope_view.waits_for_frame; }

    // Whether the document's frame flies beside the host, which has not taken it in yet.
    bool frame_flies() const;

private:
    friend class ForcedReadScope;

    explicit RenderDocument(u8 device_class);

    RustFFI::DocumentHost* m_host { nullptr };
    RustFFI::FfiReadScopeView m_read_scope_view;
};

// A scope of a read of a document's render state that the host waits for: a script API call's where `by_script`, and
// the host's own otherwise. The read's first job spends it, and only a read takes a frame in flight in. A scope begun
// inside the document's open read belongs to that read.
//
// The scope lends its read, a BegunRead, to the calls it makes. An entry that reaches the render state where the host
// is takes one, and only a scope hands one out, so code that reaches the render state without a begun read does not
// compile.
//
// The host hears of a scope only where it waits for the frame as the scope begins: one begun where it waits for none
// costs a load and a branch, and a frame that flies from inside it lands as the host's own read.
class WEB_API ForcedReadScope {
    AK_MAKE_NONCOPYABLE(ForcedReadScope);
    AK_MAKE_NONMOVABLE(ForcedReadScope);

public:
    ForcedReadScope(RenderDocument const& render_document, bool by_script)
        : m_render_document(render_document)
    {
        if (render_document.waits_for_frame()) [[unlikely]]
            begin_beside_frame(by_script);
    }

    // A read of the render state of `document`.
    ForcedReadScope(DOM::Document const& document, bool by_script);

    ~ForcedReadScope()
    {
        if (m_began_beside_frame) [[unlikely]]
            end_beside_frame();
    }

    operator BegunRead const&() const { return *m_render_document.m_read_scope_view.read; }
    operator BegunRead const*() const { return m_render_document.m_read_scope_view.read; }

private:
    // The host's entries are reached from LibWeb alone, so no other target links against them.
    void begin_beside_frame(bool by_script);
    void end_beside_frame();

    RenderDocument const& m_render_document;
    bool m_began_beside_frame { false };
};

}
