/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Function.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/Painting/DocumentFramePainter.h>
#include <LibWeb/Painting/ExternalContentSource.h>
#include <LibWeb/Painting/OffscreenPaintRequest.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/ViewportPaintable.h>

namespace Web::Painting {

static void invalidate_cached_paint_data(DOM::Document& document);
static void invalidate_cached_paint_data_for_offscreen_render(DOM::Document& document);

WEB_API bool submit_offscreen_paint_request(OffscreenPaintRequest request, u64 painting_id, Function<void()> on_submit_error)
{
    if (request.readiness == OffscreenPaintSync::WaitForExternalContent)
        invalidate_cached_paint_data_for_offscreen_render(*request.document);

    Optional<DeferredPaintCommandSubmission> prepared_submission = prepare_paint_commands_for_document_frame(*request.document, request.paint_config, painting_id, PaintServer::FrameOutputType::Offscreen, request.target);
    if (!prepared_submission.has_value())
        return false;

    if (request.readiness == OffscreenPaintSync::WaitForExternalContent) {
        Vector<NonnullRefPtr<ExternalContentSource>> unresolved_sources;
        for (auto const& source : prepared_submission->external_content_sources) {
            if (!source->has_finalized_content())
                unresolved_sources.append(source);
        }
        if (!unresolved_sources.is_empty()) {
            auto callback = GC::create_function(request.document->heap(), [remaining = unresolved_sources.size(), submit = move(prepared_submission->submit), on_submit_error = move(on_submit_error)]() mutable {
                VERIFY(remaining > 0);
                remaining--;
                if (remaining != 0)
                    return;

                Optional<PaintServer::ReleaseToken> release_token = submit();
                if (!release_token.has_value() && on_submit_error)
                    on_submit_error();
            });
            for (auto const& source : unresolved_sources)
                source->when_content_is_finalized(callback);
            return true;
        }
    }
    Optional<PaintServer::ReleaseToken> release_token = prepared_submission->submit();
    return release_token.has_value();
}

static void invalidate_cached_paint_data(DOM::Document& document)
{
    document.invalidate_display_list();
    if (ViewportPaintable* paintable = document.paintable())
        paintable->for_each_in_inclusive_subtree_of_type<PaintableBox>([](auto& paintable_box) {
            paintable_box.invalidate_paint_cache();
            return TraversalDecision::Continue;
        });
}

static void invalidate_cached_paint_data_for_offscreen_render(DOM::Document& document)
{
    invalidate_cached_paint_data(document);

    for (GC::Root<HTML::Navigable> const& navigable : document.descendant_navigables()) {
        if (GC::Ptr<DOM::Document> active_document = navigable->active_document())
            invalidate_cached_paint_data(*active_document);
    }
}

}
