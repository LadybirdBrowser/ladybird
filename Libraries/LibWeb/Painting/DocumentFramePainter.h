/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <LibGfx/Rect.h>
#include <LibPaintServer/Types.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/PaintConfig.h>
#include <LibWeb/Painting/ScrollState.h>

namespace Web::DOM {

class Document;

}

namespace Web::Painting {

struct DocumentFramePainterResult {
    RefPtr<DisplayList> display_list;
    ScrollStateSnapshot scroll_state_snapshot;
};

struct DeferredPaintCommandSubmission {
    Function<Optional<PaintServer::ReleaseToken>()> submit;
    Vector<NonnullRefPtr<ExternalContentSource>> external_content_sources;
};

WEB_API bool has_pending_scroll_state_update(DOM::Document&);
WEB_API void clear_pending_scroll_state_update(DOM::Document&);
WEB_API Optional<DeferredPaintCommandSubmission> prepare_paint_commands_for_document_frame(DOM::Document&, HTML::PaintConfig, u64 painting_id, PaintServer::FrameOutputType = PaintServer::FrameOutputType::Presentation, PaintServer::OffscreenRenderTarget = {});
WEB_API Optional<PaintServer::ReleaseToken> submit_paint_commands_for_document_frame(DOM::Document&, HTML::PaintConfig, u64 painting_id, PaintServer::FrameOutputType = PaintServer::FrameOutputType::Presentation, PaintServer::OffscreenRenderTarget = {});
WEB_API RefPtr<DisplayList> update_display_list_for_document_frame(DOM::Document&, HTML::PaintConfig);
WEB_API Optional<DocumentFramePainterResult> update_display_list_and_scroll_state_for_document_frame(DOM::Document&, HTML::PaintConfig);

}
