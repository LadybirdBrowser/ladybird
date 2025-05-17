/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/PolicyContainers.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/SourceSnapshotParams.h>
#include <LibWeb/HTML/Window.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(SourceSnapshotParams);

void SourceSnapshotParams::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(fetch_client);
    visitor.visit(source_policy_container);
}

// https://whatpr.org/html/11250/4ae3173...51371a5/browsing-the-web.html
// https://html.spec.whatwg.org/multipage/browsing-the-web.html#snapshotting-source-snapshot-params
GC::Ref<SourceSnapshotParams> snapshot_source_snapshot_params(GC::Heap& heap, GC::Ptr<DOM::Document> source_document)
{
    //  To snapshot source snapshot params given a Document-or-null sourceDocument:

    // 1.  If sourceDocument is null, then return a new source snapshot params with
    if (!source_document) {
        return heap.allocate<SourceSnapshotParams>(
            // has transient activation
            //    true
            true,

            // sandboxing flags
            //    an empty sandboxing flag set
            SandboxingFlagSet {},

            // allows downloading
            //    true
            true,

            // fetch client
            //    null
            nullptr,

            // source policy container
            //    a new policy container
            heap.allocate<PolicyContainer>(heap));
        // NOTE:  This only occurs in the case of a browser UI-initiated navigation.
    }

    // 2. Return a new source snapshot params with
    return heap.allocate<SourceSnapshotParams>(
        // has transient activation
        //    true if sourceDocument's relevant global object has transient activation; otherwise false
        as<HTML::Window>(HTML::relevant_global_object(*source_document)).has_transient_activation(),

        // sandboxing flags
        //     sourceDocument's active sandboxing flag set
        source_document->active_sandboxing_flag_set(),

        // allows downloading
        //     false if sourceDocument's active sandboxing flag set has the sandboxed downloads browsing context flag set; otherwise true
        !has_flag(source_document->active_sandboxing_flag_set(), HTML::SandboxingFlagSet::SandboxedDownloads),

        // fetch client
        //     sourceDocument's relevant settings object
        relevant_settings_object(*source_document),

        // source policy container
        //     a clone of sourceDocument's policy container
        source_document->policy_container()->clone(heap));
}

}
