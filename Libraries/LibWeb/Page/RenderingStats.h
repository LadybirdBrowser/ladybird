/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <LibCore/StatsLog.h>
#include <LibGC/Ptr.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web {

enum class RenderingPendingReason {
    NeedsRenderingUpdate,
    ImageDecodes,
    FontLoads,
    StyleSheetLoads,
    ResourceFetchesInFlight,
    ScheduledAnimationFrameCallbacks,
    LoadEventDelayers,
    DocumentLoads,
    Count,
};

using RenderingPendingCounts = Array<u64, to_underlying(RenderingPendingReason::Count)>;

struct RenderingStats {
    AK_ALLOC_WITH_KMALLOC;

    RenderingPendingCounts pending {};
    GC::RawPtr<Page const> pending_page {};
    bool displayed { true };
};

namespace Detail {

WEB_API void set_rendering_pending(DOM::Document const& document, RenderingPendingReason reason, u64 count);
WEB_API void rendering_pending_document_changed(DOM::Document const& document);
WEB_API void set_rendering_pending_displayed(HTML::Navigable const& navigable, bool displayed);
WEB_API void set_rendering_pending_ancestors_displayed(HTML::LocalNavigable&, bool);
WEB_API void forget_rendering_stats(DOM::Document const& document);
WEB_API void forget_rendering_pending(Page const& page);
WEB_API void flush_rendering_pending();
WEB_API void begin_rendering_pending_task();
WEB_API void end_rendering_pending_task();
WEB_API void did_update_rendering(DOM::Document& document);
WEB_API void begin_rendering_pending_operation(GC::Cell const* token, DOM::Document const& document, RenderingPendingReason reason);
WEB_API void end_rendering_pending_operation(GC::Cell const* token);
WEB_API void track_rendering_pending_stream(GC::Cell const* token, Streams::ReadableStream& stream);
WEB_API void complete_rendering_pending_stream(Streams::ReadableStream const& stream);

}

inline void set_rendering_pending(DOM::Document const& document, RenderingPendingReason reason, u64 count)
{
    if (Core::StatsLog::the())
        Detail::set_rendering_pending(document, reason, count);
}

inline void rendering_pending_document_changed(DOM::Document const& document)
{
    if (Core::StatsLog::the())
        Detail::rendering_pending_document_changed(document);
}

inline void set_rendering_pending_displayed(HTML::Navigable const& navigable, bool displayed)
{
    if (Core::StatsLog::the())
        Detail::set_rendering_pending_displayed(navigable, displayed);
}

inline void set_rendering_pending_ancestors_displayed(HTML::LocalNavigable& navigable, bool displayed)
{
    if (Core::StatsLog::the())
        Detail::set_rendering_pending_ancestors_displayed(navigable, displayed);
}

inline void forget_rendering_stats(DOM::Document const& document)
{
    if (Core::StatsLog::the())
        Detail::forget_rendering_stats(document);
}

inline void forget_rendering_pending(Page const& page)
{
    if (Core::StatsLog::the())
        Detail::forget_rendering_pending(page);
}

inline void flush_rendering_pending()
{
    if (Core::StatsLog::the())
        Detail::flush_rendering_pending();
}

inline void begin_rendering_pending_task()
{
    if (Core::StatsLog::the())
        Detail::begin_rendering_pending_task();
}

inline void end_rendering_pending_task()
{
    if (Core::StatsLog::the())
        Detail::end_rendering_pending_task();
}

inline void did_update_rendering(DOM::Document& document)
{
    if (Core::StatsLog::the())
        Detail::did_update_rendering(document);
}

inline void begin_rendering_pending_operation(GC::Cell const* token, DOM::Document const& document, RenderingPendingReason reason)
{
    if (Core::StatsLog::the())
        Detail::begin_rendering_pending_operation(token, document, reason);
}

inline void end_rendering_pending_operation(GC::Cell const* token)
{
    if (Core::StatsLog::the())
        Detail::end_rendering_pending_operation(token);
}

inline void track_rendering_pending_stream(GC::Cell const* token, Streams::ReadableStream& stream)
{
    if (Core::StatsLog::the())
        Detail::track_rendering_pending_stream(token, stream);
}

inline void complete_rendering_pending_stream(Streams::ReadableStream const& stream)
{
    if (Core::StatsLog::the())
        Detail::complete_rendering_pending_stream(stream);
}

}
