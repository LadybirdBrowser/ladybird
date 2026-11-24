/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include <AK/LexicalPath.h>
#include <LibGfx/SystemTheme.h>
#include <LibTest/TestCase.h>
#include <LibWebView/Application.h>
#include <LibWebView/CanonicalBrowsingContext.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/HeadlessWebView.h>
#include <LibWebView/Utilities.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>

// A frame reported in a document whose top-level browsing context has left its group, as the previous one does in a
// browsing context group switch, is dropped, and its renderer is not taken for misbehaving.

TEST_CASE(a_frame_reported_in_a_document_outside_any_group_is_dropped)
{
    Test::HeadlessTestEnvironment environment { "TestChildFrameReports"sv };

    auto app = TRY_OR_FAIL(environment.create_application());

    auto theme_path = LexicalPath::join(WebView::s_ladybird_resource_root, "themes"sv, "Default.ini"sv);
    auto theme = TRY_OR_FAIL(Gfx::load_system_theme(theme_path.string()));

    auto view = WebView::HeadlessWebView::create(theme, { 800, 600 });
    auto& client = view->page().client();
    auto& traversable = view->traversable();
    auto& document = traversable.active_document();
    document.browsing_context().remove();

    Web::HTML::PendingSessionHistoryEntryDescriptor initial_history_entry;
    initial_history_entry.url = URL::about_blank();
    initial_history_entry.document_state.id = { .namespace_id = 0xfeed0000, .local_id = 1 };
    Web::HTML::CrossProcessId frame_id { .namespace_id = 0xfeed0000, .local_id = 2 };
    Web::HTML::CrossProcessId frame_document_id { .namespace_id = 0xfeed0000, .local_id = 3 };

    static_cast<WebContentClientStub&>(client).did_create_child_frame(view->page().id(), traversable.id(), document.id(), frame_id, {}, move(initial_history_entry), URL::Origin::create_opaque(), Web::HTML::EnvironmentId::generate(), frame_document_id);
    VERIFY(!traversable.find(frame_id).has_value());
    VERIFY(client.is_open());

    outln("PASS: a frame reported in a document outside any browsing context group is dropped");
}
