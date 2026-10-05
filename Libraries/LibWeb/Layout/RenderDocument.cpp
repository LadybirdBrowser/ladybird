/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleEngineBridge.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/RenderDocument.h>

namespace Web::Layout {

// The host's entries are called from here, in LibWeb, so that no target outside it reaches them.

RenderDocument::RenderDocument()
    : m_host(RustFFI::document_host_create())
    , m_read_scope_view(RustFFI::document_host_read_scope_view(m_host))
{
}

RenderDocument::~RenderDocument()
{
    RustFFI::document_host_destroy(m_host);
}

bool RenderDocument::frame_flies() const
{
    return RustFFI::document_host_frame_flies(m_host);
}

ForcedReadScope::ForcedReadScope(DOM::Document const& document)
    : ForcedReadScope(document.style_computer().style_engine().render_document())
{
}

}
