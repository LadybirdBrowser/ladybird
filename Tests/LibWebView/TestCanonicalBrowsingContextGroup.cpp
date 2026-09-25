/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebView/CanonicalBrowsingContext.h>
#include <LibWebView/CanonicalBrowsingContextGroup.h>
#include <LibWebView/CanonicalDocument.h>
#include <LibWebView/CanonicalSessionHistoryEntry.h>
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/CanonicalWindow.h>
#include <LibWebView/SiteIsolation.h>

static URL::Origin origin_for(StringView url)
{
    return URL::Parser::basic_parse(url).value().origin();
}

TEST_CASE(top_level_browsing_context_is_alone_in_a_new_group)
{
    auto browsing_context = WebView::CanonicalBrowsingContext::create_a_new_top_level_browsing_context_and_document().browsing_context;
    auto group = browsing_context->group();
    VERIFY(group);

    EXPECT_EQ(group->browsing_context_set().size(), 1u);
    EXPECT(group->browsing_context_set().contains(browsing_context.ptr()));
}

TEST_CASE(auxiliary_browsing_context_joins_the_openers_group)
{
    WebView::CanonicalTraversable opener;
    opener.set_active_session_history_entry(WebView::CanonicalSessionHistoryEntry::create(WebView::CanonicalDocumentState::create({}, WebView::CanonicalBrowsingContext::create_a_new_top_level_browsing_context_and_document().document)));

    auto popup_browsing_context = WebView::CanonicalBrowsingContext::create_a_new_auxiliary_browsing_context_and_document(opener).browsing_context;

    EXPECT_EQ(popup_browsing_context->group(), opener.active_browsing_context().group());
    EXPECT_EQ(popup_browsing_context->group()->browsing_context_set().size(), 2u);
    EXPECT(popup_browsing_context->is_auxiliary());
    EXPECT_EQ(popup_browsing_context->opener_browsing_context().ptr(), &opener.active_browsing_context());
}

TEST_CASE(child_browsing_context_is_not_in_the_group)
{
    auto top_level = WebView::CanonicalBrowsingContext::create_a_new_top_level_browsing_context_and_document();
    auto top_level_browsing_context = top_level.browsing_context;
    auto group = top_level_browsing_context->group();
    VERIFY(group);

    Web::HTML::ReplicatedContainerState embedder {};
    auto child_browsing_context = WebView::CanonicalBrowsingContext::create_a_new_browsing_context_and_document(top_level.document.ptr(), embedder, *group).browsing_context;

    EXPECT_EQ(child_browsing_context->group(), nullptr);
    EXPECT_EQ(&child_browsing_context->top_level_browsing_context(), top_level_browsing_context.ptr());
    EXPECT_EQ(group->browsing_context_set().size(), 1u);
}

TEST_CASE(replacing_a_traversables_browsing_context_leaves_its_group)
{
    WebView::CanonicalTraversable traversable;
    traversable.set_active_session_history_entry(WebView::CanonicalSessionHistoryEntry::create(WebView::CanonicalDocumentState::create({}, WebView::CanonicalBrowsingContext::create_a_new_top_level_browsing_context_and_document().document)));
    auto* initial_browsing_context = &traversable.active_browsing_context();
    VERIFY(initial_browsing_context->group());
    NonnullRefPtr initial_group = *initial_browsing_context->group();

    auto replacement = WebView::CanonicalBrowsingContext::create_a_new_top_level_browsing_context_and_document();
    traversable.set_active_session_history_entry(WebView::CanonicalSessionHistoryEntry::create(WebView::CanonicalDocumentState::create({}, replacement.document)));

    EXPECT_EQ(&traversable.active_browsing_context(), replacement.browsing_context.ptr());
    EXPECT(initial_group->browsing_context_set().is_empty());
}

TEST_CASE(removing_a_browsing_context_clears_its_group)
{
    auto browsing_context = WebView::CanonicalBrowsingContext::create_a_new_top_level_browsing_context_and_document().browsing_context;
    VERIFY(browsing_context->group());
    NonnullRefPtr group = *browsing_context->group();

    group->remove(*browsing_context);

    EXPECT_EQ(browsing_context->group(), nullptr);
    EXPECT(!group->browsing_context_set().contains(browsing_context.ptr()));
}

TEST_CASE(response_browsing_context_is_activated_only_at_commit)
{
    WebView::CanonicalTraversable traversable;
    traversable.set_active_session_history_entry(WebView::CanonicalSessionHistoryEntry::create(WebView::CanonicalDocumentState::create({}, WebView::CanonicalBrowsingContext::create_a_new_top_level_browsing_context_and_document().document)));
    auto* initial_context = &traversable.active_browsing_context();
    auto initial_group = initial_context->group();
    auto destination_url = URL::Parser::basic_parse("https://ladybird.org/"sv).release_value();
    auto destination_document = traversable.create_and_initialize_a_document({
        .is_inline_content = false,
        .coop_enforcement_result = {
            .needs_a_browsing_context_group_switch = true,
            .url = destination_url,
            .origin = destination_url.origin(),
            .opener_policy = {},
        },
        .response_url = destination_url,
        .request_current_url = {},
        .origin = destination_url.origin(),
    });
    auto* destination_context = &destination_document->browsing_context();
    auto navigation_id = Utf16String::from_utf8("navigation"sv);
    traversable.ensure_ongoing_navigation().navigation_id = navigation_id;
    traversable.populate_document(WebView::CanonicalDocumentState::create({}), destination_document, navigation_id);

    EXPECT_EQ(&traversable.active_browsing_context(), initial_context);
    EXPECT(initial_group->browsing_context_set().contains(initial_context));

    traversable.clear_ongoing_navigation();
    EXPECT(!traversable.pending_document());
    EXPECT_EQ(&traversable.active_browsing_context(), initial_context);
    EXPECT(initial_group->browsing_context_set().contains(initial_context));

    traversable.ensure_ongoing_navigation().navigation_id = navigation_id;
    auto destination_document_state = WebView::CanonicalDocumentState::create({});
    traversable.populate_document(destination_document_state, destination_document, navigation_id);
    auto committed_entry = WebView::CanonicalSessionHistoryEntry::create(destination_document_state);
    Web::HTML::HostedNavigableState committed_state {
        .active_document_url = destination_url,
        .active_document_is_fully_active = true,
        .opener_policy = {},
        .active_document_is_completely_loaded = false,
        .is_closing = false,
        .container = {},
        .delays_the_load_event_of_its_container = false,
        .compositor_context_id = {},
    };
    traversable.did_commit_navigation(*committed_entry, move(committed_state), navigation_id, WebView::CanonicalNavigable::DidPopulateDocument::Yes, {});
    EXPECT_EQ(&traversable.active_browsing_context(), destination_context);
    EXPECT(initial_group->browsing_context_set().is_empty());
    EXPECT(!traversable.ongoing_navigation().has_value());
}

TEST_CASE(child_navigation_under_a_pending_document_uses_its_group)
{
    WebView::CanonicalTraversable traversable;
    traversable.set_active_session_history_entry(WebView::CanonicalSessionHistoryEntry::create(WebView::CanonicalDocumentState::create({}, WebView::CanonicalBrowsingContext::create_a_new_top_level_browsing_context_and_document().document)));
    auto displayed_group = traversable.active_browsing_context().group();

    auto destination_url = URL::Parser::basic_parse("https://ladybird.org/"sv).release_value();
    auto destination_document = traversable.create_and_initialize_a_document({
        .is_inline_content = false,
        .coop_enforcement_result = {
            .needs_a_browsing_context_group_switch = true,
            .url = destination_url,
            .origin = destination_url.origin(),
            .opener_policy = {},
        },
        .response_url = destination_url,
        .request_current_url = {},
        .origin = destination_url.origin(),
    });
    auto destination_group = destination_document->browsing_context().group();
    VERIFY(destination_group && destination_group != displayed_group);

    // The destination document's frame is created, and navigates, before the destination document is activated.
    Web::HTML::ReplicatedContainerState embedder {};
    auto frame_document = WebView::CanonicalBrowsingContext::create_a_new_browsing_context_and_document(destination_document.ptr(), embedder, *destination_group).document;
    auto& frame = traversable.append_child(make<WebView::CanonicalNavigable>(Web::HTML::CrossProcessId { 2, 1 }));
    frame.set_active_session_history_entry(WebView::CanonicalSessionHistoryEntry::create(WebView::CanonicalDocumentState::create({}, frame_document)));

    auto frame_url = URL::Parser::basic_parse("https://example.org/"sv).release_value();
    auto document = frame.create_and_initialize_a_document({
        .is_inline_content = false,
        .coop_enforcement_result = {
            .needs_a_browsing_context_group_switch = false,
            .url = frame_url,
            .origin = frame_url.origin(),
            .opener_policy = {},
        },
        .response_url = frame_url,
        .request_current_url = {},
        .origin = frame_url.origin(),
    });

    EXPECT_EQ(&document->relevant_global_object().agent(), destination_group->obtain_similar_origin_window_agent(frame_url.origin(), false).ptr());
}

TEST_CASE(clearing_a_navigation_abandons_only_the_document_populated_for_it)
{
    WebView::CanonicalTraversable traversable;
    auto make_document = [] { return WebView::CanonicalBrowsingContext::create_a_new_top_level_browsing_context_and_document().document; };
    traversable.set_active_session_history_entry(WebView::CanonicalSessionHistoryEntry::create(WebView::CanonicalDocumentState::create({}, make_document())));

    // A document a history job populated outlives a navigation admitted and cleared meanwhile.
    auto job_document = make_document();
    traversable.populate_document(WebView::CanonicalDocumentState::create({}), job_document);
    traversable.ensure_ongoing_navigation().navigation_id = Utf16String::from_utf8("navigation"sv);
    traversable.clear_ongoing_navigation();
    EXPECT_EQ(traversable.pending_document().ptr(), job_document.ptr());

    // A document populated for a navigation goes with it.
    auto navigation_id = Utf16String::from_utf8("navigation"sv);
    traversable.ensure_ongoing_navigation().navigation_id = navigation_id;
    traversable.populate_document(WebView::CanonicalDocumentState::create({}), make_document(), navigation_id);
    EXPECT(traversable.pending_document());
    traversable.clear_ongoing_navigation();
    EXPECT(!traversable.pending_document());
}

TEST_CASE(populated_document_replaces_tracked_load_when_document_state_is_reused)
{
    WebView::CanonicalTraversable traversable;
    auto entry = WebView::CanonicalSessionHistoryEntry::create(WebView::CanonicalDocumentState::create(Web::HTML::CrossProcessId { 1, 1 }, WebView::CanonicalBrowsingContext::create_a_new_top_level_browsing_context_and_document().document));
    entry->navigation_api_id = Utf16String::from_utf8("entry"sv);
    traversable.set_active_session_history_entry(entry);
    auto navigation_id = Utf16String::from_utf8("reload"sv);
    traversable.ensure_ongoing_navigation().navigation_id = navigation_id;
    auto destination_url = URL::Parser::basic_parse("https://ladybird.org/redirected"sv).release_value();

    Web::HTML::HostedNavigableState committed_state {
        .active_document_url = destination_url,
        .active_document_is_fully_active = true,
        .opener_policy = {},
        .active_document_is_completely_loaded = false,
        .is_closing = false,
        .container = {},
        .delays_the_load_event_of_its_container = false,
        .compositor_context_id = {},
    };
    traversable.did_commit_navigation(*entry, move(committed_state), navigation_id, WebView::CanonicalNavigable::DidPopulateDocument::Yes, {});

    EXPECT_EQ(traversable.active_document_load().navigation_id, navigation_id);
}

TEST_CASE(site_keyed_agent_clusters)
{
    auto group = WebView::CanonicalBrowsingContextGroup::create();
    auto first_origin = origin_for("https://a.ladybird.org"sv);
    auto second_origin = origin_for("https://b.ladybird.org"sv);

    auto first_agent = group->obtain_similar_origin_window_agent(first_origin, false);
    auto second_agent = group->obtain_similar_origin_window_agent(second_origin, false);

    EXPECT_EQ(first_agent.ptr(), second_agent.ptr());
}

TEST_CASE(origin_keyed_agent_clusters)
{
    auto group = WebView::CanonicalBrowsingContextGroup::create();
    auto first_origin = origin_for("https://a.ladybird.org"sv);
    auto second_origin = origin_for("https://b.ladybird.org"sv);

    auto origin_keyed_agent = group->obtain_similar_origin_window_agent(first_origin, true);
    auto site_keyed_agent = group->obtain_similar_origin_window_agent(second_origin, false);

    EXPECT_NE(origin_keyed_agent.ptr(), site_keyed_agent.ptr());

    // The first decision for an origin is permanent within a browsing context group.
    auto first_origin_again = group->obtain_similar_origin_window_agent(first_origin, false);
    EXPECT_EQ(origin_keyed_agent.ptr(), first_origin_again.ptr());
}

TEST_CASE(historical_site_key_cannot_be_changed_by_a_later_oac_request)
{
    auto group = WebView::CanonicalBrowsingContextGroup::create();
    auto origin = origin_for("https://a.ladybird.org"sv);

    auto site_keyed_agent = group->obtain_similar_origin_window_agent(origin, false);
    auto agent_after_oac_request = group->obtain_similar_origin_window_agent(origin, true);

    EXPECT_EQ(site_keyed_agent.ptr(), agent_after_oac_request.ptr());
}

TEST_CASE(opaque_origins_have_distinct_agent_clusters)
{
    auto group = WebView::CanonicalBrowsingContextGroup::create();
    auto first_origin = URL::Origin::create_opaque();
    auto second_origin = URL::Origin::create_opaque();

    auto first_agent = group->obtain_similar_origin_window_agent(first_origin, false);
    auto first_agent_again = group->obtain_similar_origin_window_agent(first_origin, false);
    auto second_agent = group->obtain_similar_origin_window_agent(second_origin, false);

    EXPECT_EQ(first_agent.ptr(), first_agent_again.ptr());
    EXPECT_NE(first_agent.ptr(), second_agent.ptr());
}
