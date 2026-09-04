/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWebView/CanonicalBrowsingContext.h>
#include <LibWebView/CanonicalBrowsingContextGroup.h>
#include <LibWebView/CanonicalTraversable.h>

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
