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
#include <LibWebView/CanonicalTraversable.h>
#include <LibWebView/SiteIsolation.h>
#include <LibWebView/SiteIsolationManager.h>

static URL::Origin origin_for(StringView url)
{
    return URL::Parser::basic_parse(url).value().origin();
}

TEST_CASE(top_level_browsing_context_is_alone_in_a_new_group)
{
    auto browsing_context = WebView::CanonicalBrowsingContext::create_a_new_top_level_browsing_context_and_document(URL::Origin::create_opaque(), {});
    auto group = browsing_context->group();
    VERIFY(group);

    EXPECT_EQ(group->browsing_context_set().size(), 1u);
    EXPECT(group->browsing_context_set().contains(browsing_context.ptr()));
}

TEST_CASE(auxiliary_browsing_context_joins_the_openers_group)
{
    WebView::CanonicalTraversable opener;
    opener.set_active_browsing_context(WebView::CanonicalBrowsingContext::create_a_new_top_level_browsing_context_and_document(origin_for("https://a.ladybird.org"sv), {}));

    auto popup_browsing_context = WebView::CanonicalBrowsingContext::create_a_new_auxiliary_browsing_context_and_document(opener, origin_for("https://a.ladybird.org"sv), {});

    EXPECT_EQ(popup_browsing_context->group(), opener.active_browsing_context().group());
    EXPECT_EQ(popup_browsing_context->group()->browsing_context_set().size(), 2u);
}

TEST_CASE(child_browsing_context_is_not_in_the_group)
{
    auto top_level_browsing_context = WebView::CanonicalBrowsingContext::create_a_new_top_level_browsing_context_and_document(origin_for("https://a.ladybird.org"sv), {});
    auto group = top_level_browsing_context->group();
    VERIFY(group);

    auto child_browsing_context = WebView::CanonicalBrowsingContext::create_a_new_browsing_context_and_document(*group, origin_for("https://a.ladybird.org"sv), {});

    EXPECT_EQ(child_browsing_context->group(), nullptr);
    EXPECT_EQ(group->browsing_context_set().size(), 1u);
}

TEST_CASE(replacing_a_traversables_browsing_context_leaves_its_group)
{
    WebView::CanonicalTraversable traversable;
    traversable.set_active_browsing_context(WebView::CanonicalBrowsingContext::create_a_new_top_level_browsing_context_and_document(URL::Origin::create_opaque(), {}));
    auto* initial_browsing_context = &traversable.active_browsing_context();
    VERIFY(initial_browsing_context->group());
    NonnullRefPtr initial_group = *initial_browsing_context->group();

    auto replacement_browsing_context = WebView::CanonicalBrowsingContext::create_a_new_top_level_browsing_context_and_document(URL::Origin::create_opaque(), {});
    traversable.set_active_browsing_context(replacement_browsing_context);

    EXPECT_EQ(&traversable.active_browsing_context(), replacement_browsing_context.ptr());
    EXPECT(initial_group->browsing_context_set().is_empty());
}

TEST_CASE(removing_a_browsing_context_clears_its_group)
{
    auto browsing_context = WebView::CanonicalBrowsingContext::create_a_new_top_level_browsing_context_and_document(URL::Origin::create_opaque(), {});
    VERIFY(browsing_context->group());
    NonnullRefPtr group = *browsing_context->group();

    group->remove(*browsing_context);

    EXPECT_EQ(browsing_context->group(), nullptr);
    EXPECT(!group->browsing_context_set().contains(browsing_context.ptr()));
}

TEST_CASE(response_browsing_context_is_activated_only_at_commit)
{
    WebView::CanonicalTraversable traversable;
    traversable.set_active_browsing_context(WebView::CanonicalBrowsingContext::create_a_new_top_level_browsing_context_and_document(URL::Origin::create_opaque(), {}));
    auto* initial_context = &traversable.active_browsing_context();
    auto initial_group = initial_context->group();
    auto destination_url = URL::Parser::basic_parse("https://ladybird.org/"sv).release_value();
    auto destination_context = traversable.obtain_a_browsing_context_to_use_for_a_navigation_response({
        .needs_a_browsing_context_group_switch = true,
        .url = destination_url,
        .origin = destination_url.origin(),
        .opener_policy = {},
    });
    auto navigation_id = Utf16String::from_utf8("navigation"sv);
    traversable.ensure_ongoing_navigation().navigation_id = navigation_id;
    traversable.ongoing_navigation()->destination_browsing_context = destination_context;

    EXPECT_EQ(&traversable.active_browsing_context(), initial_context);
    EXPECT(initial_group->browsing_context_set().contains(initial_context));

    traversable.clear_ongoing_navigation();
    EXPECT_EQ(&traversable.active_browsing_context(), initial_context);
    EXPECT(initial_group->browsing_context_set().contains(initial_context));

    traversable.ensure_ongoing_navigation().navigation_id = navigation_id;
    traversable.ongoing_navigation()->destination_browsing_context = destination_context;
    traversable.did_commit_navigation({
                                          .target_name = {},
                                          .active_document_url = destination_url,
                                          .active_document_origin = destination_url.origin(),
                                          .active_document_is_fully_active = true,
                                          .active_session_history_entry_identity = {},
                                      },
        navigation_id);
    EXPECT_EQ(&traversable.active_browsing_context(), destination_context.ptr());
    EXPECT(initial_group->browsing_context_set().is_empty());
    EXPECT(!traversable.ongoing_navigation().has_value());
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

TEST_CASE(top_level_site_isolation_process_swaps)
{
    auto restore_site_isolation_mode = ScopeGuard([mode = WebView::site_isolation_mode()] {
        WebView::set_site_isolation_mode(mode);
    });
    WebView::set_site_isolation_mode(WebView::SiteIsolationMode::TopLevel);

    auto browsing_context = WebView::CanonicalBrowsingContext::create_a_new_top_level_browsing_context_and_document(URL::Origin::create_opaque(), {});
    auto current_url = URL::Parser::basic_parse("https://a.example/path"sv).release_value();
    auto same_site_url = URL::Parser::basic_parse("https://sub.a.example/other"sv).release_value();
    auto cross_site_url = URL::Parser::basic_parse("https://b.example/path"sv).release_value();

    EXPECT(!WebView::SiteIsolationManager::the().top_level_navigation_requires_process_swap(*browsing_context, URL::about_blank(), cross_site_url));
    EXPECT(!WebView::SiteIsolationManager::the().top_level_navigation_requires_process_swap(*browsing_context, current_url, same_site_url));
    EXPECT(WebView::SiteIsolationManager::the().top_level_navigation_requires_process_swap(*browsing_context, current_url, cross_site_url));

    WebView::CanonicalTraversable opener;
    opener.set_active_browsing_context(browsing_context);
    auto related_browsing_context = WebView::CanonicalBrowsingContext::create_a_new_auxiliary_browsing_context_and_document(opener, URL::Origin::create_opaque(), {});
    EXPECT(browsing_context->group()->browsing_context_set().contains(related_browsing_context.ptr()));
    EXPECT(!WebView::SiteIsolationManager::the().top_level_navigation_requires_process_swap(*browsing_context, current_url, cross_site_url));
}
