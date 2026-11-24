/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/HashMap.h>
#include <AK/NeverDestroyed.h>
#include <LibCore/EventLoop.h>
#include <LibGC/RootVector.h>
#include <LibGC/Weak.h>
#include <LibGC/WeakHashMap.h>
#include <LibGC/WeakHashSet.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Page/RenderingStats.h>
#include <LibWeb/Streams/ReadableStream.h>

namespace Web {

struct PagePendingStats {
    RenderingPendingCounts last_counts {};
    GC::WeakHashSet<DOM::Document const> documents;
    bool logged { false };
};

struct PendingOperation {
    GC::Weak<DOM::Document const> document;
    RenderingPendingReason reason;
    GC::Weak<Streams::ReadableStream const> stream;
};

struct PendingStats {
    GC::WeakHashMap<Page const, PagePendingStats> pages;
    // By the address of each operation's token, which the token's finalizer can still name.
    HashMap<FlatPtr, PendingOperation> operations;
    GC::WeakHashMap<Streams::ReadableStream const, HashTable<FlatPtr>> streams;
    GC::WeakHashSet<Page const> dirty_pages;
    bool flush_queued { false };
    size_t task_depth { 0 };
};

static thread_local PendingStats* s_pending_stats;

static PendingStats& pending_stats()
{
    static thread_local NeverDestroyed<PendingStats> stats;
    s_pending_stats = &*stats;
    return *stats;
}

static void pending_changed(Page const& page)
{
    auto& stats = pending_stats();
    stats.dirty_pages.set(page);
    if (stats.flush_queued)
        return;
    stats.flush_queued = true;
    Core::deferred_invoke([] {
        s_pending_stats->flush_queued = false;
        // HTML tasks flush after their microtasks. Other engine callbacks flush at the Core event-loop boundary.
        if (!Bindings::main_thread_vm().has_running_execution_context())
            flush_rendering_pending();
    });
}

static void remove_pending_document(DOM::Document const& document, RenderingStats& stats)
{
    auto page = stats.pending_page.ptr();
    stats.pending_page = GC::Weak<Page const> {};
    if (!page || !s_pending_stats)
        return;
    auto page_stats = s_pending_stats->pages.get(*page);
    if (!page_stats.has_value())
        return;
    page_stats->documents.remove(document);
    pending_changed(*page);
}

void Detail::forget_rendering_stats(DOM::Document const& document)
{
    if (auto* stats = document.rendering_stats())
        remove_pending_document(document, *stats);
}

void Detail::set_rendering_pending(DOM::Document const& document, RenderingPendingReason reason, u64 count)
{
    if (!Core::StatsLog::the() || document.is_decoded_svg())
        return;
    auto& stats = document.ensure_rendering_stats();
    auto& previous = stats.pending[to_underlying(reason)];
    if (previous == count)
        return;
    if (auto page = stats.pending_page.ptr())
        pending_changed(*page);
    previous = count;
}

void Detail::rendering_pending_document_changed(DOM::Document const& document)
{
    if (!Core::StatsLog::the() || document.is_decoded_svg())
        return;
    auto& stats = document.ensure_rendering_stats();
    remove_pending_document(document, stats);
    auto& pages = pending_stats().pages;
    auto navigable = document.navigable();
    if (!navigable || document.has_been_destroyed() || !navigable->active_document_is(document))
        return;
    auto& page = document.page();
    auto& page_stats = pages.ensure(page);
    stats.pending_page = page;
    page_stats.documents.set(document);
    stats.displayed = navigable->is_local_root() || (navigable->container() && navigable->container()->stats_displayed());
    pending_changed(page);
    Detail::set_rendering_pending(document, RenderingPendingReason::NeedsRenderingUpdate, 1);
    Detail::set_rendering_pending(document, RenderingPendingReason::DocumentLoads, document.readiness() != HTML::DocumentReadyState::Complete);
}

void Detail::set_rendering_pending_displayed(HTML::Navigable const& navigable, bool displayed)
{
    auto const* local = as_if<HTML::LocalNavigable>(navigable);
    if (!local)
        return;
    if (auto document = local->active_document()) {
        auto& stats = document->ensure_rendering_stats();
        if (exchange(stats.displayed, displayed) != displayed && stats.pending_page)
            pending_changed(*stats.pending_page);
    }
}

void Detail::set_rendering_pending_ancestors_displayed(HTML::LocalNavigable& navigable, bool displayed)
{
    if (navigable.update_stats_ancestors_displayed(displayed))
        pending_changed(navigable.page());
}

void Detail::forget_rendering_pending(Page const& page)
{
    if (!s_pending_stats)
        return;
    auto page_stats = s_pending_stats->pages.get(page);
    if (!page_stats.has_value())
        return;
    for (auto& document : page_stats->documents)
        document.rendering_stats()->pending_page = GC::Weak<Page const> {};
    s_pending_stats->dirty_pages.remove(page);
    s_pending_stats->pages.remove(page);
}

static FlatPtr operation_key(GC::Cell const* token)
{
    return reinterpret_cast<FlatPtr>(token);
}

void Detail::begin_rendering_pending_operation(GC::Cell const* token, DOM::Document const& document, RenderingPendingReason reason)
{
    if (!Core::StatsLog::the() || document.is_decoded_svg())
        return;
    auto& operations = pending_stats().operations;
    VERIFY(!operations.contains(operation_key(token)));
    operations.set(operation_key(token), PendingOperation { document, reason, {} });
    Detail::set_rendering_pending(document, reason, document.ensure_rendering_stats().pending[to_underlying(reason)] + 1);
}

static void end_pending_operation(FlatPtr key)
{
    if (!s_pending_stats)
        return;
    auto pending_operation = s_pending_stats->operations.take(key);
    if (!pending_operation.has_value())
        return;
    auto& operation = *pending_operation;
    if (auto stream = operation.stream.ptr()) {
        if (auto tokens = s_pending_stats->streams.get(*stream); tokens.has_value()) {
            tokens->remove(key);
            if (tokens->is_empty())
                s_pending_stats->streams.remove(*stream);
        }
    }
    if (!operation.document)
        return;
    auto& document = *operation.document;
    auto& count = document.ensure_rendering_stats().pending[to_underlying(operation.reason)];
    VERIFY(count);
    Detail::set_rendering_pending(document, operation.reason, count - 1);
}

void Detail::end_rendering_pending_operation(GC::Cell const* token)
{
    end_pending_operation(operation_key(token));
}

void Detail::track_rendering_pending_stream(GC::Cell const* token, Streams::ReadableStream& stream)
{
    if (!s_pending_stats)
        return;
    auto operation = s_pending_stats->operations.get(operation_key(token));
    if (!operation.has_value())
        return;
    // Closing a source with unread queued bytes need not close its identity transform. Observe the source's
    // close request, before fetch installs that transform, so an unread response cannot keep the page pending.
    if (!stream.is_readable() || stream.close_requested()) {
        end_rendering_pending_operation(token);
        return;
    }
    VERIFY(!operation->stream);
    operation->stream = stream;
    s_pending_stats->streams.ensure(stream).set(operation_key(token));
}

void Detail::complete_rendering_pending_stream(Streams::ReadableStream const& stream)
{
    if (!s_pending_stats)
        return;
    auto stream_tokens = s_pending_stats->streams.get(stream);
    if (!stream_tokens.has_value())
        return;
    auto tokens = move(*stream_tokens);
    s_pending_stats->streams.remove(stream);
    for (auto token : tokens)
        end_pending_operation(token);
}

void Detail::did_update_rendering(DOM::Document& document)
{
    if (!Core::StatsLog::the())
        return;
    auto navigable = document.navigable();
    if (!navigable || navigable->needs_repaint() || document.needs_animated_style_update())
        return;
    if (auto* style_computer = document.style_computer_if_created(); style_computer && style_computer->style_engine().has_recorded_input())
        return;
    // A no-op style/layout update can consume an invalidation without committing a presentation.
    Detail::set_rendering_pending(document, RenderingPendingReason::NeedsRenderingUpdate, 0);
}

void Detail::flush_rendering_pending()
{
    auto* log = Core::StatsLog::the();
    if (!log || !s_pending_stats || s_pending_stats->task_depth)
        return;
    GC::RootVector<GC::Ref<Page const>> dirty_pages;
    for (auto& page : s_pending_stats->dirty_pages)
        dirty_pages.append(page);
    s_pending_stats->dirty_pages.clear();
    for (auto page : dirty_pages) {
        auto page_stats = s_pending_stats->pages.get(page);
        if (!page_stats.has_value() || !page->has_top_level_traversable())
            continue;
        auto& stats = *page_stats;
        RenderingPendingCounts counts {};
        for (auto& document : stats.documents) {
            auto const& document_stats = *document.rendering_stats();
            auto navigable = document.navigable();
            auto local_root = navigable->local_root();
            if (local_root->parent() && !local_root->stats_ancestors_displayed())
                continue;
            if (!document_stats.displayed || document.visibility_state() != HTML::VisibilityState::Visible
                || navigable->has_inclusive_ancestor_with_visibility_hidden())
                continue;
            if (auto intersection = navigable->viewport_intersection(); intersection.has_value() && intersection->is_empty())
                continue;
            for (size_t i = 0; i < counts.size(); ++i)
                counts[i] += document_stats.pending[i];
        }
        if (stats.logged && counts == stats.last_counts)
            continue;
        static constexpr Array names {
            "needs_rendering_update"sv,
            "image_decodes"sv,
            "font_loads"sv,
            "style_sheet_loads"sv,
            "resource_fetches_in_flight"sv,
            "scheduled_animation_frame_callbacks"sv,
            "load_event_delayers"sv,
            "document_loads"sv,
        };
        static_assert(names.size() == to_underlying(RenderingPendingReason::Count));
        JsonObject reasons;
        bool pending = false;
        for (size_t i = 0; i < names.size(); ++i) {
            reasons.set(names[i], counts[i]);
            pending |= counts[i] != 0;
        }
        JsonObject data;
        data.set("page_id"sv, page->client().id().value());
        data.set("navigable_id"sv, MUST(String::formatted("{}", page->top_level_traversable()->id())));
        data.set("pending"sv, pending);
        data.set("reasons"sv, move(reasons));
        log->event("rendering_pending"sv, move(data));
        stats.last_counts = counts;
        stats.logged = true;
    }
}

void Detail::begin_rendering_pending_task()
{
    flush_rendering_pending();
    ++pending_stats().task_depth;
}

void Detail::end_rendering_pending_task()
{
    VERIFY(s_pending_stats && s_pending_stats->task_depth);
    --s_pending_stats->task_depth;
    flush_rendering_pending();
}

}
