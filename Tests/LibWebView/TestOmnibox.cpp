/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWebView/AutocompleteMuxer.h>
#include <LibWebView/AutocompleteRanker.h>
#include <LibWebView/Omnibox.h>
#include <LibWebView/OmniboxEngagement.h>
#include <LibWebView/URL.h>
#include <LibWebView/WebUI.h>

namespace {

using WebView::AutocompleteSuggestion;
using WebView::AutocompleteSuggestionSource;
using WebView::Omnibox;

AutocompleteSuggestion row(AutocompleteSuggestionSource source, StringView text)
{
    return {
        .source = source,
        .text = MUST(String::from_utf8(text)),
        .title = {},
        .subtitle = {},
        .favicon_png = {},
        .highlight_input = {},
        .can_be_automatically_selected = true,
        .can_be_inline_completed = source == AutocompleteSuggestionSource::History || source == AutocompleteSuggestionSource::WebUI,
    };
}

AutocompleteSuggestion history_row(StringView url, i32 relevance = 0)
{
    auto suggestion = row(AutocompleteSuggestionSource::History, url);
    suggestion.relevance = relevance;
    return suggestion;
}

AutocompleteSuggestion non_automatic_history_row(StringView url, StringView title)
{
    return {
        .source = AutocompleteSuggestionSource::History,
        .text = MUST(String::from_utf8(url)),
        .title = MUST(String::from_utf8(title)),
        .subtitle = {},
        .favicon_png = {},
        .highlight_input = {},
        .can_be_automatically_selected = false,
    };
}

AutocompleteSuggestion non_inline_history_row(StringView url, i32 relevance = 0)
{
    auto suggestion = history_row(url, relevance);
    suggestion.can_be_inline_completed = false;
    return suggestion;
}

AutocompleteSuggestion search_row(StringView query)
{
    return row(AutocompleteSuggestionSource::Search, query);
}

AutocompleteSuggestion literal_row(StringView url)
{
    return row(AutocompleteSuggestionSource::LiteralURL, url);
}

AutocompleteSuggestion web_ui_row(StringView url)
{
    return row(AutocompleteSuggestionSource::WebUI, url);
}

class ScriptedProvider final : public WebView::OmniboxSuggestionProvider {
public:
    AK_ALLOC_WITH_KMALLOC;

    virtual void query(WebView::AutocompleteQueryID query_id, String query, size_t) override
    {
        query_ids.append(query_id);
        queries.append(move(query));
    }

    virtual void cancel() override
    {
        ++cancel_count;
    }

    virtual void record_engagement(WebView::OmniboxEngagement engagement) override
    {
        engagements.append(move(engagement));
    }

    void deliver(Vector<AutocompleteSuggestion> suggestions)
    {
        VERIFY(!query_ids.is_empty());
        deliver(query_ids.last(), move(suggestions));
    }

    void deliver(WebView::AutocompleteQueryID query_id, Vector<AutocompleteSuggestion> suggestions)
    {
        on_suggestions(query_id, move(suggestions));
        if (after_delivery)
            after_delivery();
    }

    Function<void()> after_delivery;

    Vector<String> queries;
    Vector<WebView::AutocompleteQueryID> query_ids;
    Vector<WebView::OmniboxEngagement> engagements;
    size_t cancel_count { 0 };
};

// Emulates the line edit the way the chrome uses the model: it renders Display instructions, and user
// keystrokes replace the selected completion range just like they would in a real text box.
struct Harness {
    Harness()
        : omnibox(adopt_provider(*this))
    {
        omnibox.on_display_change = [this](Omnibox::Display const& display) {
            display_text = display.text;
            selection_start = display.selection_start;
            // NB: The chrome redraws the editing icon whenever the text changes, even in the middle of an update.
            (void)omnibox.editing_icon();
        };
        omnibox.on_commit = [this](String text) {
            commits.append(move(text));
            commit_destination_kinds.append(omnibox.destination_kind_for_last_commit());
        };

        // Mirror the popup exactly the way a real chrome does, so the tests exercise the notification
        // contract and not just the model's internal state.
        omnibox.on_suggestions_change = [this] {
            ++popup_repaints;
            visible_popup = omnibox.is_popup_visible();
            visible_rows = omnibox.suggestions();
            visible_selection = omnibox.selected_suggestion();
        };
        omnibox.on_selection_change = [this] {
            visible_selection = omnibox.selected_suggestion();
        };
        provider->after_delivery = [this] { check_invariants(); };
    }

    ~Harness()
    {
        check_invariants();
    }

    // What the user sees must agree with what Return does. These hold after every event, in every test.
    void check_invariants()
    {
        auto fail = [&](StringView invariant) {
            FAIL(MUST(String::formatted("Omnibox invariant broken: {} (query '{}', display '{}', popup {}, pending {}, selection {}, rows [{}]){}",
                invariant, omnibox.query(), display_text, omnibox.is_popup_visible(), omnibox.has_pending_suggestions(),
                omnibox.selected_suggestion(), describe_rows(), describe_context ? describe_context() : String {})));
        };

        // The chrome is told about every change it has to draw.
        if (visible_popup != omnibox.is_popup_visible())
            fail("the chrome shows the popup open or closed, unlike the model"sv);
        if (visible_selection != omnibox.selected_suggestion())
            fail("the chrome highlights a different row than the model"sv);
        if (omnibox.is_popup_visible() && !rows_match(visible_rows, omnibox.suggestions()))
            fail("the chrome shows different rows than the model"sv);

        if (!omnibox.is_editing() || !omnibox.is_popup_visible())
            return;

        auto return_target = omnibox.suggestion_for_return();

        // Text completed inline goes where Return goes, and only the completion is selected, even while new rows are
        // on their way.
        if (omnibox.is_inline_completed()) {
            if (!return_target.has_value() || !return_target->text.bytes_as_string_view().contains(display_text, CaseSensitivity::CaseInsensitive))
                fail("Return ignores the inline completion on display"sv);
            if (!selection_start.has_value() || !display_text.bytes_as_string_view().starts_with(omnibox.query().bytes_as_string_view(), CaseSensitivity::CaseInsensitive))
                fail("the inline completion does not extend the text as typed"sv);
        }

        if (omnibox.has_pending_suggestions())
            return;

        auto const& rows = omnibox.suggestions();
        auto selection = omnibox.selected_suggestion();

        if (selection.has_value()) {
            if (*selection >= rows.size()) {
                fail("the highlighted row does not exist"sv);
                return;
            }

            // The highlighted row is where Return goes: that row, or the search for the text as typed that the row is.
            // NB: Text as typed that is taken for an address goes to the address, not to a search for it.
            auto const& highlighted = rows[*selection];
            auto return_goes_to_highlighted_row = return_target.has_value()
                ? &*return_target == &highlighted
                : highlighted.source == AutocompleteSuggestionSource::Search && is_search_for_query(highlighted)
                    && WebView::classify_user_input(omnibox.query()).classification == WebView::UserInputClassification::Search;
            if (!return_goes_to_highlighted_row)
                fail("Return does not go where the highlighted row says"sv);

            // A row the user did not choose is highlighted only at the top.
            if (!omnibox.selection_is_explicit() && *selection != 0)
                fail("an automatically highlighted row is not the top row"sv);
        }
    }

    bool is_search_for_query(AutocompleteSuggestion const& suggestion) const
    {
        auto kind = WebView::OmniboxDestinationKind::Search;
        return WebView::normalize_omnibox_destination(suggestion.text, kind) == WebView::normalize_omnibox_destination(omnibox.query(), kind);
    }

    static bool rows_match(Vector<AutocompleteSuggestion> const& left, Vector<AutocompleteSuggestion> const& right)
    {
        if (left.size() != right.size())
            return false;
        for (size_t index = 0; index < left.size(); ++index) {
            if (left[index].text != right[index].text || left[index].source != right[index].source)
                return false;
        }
        return true;
    }

    String describe_rows() const
    {
        StringBuilder builder;
        for (auto const& suggestion : omnibox.suggestions()) {
            if (!builder.is_empty())
                builder.append(", "sv);
            builder.append(suggestion.text);
        }
        return MUST(builder.to_string());
    }

    static NonnullOwnPtr<ScriptedProvider> adopt_provider(Harness& self)
    {
        auto scripted_provider = make<ScriptedProvider>();
        self.provider = scripted_provider.ptr();
        return scripted_provider;
    }

    void begin_editing(StringView text = ""sv)
    {
        check_invariants();
        display_text = MUST(String::from_utf8(text));
        selection_start = {};
        omnibox.begin_editing(display_text);
        check_invariants();
    }

    void press_key(char code_point)
    {
        check_invariants();
        auto text_before_selection = selected_text_range_prefix();
        StringBuilder builder;
        builder.append(text_before_selection);
        builder.append(code_point);
        display_text = MUST(builder.to_string());
        selection_start = {};
        omnibox.text_edited(display_text, true);
        check_invariants();
    }

    void press_backspace()
    {
        check_invariants();
        omnibox.will_delete_text();
        auto text_before_selection = selected_text_range_prefix();
        if (!selection_start.has_value() && !text_before_selection.is_empty()) {
            // Remove the last code point, not the last byte.
            Utf8View code_points { text_before_selection };
            text_before_selection = code_points.unicode_substring_view(0, code_points.length() - 1).as_string();
        }
        display_text = MUST(String::from_utf8(text_before_selection));
        selection_start = {};
        omnibox.text_edited(display_text, true);
        check_invariants();
    }

    StringView selected_text_range_prefix() const
    {
        auto view = display_text.bytes_as_string_view();
        if (selection_start.has_value())
            return view.substring_view(0, *selection_start);
        return view;
    }

    ScriptedProvider* provider { nullptr };
    Omnibox omnibox;
    // Says how the test got here, for a failure in a generated test.
    Function<String()> describe_context;

    String display_text;
    Optional<size_t> selection_start;
    Vector<String> commits;
    Vector<WebView::OmniboxDestinationKind> commit_destination_kinds;

    size_t popup_repaints { 0 };
    bool visible_popup { false };
    Vector<AutocompleteSuggestion> visible_rows;
    Optional<size_t> visible_selection;
};

}

TEST_CASE(all_web_ui_pages_are_suggested)
{
    static constexpr Array expected_urls {
        "about:about"sv,
        "about:blocking"sv,
        "about:crashes"sv,
        "about:newtab"sv,
        "about:settings"sv,
        "about:welcome"sv,
    };

    auto suggestions = WebView::web_ui_autocomplete_suggestions("about:"sv);

    EXPECT_EQ(WebView::WebUI::pages().size(), expected_urls.size());
    EXPECT_EQ(suggestions.size(), expected_urls.size());
    for (size_t index = 0; index < expected_urls.size(); ++index) {
        EXPECT_EQ(suggestions[index].source, WebView::AutocompleteSuggestionSource::WebUI);
        EXPECT_EQ(suggestions[index].text, expected_urls[index]);
    }
}

TEST_CASE(paste_and_go_treats_external_urls_as_urls)
{
    EXPECT_EQ(Omnibox::text_for_paste_and_go_action("mailto:hello@example.com"_string, true), "Paste and Go to mailto:hello@example.com"sv);
    EXPECT_EQ(Omnibox::text_for_paste_and_go_action("hello world"_string, true), "Paste and Search for 'hello world'"sv);
}

TEST_CASE(web_ui_pages_are_matched_by_case_insensitive_url_prefix)
{
    auto suggestions = WebView::web_ui_autocomplete_suggestions("ABOUT:SET"sv);

    EXPECT_EQ(suggestions.size(), 1u);
    EXPECT_EQ(suggestions[0].text, "about:settings"sv);
    EXPECT_EQ(suggestions[0].title, "Settings"sv);
    EXPECT(suggestions[0].can_be_automatically_selected);
    EXPECT(suggestions[0].can_be_inline_completed);
}

TEST_CASE(web_ui_pages_are_not_suggested_for_unrelated_input)
{
    EXPECT(WebView::web_ui_autocomplete_suggestions("settings"sv).is_empty());
    EXPECT(WebView::web_ui_autocomplete_suggestions("about:foo"sv).is_empty());
}

TEST_CASE(fast_typing_publishes_each_local_generation)
{
    Harness harness;
    harness.begin_editing();

    // "t" completes to the top history hit for "t".
    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.twin.example/"sv), search_row("t"sv) });
    EXPECT_EQ(harness.display_text, "twin.example"sv);
    EXPECT_EQ(harness.selection_start, 1u);
    EXPECT(harness.omnibox.is_popup_visible());

    // "th" no longer matches it; the completion breaks, and a different suggestion takes over.
    harness.press_key('h');
    EXPECT_EQ(harness.display_text, "th"sv);
    harness.provider->deliver({ history_row("https://www.thee.example/"sv), search_row("th"sv) });
    EXPECT_EQ(harness.display_text, "thee.example"sv);

    // The user keeps typing into and past that suggestion.
    harness.press_key('e');
    EXPECT_EQ(harness.display_text, "thee.example"sv);
    EXPECT_EQ(harness.selection_start, 3u);
    harness.provider->deliver({ history_row("https://www.thee.example/"sv), search_row("the"sv) });

    harness.press_key('v');
    EXPECT_EQ(harness.display_text, "thev"sv);
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("thev"sv) });
    EXPECT_EQ(harness.display_text, "thev.example"sv);
    EXPECT_EQ(harness.selection_start, 4u);

    // Each generation publishes its local results immediately, so the popup and completion agree even
    // when the user types faster than remote suggestions arrive.
    EXPECT_EQ(harness.omnibox.suggestions().first().text, "https://www.thev.example/"sv);
    harness.omnibox.return_pressed();
    EXPECT_EQ(harness.commits.size(), 1u);
    EXPECT_EQ(harness.commits.last(), "https://www.thev.example/"sv);
}

TEST_CASE(editing_the_completion_submits_the_typed_text)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) });
    EXPECT_EQ(harness.display_text, "thev.example"sv);

    // Typing something that breaks the completion means Enter takes the text literally.
    harness.press_key('x');
    EXPECT_EQ(harness.display_text, "tx"sv);
    harness.provider->deliver({ search_row("tx"sv) });
    EXPECT_EQ(harness.display_text, "tx"sv);

    harness.omnibox.return_pressed();
    EXPECT_EQ(harness.commits.size(), 1u);
    EXPECT_EQ(harness.commits.last(), "tx"sv);
    EXPECT(!harness.omnibox.is_popup_visible());
}

TEST_CASE(backspacing_the_completion_submits_the_typed_text)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) });
    EXPECT_EQ(harness.display_text, "thev.example"sv);

    // Backspace deletes the selected completion; the suggestion must not come back for the same query.
    harness.press_backspace();
    EXPECT_EQ(harness.display_text, "t"sv);
    // Return goes to the search for the text as typed, so that row leads and is highlighted.
    EXPECT_EQ(harness.omnibox.suggestions().first().text, "t"sv);
    EXPECT_EQ(harness.omnibox.selected_suggestion(), 0u);
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) });
    EXPECT_EQ(harness.display_text, "t"sv);
    EXPECT_EQ(harness.omnibox.suggestions().first().text, "t"sv);
    EXPECT_EQ(harness.omnibox.selected_suggestion(), 0u);

    harness.omnibox.return_pressed();
    EXPECT_EQ(harness.commits.size(), 1u);
    EXPECT_EQ(harness.commits.last(), "t"sv);
}

TEST_CASE(typing_after_backspace_lifts_the_suppression)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) });
    harness.press_backspace();
    EXPECT_EQ(harness.display_text, "t"sv);

    // A new query means the user is typing again; completion resumes and Enter activates it.
    harness.press_key('h');
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("th"sv) });
    EXPECT_EQ(harness.display_text, "thev.example"sv);
    EXPECT_EQ(harness.selection_start, 2u);

    harness.omnibox.return_pressed();
    EXPECT_EQ(harness.commits.size(), 1u);
    EXPECT_EQ(harness.commits.last(), "https://www.thev.example/"sv);
}

TEST_CASE(rows_without_one_for_the_typed_text_highlight_none_after_rejecting_a_completion)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.thee.example/"sv), search_row("t"sv) });
    EXPECT(harness.omnibox.is_inline_completed());
    harness.press_backspace();
    EXPECT_EQ(harness.display_text, "t"sv);

    // Return goes to the text as typed, and the rows have no row for it, such as while the row for the text waits on
    // the check for an app that handles it. So none of them is highlighted.
    harness.provider->deliver({ non_inline_history_row("https://www.tango.example/"sv) });
    EXPECT(!harness.omnibox.selected_suggestion().has_value());
    EXPECT(!harness.omnibox.suggestion_for_return().has_value());
}

TEST_CASE(the_first_intermediate_result_repaints_for_each_generation)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.twin.example/"sv), search_row("t"sv) });
    EXPECT_EQ(harness.omnibox.suggestions().size(), 2u);
    EXPECT_EQ(harness.omnibox.suggestions().first().text, "https://www.twin.example/"sv);

    harness.press_key('h');
    harness.provider->deliver({ history_row("https://www.thee.example/"sv), search_row("th"sv) });
    EXPECT_EQ(harness.omnibox.suggestions().first().text, "https://www.thee.example/"sv);

    harness.provider->deliver({ history_row("https://www.thee.example/"sv), search_row("th"sv), search_row("th zzz"sv) });
    EXPECT_EQ(harness.omnibox.suggestions().size(), 3u);
    EXPECT_EQ(harness.omnibox.suggestions().first().text, "https://www.thee.example/"sv);

    // The popup is now fresh, so Enter activates the selected row without another engine round-trip.
    auto queries_before_return = harness.provider->queries.size();
    harness.omnibox.return_pressed();
    EXPECT_EQ(harness.provider->queries.size(), queries_before_return);
    EXPECT_EQ(harness.commits.size(), 1u);
    EXPECT_EQ(harness.commits.last(), "https://www.thee.example/"sv);
}

TEST_CASE(later_intermediate_results_refresh_the_active_generation)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ search_row("t"sv) });
    auto repaints_after_first_delivery = harness.popup_repaints;

    harness.provider->deliver({ history_row("https://www.thee.example/"sv, 201), search_row("t"sv) });

    EXPECT_EQ(harness.popup_repaints, repaints_after_first_delivery + 1);
    EXPECT_EQ(harness.omnibox.suggestions().first().text, "https://www.thee.example/"sv);
    EXPECT_EQ(harness.display_text, "thee.example"sv);
}

TEST_CASE(automatic_default_uses_point_and_percentage_hysteresis)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('a');
    harness.provider->deliver({
        non_inline_history_row("https://alpha.example/"sv, 2'000),
        search_row("a"sv),
    });

    // This challenger clears the point margin, but not the ten-percent margin.
    harness.provider->deliver({
        non_inline_history_row("https://another.example/"sv, 2'110),
        non_inline_history_row("https://alpha.example/"sv, 2'000),
        search_row("a"sv),
    });
    EXPECT_EQ(harness.omnibox.suggestions().first().text, "https://alpha.example/"sv);

    // Clearing both margins permits a default change.
    harness.provider->deliver({
        non_inline_history_row("https://another.example/"sv, 2'201),
        non_inline_history_row("https://alpha.example/"sv, 2'000),
        search_row("a"sv),
    });
    EXPECT_EQ(harness.omnibox.suggestions().first().text, "https://another.example/"sv);
}

TEST_CASE(inline_completion_has_a_larger_stability_margin)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('a');
    harness.provider->deliver({
        history_row("https://alpha.example/"sv, 1'000),
        search_row("a"sv),
    });
    EXPECT_EQ(harness.display_text, "alpha.example"sv);

    // The challenger clears the default margins but remains within the 150-point inline margin.
    harness.provider->deliver({
        history_row("https://another.example/"sv, 1'110),
        history_row("https://alpha.example/"sv, 1'000),
        search_row("a"sv),
    });
    EXPECT_EQ(harness.omnibox.suggestions().first().text, "https://alpha.example/"sv);
    EXPECT_EQ(harness.display_text, "alpha.example"sv);

    harness.provider->deliver({
        history_row("https://another.example/"sv, 1'151),
        history_row("https://alpha.example/"sv, 1'000),
        search_row("a"sv),
    });
    EXPECT_EQ(harness.omnibox.suggestions().first().text, "https://another.example/"sv);
    EXPECT_EQ(harness.display_text, "another.example"sv);
}

TEST_CASE(a_non_inline_default_can_replace_an_inline_completion)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('a');
    harness.provider->deliver({
        history_row("https://alpha.example/"sv, 1'000),
        search_row("a"sv),
    });
    EXPECT_EQ(harness.display_text, "alpha.example"sv);

    harness.provider->deliver({
        non_inline_history_row("https://another.example/"sv, 1'201),
        history_row("https://alpha.example/"sv, 1'000),
        search_row("a"sv),
    });
    EXPECT_EQ(harness.omnibox.suggestions().first().text, "https://another.example/"sv);
    EXPECT_EQ(harness.display_text, "a"sv);
}

TEST_CASE(explicit_selection_survives_same_generation_reordering)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('a');
    harness.provider->deliver({
        history_row("https://alpha.example/"sv, 1'000),
        search_row("a"sv),
    });
    EXPECT(harness.omnibox.select_next_suggestion());
    EXPECT_EQ(harness.omnibox.selected_suggestion(), 1u);

    harness.provider->deliver({
        history_row("https://another.example/"sv, 1'201),
        history_row("https://alpha.example/"sv, 1'000),
        search_row("a"sv),
    });
    EXPECT_EQ(harness.omnibox.selected_suggestion(), 2u);
    EXPECT_EQ(harness.display_text, "a"sv);

    harness.omnibox.return_pressed();
    EXPECT_EQ(harness.commits.last(), "a"sv);
    EXPECT(harness.provider->engagements.last().was_explicit);
}

TEST_CASE(typing_into_a_chosen_completion_keeps_it_chosen)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('g');
    harness.provider->deliver({ search_row("g"sv), history_row("https://gitlab.example/"sv) });
    EXPECT(harness.omnibox.select_next_suggestion());
    EXPECT_EQ(harness.display_text, "gitlab.example"sv);

    // The user typed further into the row they chose, so it is still their choice until the rows for the text arrive.
    harness.press_key('i');
    EXPECT_EQ(harness.display_text, "gitlab.example"sv);
    EXPECT(harness.omnibox.selection_is_explicit());

    harness.omnibox.return_pressed();
    EXPECT_EQ(harness.commits.last(), "https://gitlab.example/"sv);
    EXPECT(harness.provider->engagements.last().was_explicit);
}

TEST_CASE(search_selection_survives_equivalent_text_normalization)
{
    Harness harness;
    harness.begin_editing();

    for (auto code_point : "lady bird"sv)
        harness.press_key(code_point);
    harness.provider->deliver({
        non_inline_history_row("https://example.com/lady-bird"sv),
        search_row("lady bird"sv),
    });
    EXPECT(harness.omnibox.select_next_suggestion());
    EXPECT_EQ(harness.omnibox.selected_suggestion(), 1u);

    harness.provider->deliver({
        non_inline_history_row("https://example.com/lady-bird"sv),
        search_row("Lady   Bird"sv),
    });
    EXPECT_EQ(harness.omnibox.selected_suggestion(), 1u);

    harness.omnibox.return_pressed();
    EXPECT_EQ(harness.commits.last(), "Lady   Bird"sv);
    EXPECT(harness.provider->engagements.last().was_explicit);
}

TEST_CASE(enter_with_stale_results_goes_to_the_completion_on_display_immediately)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.thee.example/"sv), search_row("t"sv) });
    harness.press_key('h');
    EXPECT_EQ(harness.display_text, "thee.example"sv);

    // Enter with a stale popup never waits for a provider response. It goes where the field shows, as it would once
    // the rows arrive.
    harness.omnibox.return_pressed();
    EXPECT_EQ(harness.commits.size(), 1u);
    EXPECT_EQ(harness.commits.last(), "https://www.thee.example/"sv);

    // Later deliveries may update the popup if editing is still active, but cannot trigger a second
    // navigation.
    harness.provider->deliver({ search_row("th"sv) });
    harness.provider->deliver({ history_row("https://www.thee.example/"sv), search_row("th"sv) });
    EXPECT_EQ(harness.commits.size(), 1u);
}

TEST_CASE(enter_with_stale_results_and_no_completion_commits_the_current_input_immediately)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.thee.example/"sv), search_row("t"sv) });
    harness.press_key('x');
    EXPECT_EQ(harness.display_text, "tx"sv);

    harness.omnibox.return_pressed();
    EXPECT_EQ(harness.commits.size(), 1u);
    EXPECT_EQ(harness.commits.last(), "tx"sv);

    harness.provider->deliver({ search_row("tx"sv) });
    EXPECT_EQ(harness.commits.size(), 1u);
}

TEST_CASE(enter_with_stale_results_after_deleting_the_completion_commits_the_current_input)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.press_key('h');
    harness.provider->deliver({ history_row("https://www.thee.example/"sv), search_row("th"sv) });
    EXPECT_EQ(harness.display_text, "thee.example"sv);

    harness.press_backspace();
    EXPECT_EQ(harness.display_text, "th"sv);
    harness.omnibox.return_pressed();
    EXPECT_EQ(harness.commits.size(), 1u);
    EXPECT_EQ(harness.commits.last(), "th"sv);
}

TEST_CASE(escape_rejects_an_automatic_completion_and_closes_the_popup)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) });
    EXPECT_EQ(harness.display_text, "thev.example"sv);

    auto cancels_before_escape = harness.provider->cancel_count;
    EXPECT_EQ(harness.omnibox.escape_pressed(), Omnibox::EscapeAction::ClosedPopup);
    EXPECT(!harness.omnibox.is_popup_visible());
    EXPECT_EQ(harness.display_text, "t"sv);
    EXPECT_EQ(harness.provider->cancel_count, cancels_before_escape + 1);

    // With the popup gone, Enter submits the input the user actually entered.
    harness.omnibox.return_pressed();
    EXPECT_EQ(harness.commits.last(), "t"sv);

    EXPECT_EQ(harness.omnibox.escape_pressed(), Omnibox::EscapeAction::EndEditing);
}

TEST_CASE(arrow_keys_walk_the_suggestions_and_enter_activates_the_choice)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) });
    EXPECT_EQ(harness.omnibox.selected_suggestion(), 0u);
    EXPECT_EQ(harness.display_text, "thev.example"sv);

    // Down to the search row: its text equals the query, so the display returns to the typed text.
    EXPECT(harness.omnibox.select_next_suggestion());
    EXPECT_EQ(harness.omnibox.selected_suggestion(), 1u);
    EXPECT_EQ(harness.display_text, "t"sv);

    // And back up to the history row.
    EXPECT(harness.omnibox.select_previous_suggestion());
    EXPECT_EQ(harness.omnibox.selected_suggestion(), 0u);
    EXPECT_EQ(harness.display_text, "thev.example"sv);

    harness.omnibox.return_pressed();
    EXPECT_EQ(harness.commits.last(), "https://www.thev.example/"sv);
}

TEST_CASE(automatic_default_records_the_typed_input)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) });
    harness.omnibox.return_pressed();

    EXPECT_EQ(harness.provider->engagements.size(), 1u);
    EXPECT_EQ(harness.provider->engagements[0].input, "t"sv);
    EXPECT_EQ(harness.provider->engagements[0].destination_kind, WebView::OmniboxDestinationKind::URL);
    EXPECT_EQ(harness.provider->engagements[0].destination, "https://www.thev.example/"sv);
    EXPECT(!harness.provider->engagements[0].was_explicit);
}

TEST_CASE(keyboard_choice_records_explicit_engagement)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) });
    EXPECT(harness.omnibox.select_next_suggestion());
    harness.omnibox.return_pressed();

    EXPECT_EQ(harness.provider->engagements.size(), 1u);
    EXPECT_EQ(harness.provider->engagements[0].input, "t"sv);
    EXPECT_EQ(harness.provider->engagements[0].destination_kind, WebView::OmniboxDestinationKind::Search);
    EXPECT_EQ(harness.provider->engagements[0].destination, "t"sv);
    EXPECT(harness.provider->engagements[0].was_explicit);
}

TEST_CASE(verbatim_commit_records_the_unmodified_input)
{
    Harness harness;
    harness.begin_editing("ladybird browser"sv);

    harness.omnibox.return_pressed();

    EXPECT_EQ(harness.provider->engagements.size(), 1u);
    EXPECT_EQ(harness.provider->engagements[0].input, "ladybird browser"sv);
    EXPECT_EQ(harness.provider->engagements[0].destination_kind, WebView::OmniboxDestinationKind::Search);
    EXPECT_EQ(harness.provider->engagements[0].destination, "ladybird browser"sv);
    EXPECT(!harness.provider->engagements[0].was_explicit);
}

TEST_CASE(verbatim_url_candidates_use_url_or_search_resolution)
{
    static constexpr Array url_candidates {
        "geo:23.4,12.4"sv,
        "tel:123"sv,
        "localhost:8080"sv,
        "example.com:8080"sv,
    };

    for (auto input : url_candidates) {
        Harness harness;
        harness.begin_editing(input);

        harness.omnibox.return_pressed();

        EXPECT_EQ(harness.commits.size(), 1u);
        EXPECT_EQ(harness.commits[0], input);
        EXPECT_EQ(harness.commit_destination_kinds[0], WebView::OmniboxDestinationKind::URL);
    }
}

TEST_CASE(a_user_chosen_row_wins_over_edited_text)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) });

    // Break the completion so Enter would take the text literally...
    harness.press_key('x');
    harness.provider->deliver({ history_row("https://www.txt.example/"sv), search_row("tx"sv) });

    // ...but explicitly arrowing onto a row overrides that.
    EXPECT(harness.omnibox.select_next_suggestion());
    EXPECT_EQ(harness.omnibox.selected_suggestion(), 1u);
    harness.omnibox.return_pressed();
    EXPECT_EQ(harness.commits.last(), "tx"sv);
}

TEST_CASE(a_literal_url_suggestion_never_completes)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ literal_row("t.example/path"sv), history_row("https://www.thev.example/"sv) });
    EXPECT_EQ(harness.display_text, "t"sv);
    EXPECT_EQ(harness.omnibox.selected_suggestion(), 0u);

    harness.omnibox.return_pressed();
    EXPECT_EQ(harness.commits.last(), "t.example/path"sv);
}

TEST_CASE(a_web_ui_suggestion_completes_and_commits_as_a_url)
{
    Harness harness;
    harness.begin_editing();
    harness.display_text = "about:set"_string;
    harness.omnibox.text_edited(harness.display_text, true);

    harness.provider->deliver({ web_ui_row("about:settings"sv) });
    EXPECT_EQ(harness.display_text, "about:settings"sv);
    EXPECT_EQ(harness.selection_start, 9u);
    EXPECT_EQ(harness.omnibox.selected_suggestion(), 0u);

    harness.omnibox.return_pressed();
    EXPECT_EQ(harness.commits.last(), "about:settings"sv);
    EXPECT_EQ(harness.provider->engagements.last().destination_kind, WebView::OmniboxDestinationKind::URL);
}

TEST_CASE(an_automatic_default_does_not_imply_inline_completion)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ non_inline_history_row("https://www.thev.example/"sv), search_row("t"sv) });
    EXPECT_EQ(harness.display_text, "t"sv);
    EXPECT_EQ(harness.omnibox.selected_suggestion(), 0u);

    harness.omnibox.return_pressed();
    EXPECT_EQ(harness.commits.last(), "https://www.thev.example/"sv);
}

TEST_CASE(a_top_row_that_does_not_match_the_prefix_is_not_automatically_selected)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ non_automatic_history_row("https://odd.example/t"sv, "Odd"sv), search_row("t"sv) });
    EXPECT_EQ(harness.display_text, "t"sv);
    EXPECT_EQ(harness.omnibox.selected_suggestion(), 0u);
    EXPECT_EQ(harness.omnibox.suggestions().first().text, "t"sv);

    harness.omnibox.return_pressed();
    EXPECT_EQ(harness.commits.last(), "t"sv);
}

TEST_CASE(a_title_only_history_row_is_not_automatically_selected)
{
    Harness harness;
    harness.begin_editing();

    for (auto code_point : "eau de"sv)
        harness.press_key(code_point);

    harness.provider->deliver({ non_automatic_history_row("https://example.com/fragrance"sv, "Eau de Parfum"sv), search_row("eau de"sv) });
    EXPECT_EQ(harness.display_text, "eau de"sv);
    EXPECT_EQ(harness.omnibox.selected_suggestion(), 0u);
    EXPECT_EQ(harness.omnibox.suggestions().first().text, "eau de"sv);
    EXPECT(harness.omnibox.is_popup_visible());

    harness.omnibox.return_pressed();
    EXPECT_EQ(harness.commits.size(), 1u);
    EXPECT_EQ(harness.commits.last(), "eau de"sv);
}

TEST_CASE(a_user_chosen_title_only_history_row_is_activated)
{
    Harness harness;
    harness.begin_editing();

    for (auto code_point : "eau de"sv)
        harness.press_key(code_point);

    harness.provider->deliver({ non_automatic_history_row("https://example.com/fragrance"sv, "Eau de Parfum"sv), search_row("eau de"sv) });

    EXPECT(harness.omnibox.select_next_suggestion());
    EXPECT_EQ(harness.omnibox.selected_suggestion(), 1u);
    EXPECT_EQ(harness.display_text, "https://example.com/fragrance"sv);

    harness.omnibox.return_pressed();
    EXPECT_EQ(harness.commits.size(), 1u);
    EXPECT_EQ(harness.commits.last(), "https://example.com/fragrance"sv);
}

TEST_CASE(a_fresh_non_prefix_url_top_row_does_not_win_after_editing_a_completion)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) });
    EXPECT_EQ(harness.display_text, "thev.example"sv);

    // The user types over the completion, and the fresh top hit is not
    // selected merely because the query appears somewhere in its URL.
    for (auto code_point : "itle match"sv)
        harness.press_key(code_point);
    harness.provider->deliver({ non_automatic_history_row("https://news.ycombinator.com/title-match"sv, "Title Match"sv), search_row("title match"sv) });
    EXPECT_EQ(harness.display_text, "title match"sv);
    EXPECT_EQ(harness.omnibox.selected_suggestion(), 0u);
    EXPECT_EQ(harness.omnibox.suggestions().first().text, "title match"sv);

    harness.omnibox.return_pressed();
    EXPECT_EQ(harness.commits.size(), 1u);
    EXPECT_EQ(harness.commits.last(), "title match"sv);
}

TEST_CASE(clicking_a_suggestion_commits_it)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) });

    harness.omnibox.suggestion_clicked(1);
    EXPECT_EQ(harness.commits.last(), "t"sv);
    EXPECT_EQ(harness.commit_destination_kinds.last(), WebView::OmniboxDestinationKind::Search);
    EXPECT(!harness.omnibox.is_popup_visible());
}

TEST_CASE(late_results_after_editing_ends_are_ignored)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    auto cancels_before_end = harness.provider->cancel_count;
    harness.omnibox.end_editing();
    EXPECT_EQ(harness.provider->cancel_count, cancels_before_end + 1);

    harness.provider->deliver({ history_row("https://www.thev.example/"sv) });
    EXPECT(!harness.omnibox.is_popup_visible());
    EXPECT(harness.commits.is_empty());
}

TEST_CASE(showing_all_suggestions_does_not_complete_over_the_address)
{
    Harness harness;
    harness.begin_editing("https://site.example/page"sv);
    harness.omnibox.show_all_suggestions();
    EXPECT_EQ(harness.provider->queries.last(), "https://site.example/page"sv);

    harness.provider->deliver({ history_row("https://site.example/page"sv), history_row("https://site.example/page2"sv) });
    EXPECT_EQ(harness.display_text, "https://site.example/page"sv);
    EXPECT_EQ(harness.omnibox.selected_suggestion(), 0u);

    harness.omnibox.return_pressed();
    EXPECT_EQ(harness.commits.last(), "https://site.example/page"sv);
}

TEST_CASE(empty_results_close_the_popup_and_restore_the_query)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.thev.example/"sv) });
    EXPECT_EQ(harness.display_text, "thev.example"sv);

    harness.provider->deliver({});
    EXPECT(!harness.omnibox.is_popup_visible());
    EXPECT_EQ(harness.display_text, "t"sv);
}

TEST_CASE(no_completion_is_applied_when_the_cursor_is_not_at_the_end)
{
    Harness harness;
    harness.begin_editing();

    harness.omnibox.text_edited("t"_string, false);
    harness.display_text = "t"_string;
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) });
    EXPECT_EQ(harness.display_text, "t"sv);
    EXPECT(!harness.omnibox.selected_suggestion().has_value());

    // With nothing selected, Enter submits the typed text.
    harness.omnibox.return_pressed();
    EXPECT_EQ(harness.commits.last(), "t"sv);
}

TEST_CASE(dismissing_the_popup_restores_the_query)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) });
    EXPECT_EQ(harness.display_text, "thev.example"sv);

    // A click elsewhere in the window dismisses the popup without ending the editing session.
    harness.omnibox.popup_dismissed();
    EXPECT(!harness.omnibox.is_popup_visible());
    EXPECT_EQ(harness.display_text, "t"sv);
    EXPECT(harness.omnibox.is_editing());
}

TEST_CASE(deliveries_from_an_old_generation_are_ignored)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    auto old_query_id = harness.provider->query_ids.last();
    harness.press_key('h');

    harness.provider->deliver(old_query_id, { history_row("https://www.thee.example/"sv), search_row("t"sv) });
    EXPECT_EQ(harness.display_text, "th"sv);
    EXPECT(!harness.omnibox.is_popup_visible());
    EXPECT(harness.commits.is_empty());
}

TEST_CASE(a_surviving_preview_row_remains_the_users_choice)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), history_row("https://odd.example/t"sv), search_row("t"sv) });

    // Break the completion so Enter would otherwise take the query literally, then preview a row by
    // arrowing away and back onto it.
    harness.press_key('x');
    harness.provider->deliver({ history_row("https://odd.example/tx"sv), search_row("tx"sv) });
    EXPECT(harness.omnibox.select_next_suggestion());
    EXPECT(harness.omnibox.select_previous_suggestion());
    EXPECT_EQ(harness.display_text, "https://odd.example/tx"sv);

    // A refresh that still contains the previewed row keeps it selected as the user's choice.
    harness.provider->deliver({ history_row("https://odd.example/tx"sv), search_row("tx"sv) });
    EXPECT_EQ(harness.omnibox.selected_suggestion(), 0u);

    harness.omnibox.return_pressed();
    EXPECT_EQ(harness.commits.last(), "https://odd.example/tx"sv);
}

TEST_CASE(showing_all_suggestions_adopts_an_active_completion)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) });
    EXPECT_EQ(harness.display_text, "thev.example"sv);

    // The focus shortcut queries whatever the bar displays; results must not truncate the bar back to
    // the typed prefix.
    harness.omnibox.show_all_suggestions();
    EXPECT_EQ(harness.provider->queries.last(), "thev.example"sv);
    harness.provider->deliver({ history_row("https://www.thev.example/"sv) });
    EXPECT_EQ(harness.display_text, "thev.example"sv);
}

TEST_CASE(arrow_keys_reopen_a_dismissed_popup)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) });

    EXPECT_EQ(harness.omnibox.escape_pressed(), Omnibox::EscapeAction::ClosedPopup);
    EXPECT(!harness.omnibox.is_popup_visible());

    EXPECT(harness.omnibox.select_next_suggestion());
    EXPECT(harness.omnibox.is_popup_visible());
    EXPECT_EQ(harness.omnibox.selected_suggestion(), 0u);
}

TEST_CASE(a_new_editing_session_does_not_inherit_popup_rows)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) });
    harness.omnibox.end_editing();

    harness.begin_editing();
    EXPECT(harness.omnibox.suggestions().is_empty());
    EXPECT(!harness.omnibox.select_next_suggestion());
}

TEST_CASE(deliveries_while_suspended_are_dropped)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');

    // A context menu borrows focus without ending the session; a late delivery must not rewrite the
    // text or raise the popup while it is open.
    harness.omnibox.set_suspended(true);
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) });
    EXPECT_EQ(harness.display_text, "t"sv);
    EXPECT(!harness.omnibox.is_popup_visible());

    harness.omnibox.set_suspended(false);
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) });
    EXPECT_EQ(harness.display_text, "thev.example"sv);
}

TEST_CASE(no_completion_is_applied_after_the_cursor_moves_from_the_end)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('o');
    harness.press_key('d');

    // The user clicks into the middle of the text; a late delivery must not rewrite it and yank the
    // caret away.
    harness.omnibox.cursor_moved(false);
    harness.provider->deliver({ history_row("https://odd.example/"sv), search_row("od"sv) });
    EXPECT_EQ(harness.display_text, "od"sv);
}

TEST_CASE(final_results_repaint_a_visible_popup)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.twin.example/"sv), search_row("t"sv) });
    EXPECT(harness.visible_popup);
    EXPECT_EQ(harness.visible_rows.first().text, "https://www.twin.example/"sv);

    // The final result set replaces the rows under an already-open popup; the chrome must be told.
    harness.provider->deliver({ history_row("https://www.twin.example/"sv), search_row("t"sv), search_row("t zzz"sv) });
    EXPECT_EQ(harness.visible_rows.size(), 3u);
    EXPECT_EQ(harness.visible_rows.last().text, "t zzz"sv);
    EXPECT_EQ(harness.visible_selection, 0u);
}

TEST_CASE(a_user_highlighted_completion_survives_a_refresh)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) });

    // Walk away and back so the highlight is a deliberate act.
    EXPECT(harness.omnibox.select_next_suggestion());
    EXPECT(harness.omnibox.select_previous_suggestion());
    EXPECT_EQ(harness.display_text, "thev.example"sv);

    // A refresh that keeps the chosen row selected keeps it the user's choice, so Escape restores the
    // typed text instead of preserving the completion.
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) });
    EXPECT_EQ(harness.omnibox.escape_pressed(), Omnibox::EscapeAction::ClosedPopup);
    EXPECT_EQ(harness.display_text, "t"sv);
}

TEST_CASE(provider_results_after_a_stale_enter_do_not_navigate)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.thee.example/"sv), search_row("t"sv) });
    harness.press_key('h');
    harness.omnibox.return_pressed();
    EXPECT_EQ(harness.commits.size(), 1u);
    EXPECT_EQ(harness.commits.last(), "https://www.thee.example/"sv);

    // Provider updates may continue until the chrome's commit handler ends editing, but no response is
    // armed to perform another navigation.
    harness.provider->deliver({});
    harness.provider->deliver({ history_row("https://www.thee.example/"sv), search_row("th"sv) });
    EXPECT_EQ(harness.commits.size(), 1u);
}

TEST_CASE(deleting_from_a_previewed_row_does_not_highlight_rows_for_older_text)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('e');
    harness.press_key('x');
    auto search = search_row("ex"sv);
    search.is_verbatim = true;
    harness.provider->deliver({ move(search), non_automatic_history_row("https://other.example/foo"sv, "Ex"sv) });
    EXPECT(harness.omnibox.select_next_suggestion());
    auto previewed_text = harness.display_text;
    EXPECT_NE(previewed_text, "ex"sv);

    // The user moves the cursor to the end of the previewed text, and deletes a character from it.
    harness.selection_start = {};
    harness.omnibox.cursor_moved(true);
    harness.press_backspace();
    EXPECT_NE(harness.omnibox.query(), "ex"sv);
    auto const& rows = harness.omnibox.suggestions();
    if (auto selection = harness.omnibox.selected_suggestion(); selection.has_value())
        EXPECT_NE(rows[*selection].text, "ex"sv);

    // Return goes to the edited text, not to the row it was edited from.
    auto edited_text = harness.display_text;
    harness.omnibox.return_pressed();
    EXPECT_EQ(harness.commits.size(), 1u);
    EXPECT_EQ(harness.commits.last(), edited_text);
}

TEST_CASE(a_completion_for_fewer_rows_than_the_chosen_row_does_not_read_past_them)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://ta.example/"sv), history_row("https://tb.example/"sv), history_row("https://tc.example/"sv), history_row("https://td.example/"sv) });
    EXPECT(harness.omnibox.select_next_suggestion());
    EXPECT(harness.omnibox.select_next_suggestion());
    EXPECT(harness.omnibox.select_next_suggestion());
    EXPECT_EQ(harness.omnibox.selected_suggestion(), 3u);

    // The user types after the chosen row, and the new rows are fewer than the row that was chosen. Completing to the
    // top row redraws the editing icon before the selection for the new rows is decided.
    harness.press_key('x');
    auto completed_row = MUST(String::formatted("https://{}yz.example/", harness.display_text));
    harness.provider->deliver({ history_row(completed_row) });
    EXPECT(harness.omnibox.is_inline_completed());
    EXPECT_EQ(harness.omnibox.selected_suggestion(), 0u);
}

TEST_CASE(rows_held_while_suspended_show_on_resuming)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.omnibox.set_suspended(true);
    harness.provider->deliver({ search_row("t"sv) });
    EXPECT(harness.omnibox.has_pending_suggestions());
    EXPECT(!harness.omnibox.is_popup_visible());

    // The rows show without asking for them again.
    auto queries_before_resuming = harness.provider->queries.size();
    harness.omnibox.set_suspended(false);
    EXPECT_EQ(harness.provider->queries.size(), queries_before_resuming);
    EXPECT(!harness.omnibox.has_pending_suggestions());
    EXPECT(harness.omnibox.is_popup_visible());
}

TEST_CASE(refreshed_rows_held_while_suspended_keep_the_chosen_row)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ search_row("t"sv), non_inline_history_row("https://www.tango.example/"sv) });
    EXPECT(harness.omnibox.select_next_suggestion());
    auto previewed_text = harness.display_text;

    // The remote rows refresh the rows on display while a context menu is open.
    harness.omnibox.set_suspended(true);
    harness.provider->deliver({ search_row("t"sv), non_inline_history_row("https://www.tango.example/"sv), search_row("tea"sv) });

    auto queries_before_resuming = harness.provider->queries.size();
    harness.omnibox.set_suspended(false);
    EXPECT_EQ(harness.provider->queries.size(), queries_before_resuming);
    EXPECT_EQ(harness.omnibox.suggestions().size(), 3u);
    EXPECT(harness.omnibox.selection_is_explicit());
    EXPECT_EQ(harness.display_text, previewed_text);
}

TEST_CASE(no_rows_with_the_popup_closed_still_tell_the_chrome)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    auto repaints_before = harness.popup_repaints;
    harness.provider->deliver({});
    EXPECT(!harness.omnibox.has_pending_suggestions());
    EXPECT(!harness.omnibox.is_popup_visible());
    EXPECT_EQ(harness.popup_repaints, repaints_before + 1);
}

TEST_CASE(restarting_the_session_cancels_in_flight_queries)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    auto cancels_before_restart = harness.provider->cancel_count;

    // A programmatic text change (for example a redirect landing) restarts the session; results still in
    // flight for the old query must not deliver into it.
    harness.begin_editing("https://site.example/"sv);
    EXPECT_EQ(harness.provider->cancel_count, cancels_before_restart + 1);
    EXPECT(!harness.omnibox.is_popup_visible());
}

TEST_CASE(a_moved_cursor_blocks_completions_under_any_provenance)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) });
    EXPECT_EQ(harness.display_text, "thev.example"sv);

    // The user clicks into the middle of the completed text. The borrowed suffix is removed and a
    // late delivery must not grow it back for the same input.
    harness.omnibox.cursor_moved(false);
    EXPECT_EQ(harness.display_text, "t"sv);
    harness.provider->deliver({ history_row("https://www.thevx.example/"sv), search_row("t"sv) });
    EXPECT_EQ(harness.display_text, "t"sv);
    EXPECT(!harness.omnibox.selected_suggestion().has_value());
}

TEST_CASE(accepting_a_completion_requeries_without_losing_the_learning_prefix)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('g');
    harness.press_key('i');
    harness.provider->deliver({ history_row("https://github.com/LadybirdBrowser/ladybird"sv), search_row("gi"sv) });
    EXPECT_EQ(harness.display_text, "github.com/LadybirdBrowser/ladybird"sv);
    EXPECT(harness.selection_start.has_value());

    EXPECT(harness.omnibox.accept_completion());
    EXPECT_EQ(harness.display_text, "github.com/LadybirdBrowser/ladybird"sv);
    EXPECT(!harness.selection_start.has_value());
    EXPECT_EQ(harness.provider->queries.last(), "github.com/LadybirdBrowser/ladybird"sv);

    harness.provider->deliver({ history_row("https://github.com/LadybirdBrowser/ladybird"sv) });
    harness.omnibox.return_pressed();
    EXPECT_EQ(harness.provider->engagements.size(), 1u);
    EXPECT_EQ(harness.provider->engagements[0].input, "gi"sv);
    EXPECT_EQ(harness.provider->engagements[0].destination, "https://github.com/LadybirdBrowser/ladybird"sv);
}

TEST_CASE(direct_navigation_does_not_retain_previous_engagement_input)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('g');
    harness.press_key('i');
    harness.provider->deliver({ history_row("https://github.com/LadybirdBrowser/ladybird"sv), search_row("gi"sv) });

    EXPECT(harness.omnibox.accept_completion());

    harness.omnibox.navigate_directly_to_query("https://example.com/"_string);

    EXPECT_EQ(harness.provider->engagements.size(), 1u);
    EXPECT_EQ(harness.provider->engagements[0].input, "https://example.com/"sv);
    EXPECT_EQ(harness.provider->engagements[0].destination, "https://example.com/"sv);
}

TEST_CASE(committing_may_reentrantly_end_editing)
{
    Harness harness;
    harness.begin_editing();

    // The real chrome clears focus while handling a commit, which calls back into end_editing.
    harness.omnibox.on_commit = [&](String text) {
        harness.commits.append(move(text));
        harness.commit_destination_kinds.append(harness.omnibox.destination_kind_for_last_commit());
        harness.omnibox.end_editing();
    };

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) });

    harness.omnibox.return_pressed();
    EXPECT_EQ(harness.commits.size(), 1u);
    EXPECT_EQ(harness.commits.last(), "https://www.thev.example/"sv);
    EXPECT(!harness.omnibox.is_editing());
    EXPECT(!harness.omnibox.is_popup_visible());
}

namespace {

// A small deterministic generator, so a failing walk can be replayed from its seed.
class WalkRandom {
public:
    explicit WalkRandom(u64 seed)
        : m_state(seed)
    {
    }

    u64 next()
    {
        m_state += 0x9e3779b97f4a7c15;
        auto value = m_state;
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9;
        value = (value ^ (value >> 27)) * 0x94d049bb133111eb;
        return value ^ (value >> 31);
    }

    size_t below(size_t bound) { return next() % bound; }

private:
    u64 m_state { 0 };
};

struct WalkPage {
    StringView url;
    StringView title;
};

static constexpr Array walk_pages {
    WalkPage { "https://www.reddit.com/"sv, "Reddit"sv },
    WalkPage { "https://reddit.com/r/programming/"sv, "Programming"sv },
    WalkPage { "https://www.thev.example/"sv, "The V"sv },
    WalkPage { "https://the.example/news"sv, "The News"sv },
    WalkPage { "https://ladybird.org/"sv, "Ladybird"sv },
    WalkPage { "https://example.com/fragrance"sv, "Eau de Parfum"sv },
};

StringView walk_page_address(StringView url)
{
    if (auto scheme_end = url.find("://"sv); scheme_end.has_value())
        url = url.substring_view(*scheme_end + 3);
    if (url.starts_with("www."sv))
        url = url.substring_view(4);
    return url;
}

// The rows a query would get, put in order by the real muxer. Remote rows come in a later delivery, as they do.
Vector<AutocompleteSuggestion> walk_rows(String const& query, bool with_remote_rows)
{
    auto trimmed_query = query.bytes_as_string_view().trim_whitespace();
    if (trimmed_query.is_empty())
        return {};

    auto verbatim = search_row(trimmed_query);
    verbatim.is_verbatim = true;
    verbatim.relevance = 900;
    verbatim.can_be_inline_completed = false;

    Vector<AutocompleteSuggestion> local_rows;
    for (size_t index = 0; index < walk_pages.size(); ++index) {
        auto const& page = walk_pages[index];
        if (walk_page_address(page.url).starts_with(trimmed_query, CaseSensitivity::CaseInsensitive)) {
            auto suggestion = history_row(page.url, 1000 - static_cast<i32>(index) * 10);
            suggestion.title = MUST(String::from_utf8(page.title));
            suggestion.match_class = WebView::AutocompleteMatchClass::URLPrefix;
            local_rows.append(move(suggestion));
        } else if (trimmed_query.length() >= 3 && page.title.starts_with(trimmed_query, CaseSensitivity::CaseInsensitive)) {
            local_rows.append(non_automatic_history_row(page.url, page.title));
            local_rows.last().relevance = 950;
        }
    }

    Vector<AutocompleteSuggestion> remote_rows;
    if (with_remote_rows) {
        for (auto suffix : { " news"sv, " 2026"sv }) {
            auto remote = search_row(MUST(String::formatted("{}{}", trimmed_query, suffix)));
            remote.relevance = 700;
            remote.can_be_automatically_selected = false;
            remote_rows.append(move(remote));
        }
    }

    return WebView::mux_autocomplete_suggestions(trimmed_query, move(verbatim), move(local_rows), move(remote_rows), 8);
}

// How often the walks reach the states worth checking, so they cannot quietly stop reaching them.
struct WalkCoverage {
    size_t inline_completions { 0 };
    size_t explicit_selections { 0 };
    size_t pending_steps { 0 };
    size_t returns_to_suggestions { 0 };
    size_t returns_to_typed_text { 0 };
};

void walk(u64 seed, size_t step_count, WalkCoverage& coverage)
{
    WalkRandom random(seed);
    // NB: The harness checks its invariants once more as it is destroyed, and a failure there describes the steps, so
    //     they must outlive it.
    Vector<String> steps;
    Harness harness;
    harness.describe_context = [&] {
        return MUST(String::formatted("\n    seed {}, steps: {}", seed, MUST(String::join(", "sv, steps))));
    };

    auto restart = [&] {
        harness.omnibox.end_editing();
        harness.begin_editing();
    };

    harness.begin_editing();
    static constexpr auto typed_characters = "redthvlaybi eaux"sv;

    for (size_t step = 0; step < step_count; ++step) {
        auto action = random.below(100);
        if (action < 35) {
            auto character = typed_characters[random.below(typed_characters.length())];
            steps.append(MUST(String::formatted("type '{}'", character)));
            harness.press_key(character);
        } else if (action < 47) {
            steps.append("backspace"_string);
            harness.press_backspace();
        } else if (action < 62) {
            steps.append("deliver"_string);
            if (!harness.provider->query_ids.is_empty())
                harness.provider->deliver(walk_rows(harness.provider->queries.last(), false));
        } else if (action < 70) {
            steps.append("deliver with remote rows"_string);
            if (!harness.provider->query_ids.is_empty())
                harness.provider->deliver(walk_rows(harness.provider->queries.last(), true));
        } else if (action < 74) {
            // A late answer to an earlier query must change nothing.
            steps.append("deliver stale"_string);
            if (harness.provider->query_ids.size() >= 2) {
                auto index = random.below(harness.provider->query_ids.size() - 1);
                harness.provider->deliver(harness.provider->query_ids[index], walk_rows(harness.provider->queries[index], true));
            }
        } else if (action < 81) {
            steps.append("down"_string);
            harness.omnibox.select_next_suggestion();
        } else if (action < 85) {
            steps.append("up"_string);
            harness.omnibox.select_previous_suggestion();
        } else if (action < 89) {
            steps.append("tab"_string);
            harness.omnibox.accept_completion();
        } else if (action < 93) {
            steps.append("escape"_string);
            if (harness.omnibox.escape_pressed() == Omnibox::EscapeAction::EndEditing)
                restart();
        } else {
            steps.append("return"_string);
            if (harness.display_text.is_empty())
                continue;

            // Return goes exactly where the field said it would.
            auto target = harness.omnibox.suggestion_for_return();
            auto expected = target.has_value() ? target->text : harness.omnibox.query();
            auto icon_says_search = harness.omnibox.editing_icon().kind == Omnibox::EditingIcon::Kind::Search;
            ++(target.has_value() ? coverage.returns_to_suggestions : coverage.returns_to_typed_text);
            auto commit_count = harness.commits.size();
            harness.omnibox.return_pressed();
            if (harness.commits.size() != commit_count + 1 || harness.commits.last() != expected) {
                FAIL(MUST(String::formatted("Return did not go to '{}'{}", expected, harness.describe_context())));
                return;
            }
            // The icon said whether Return would search or go to a page.
            if (icon_says_search != (harness.commit_destination_kinds.last() == WebView::OmniboxDestinationKind::Search)) {
                FAIL(MUST(String::formatted("The icon did not say where Return went for '{}'{}", expected, harness.describe_context())));
                return;
            }
            restart();
        }
        harness.check_invariants();
        if (Test::current_test_result() == Test::TestResult::Failed)
            return;

        coverage.inline_completions += harness.omnibox.is_inline_completed() ? 1 : 0;
        coverage.explicit_selections += harness.omnibox.selection_is_explicit() ? 1 : 0;
        coverage.pending_steps += harness.omnibox.has_pending_suggestions() ? 1 : 0;
    }
}

}

TEST_CASE(random_editing_keeps_the_omnibox_consistent)
{
    WalkCoverage coverage;
    for (u64 seed = 1; seed <= 400; ++seed) {
        walk(seed, 60, coverage);
        if (Test::current_test_result() == Test::TestResult::Failed)
            return;
    }

    EXPECT(coverage.inline_completions >= 100);
    EXPECT(coverage.explicit_selections >= 100);
    EXPECT(coverage.pending_steps >= 100);
    EXPECT(coverage.returns_to_suggestions >= 50);
    EXPECT(coverage.returns_to_typed_text >= 50);
}

// End-to-end scenarios: a profile's history, bookmarks and learned engagements go through the real ranker and muxer,
// as Autocomplete does, and the omnibox shows the result. Each scenario says what the user sees and where Return goes.
namespace {

constexpr i64 scenario_now_seconds = 2'000'000;

UnixDateTime scenario_now() { return UnixDateTime::from_seconds_since_epoch(scenario_now_seconds); }

ByteBuffer scenario_favicon()
{
    return MUST(ByteBuffer::copy("favicon"sv.bytes()));
}

struct ScenarioProfile {
    Vector<WebView::HistoryEntry> history;
    Vector<WebView::AutocompleteBookmark> bookmarks;
    Vector<WebView::StoredOmniboxEngagement> engagements;
    Optional<WebView::SearchEngine> search_engine { WebView::builtin_search_engines().first() };

    void visit(StringView url, StringView title, i64 direct_visit_count, bool has_favicon = true)
    {
        history.append({
            .url = MUST(String::from_utf8(url)),
            .title = MUST(String::from_utf8(title)),
            .favicon_png = has_favicon ? Optional<ByteBuffer> { scenario_favicon() } : OptionalNone {},
            .visit_count = direct_visit_count,
            .direct_visit_count = direct_visit_count,
            .last_visited_time = scenario_now(),
            .last_qualifying_visit_time = scenario_now(),
            .last_direct_visit_time = scenario_now(),
            .decayed_visit_score = static_cast<double>(direct_visit_count),
            .decayed_direct_score = static_cast<double>(direct_visit_count),
            .score_updated_at = scenario_now(),
        });
    }

    // The user went to the destination after typing the input, a number of times.
    void learn(StringView input, StringView url, i64 explicit_use_count, i64 default_use_count)
    {
        engagements.append({
            .normalized_input = MUST(String::from_utf8(input)),
            .destination_kind = WebView::OmniboxDestinationKind::URL,
            .destination = MUST(String::from_utf8(url)),
            .explicit_use_count = explicit_use_count,
            .default_use_count = default_use_count,
            .last_used_time = scenario_now(),
        });
    }

    // The rows for a query, assembled the way Autocomplete and its service assemble them once every source answered.
    // The remote suggestions are the search engine's answer for this query.
    Vector<AutocompleteSuggestion> rows(String const& query, Vector<String> remote_suggestions = {}) const
    {
        auto trimmed_query = query.bytes_as_string_view().trim_whitespace();
        if (trimmed_query.is_empty())
            return {};

        auto limit = WebView::autocomplete_preliminary_limit(WebView::default_autocomplete_suggestion_limit);
        auto local_rows = WebView::web_ui_autocomplete_suggestions(trimmed_query);
        Optional<AutocompleteSuggestion> verbatim_row;
        if (local_rows.is_empty()) {
            auto classified_query = WebView::classify_user_input(trimmed_query);
            verbatim_row = WebView::make_verbatim_autocomplete_suggestion(query, classified_query, search_engine);
            if (auto search = WebView::make_search_suggestion_for_address_like_text(query, classified_query, search_engine); search.has_value())
                local_rows.append(search.release_value());
        }
        local_rows.extend(WebView::rank_history_suggestions(trimmed_query, history, limit, scenario_now()));
        local_rows.extend(WebView::rank_bookmark_suggestions(trimmed_query, bookmarks, limit));
        local_rows.extend(WebView::rank_engagement_suggestions(trimmed_query, engagements, limit, scenario_now()));
        WebView::show_search_results_pages_as_searches(trimmed_query, local_rows, search_engine);

        auto remote_rows = WebView::make_remote_autocomplete_suggestions(move(remote_suggestions), local_rows);
        auto rows = WebView::mux_autocomplete_suggestions(
            trimmed_query,
            move(verbatim_row),
            move(local_rows),
            move(remote_rows),
            WebView::default_autocomplete_suggestion_limit);
        for (auto& row : rows)
            row.highlight_input = MUST(String::from_utf8(trimmed_query));
        return rows;
    }
};

struct Scenario {
    explicit Scenario(ScenarioProfile profile)
        : profile(move(profile))
    {
        harness.begin_editing();
    }

    // Types each character and answers each query before the next key press, as a fast local profile does.
    // NB: The search engine answers each query with those of the remote suggestions that start with it.
    void type(StringView text, Vector<String> const& remote_suggestions = {})
    {
        for (auto character : text) {
            harness.press_key(character);
            auto query = harness.provider->queries.last().bytes_as_string_view().trim_whitespace();
            Vector<String> answer;
            for (auto const& suggestion : remote_suggestions) {
                if (WebView::remote_autocomplete_suggestion_matches_input(suggestion, query))
                    answer.append(suggestion);
            }
            deliver(move(answer));
        }
    }

    void backspace()
    {
        harness.press_backspace();
        deliver();
    }

    void deliver(Vector<String> remote_suggestions = {})
    {
        harness.provider->deliver(profile.rows(harness.provider->queries.last(), move(remote_suggestions)));
    }

    AutocompleteSuggestion const& top_row() const { return harness.omnibox.suggestions().first(); }
    bool icon_is_search() const { return harness.omnibox.editing_icon().kind == Omnibox::EditingIcon::Kind::Search; }
    bool icon_is_page_with_favicon() const
    {
        auto icon = harness.omnibox.editing_icon();
        return icon.kind == Omnibox::EditingIcon::Kind::Page && icon.favicon_png.has_value();
    }
    bool top_row_is_highlighted() const { return harness.omnibox.selected_suggestion() == 0u; }

    // Where Return would go, as text.
    String return_destination() const
    {
        auto target = harness.omnibox.suggestion_for_return();
        return target.has_value() ? target->text : harness.omnibox.query();
    }

    bool return_searches() const
    {
        auto target = harness.omnibox.suggestion_for_return();
        if (target.has_value())
            return target->source == AutocompleteSuggestionSource::Search;
        return WebView::classify_user_input(harness.omnibox.query()).classification == WebView::UserInputClassification::Search;
    }

    // The rows that go to an address, given without its scheme, "www." or trailing slash.
    size_t rows_for(StringView address) const
    {
        size_t count = 0;
        for (auto const& row : harness.omnibox.suggestions()) {
            if (row.source != AutocompleteSuggestionSource::Search && walk_page_address(row.text).trim("/"sv, TrimMode::Right) == address)
                ++count;
        }
        return count;
    }

    ScenarioProfile profile;
    Harness harness;
};

ScenarioProfile reddit_profile()
{
    // Typing reddit.com went to https://reddit.com/, which redirected to www.reddit.com, where the favicon is.
    ScenarioProfile profile;
    profile.visit("https://www.reddit.com/"sv, "Reddit - The heart of the internet"sv, 11);
    for (auto input : { "r"sv, "re"sv, "red"sv, "redd"sv, "reddit"sv, "reddit.com"sv })
        profile.learn(input, "https://reddit.com/"sv, 1, 3);
    return profile;
}

}

TEST_CASE(scenario_typing_a_visited_site_completes_it_with_its_favicon)
{
    Scenario scenario(reddit_profile());
    EXPECT(scenario.icon_is_search());
    scenario.type("red"sv);

    EXPECT_EQ(scenario.harness.display_text, "reddit.com"sv);
    EXPECT_EQ(scenario.harness.selection_start, 3u);
    EXPECT(scenario.top_row_is_highlighted());
    EXPECT(scenario.top_row().favicon_png.has_value());
    EXPECT_EQ(scenario.top_row().title, "Reddit - The heart of the internet"sv);
    EXPECT_EQ(scenario.harness.omnibox.completion_title(), "Reddit - The heart of the internet"sv);
    EXPECT_EQ(scenario.rows_for("reddit.com"sv), 1u);
    EXPECT(!scenario.return_searches());
    EXPECT(scenario.icon_is_page_with_favicon());
    EXPECT(walk_page_address(scenario.return_destination()) == "reddit.com/"sv);
}

TEST_CASE(scenario_backspacing_a_completion_leads_with_the_search)
{
    Scenario scenario(reddit_profile());
    scenario.type("red"sv);
    scenario.backspace();

    EXPECT_EQ(scenario.harness.display_text, "red"sv);
    EXPECT_EQ(scenario.top_row().source, AutocompleteSuggestionSource::Search);
    EXPECT_EQ(scenario.top_row().text, "red"sv);
    EXPECT(scenario.top_row_is_highlighted());
    EXPECT(scenario.return_searches());
    EXPECT(scenario.icon_is_search());
    EXPECT_EQ(scenario.return_destination(), "red"sv);
}

TEST_CASE(scenario_a_page_matching_only_by_title_does_not_take_return)
{
    ScenarioProfile profile;
    profile.visit("https://example.com/fragrance"sv, "Eau de Parfum"sv, 3);
    Scenario scenario(move(profile));
    scenario.type("eau de"sv);

    EXPECT_EQ(scenario.harness.display_text, "eau de"sv);
    EXPECT_EQ(scenario.top_row().text, "eau de"sv);
    EXPECT(scenario.top_row_is_highlighted());
    EXPECT(scenario.return_searches());
    EXPECT(scenario.icon_is_search());
    EXPECT_EQ(scenario.rows_for("example.com/fragrance"sv), 1u);
}

// The user picked a deep page of a site once after typing the start of its address, and pressed Return on it twice
// since. Typing that start again completes to the site, as Return goes where the text shows. A second pick of the page
// makes it the completion.
TEST_CASE(scenario_a_deep_page_picked_once_does_not_take_the_start_of_its_address)
{
    auto profile_with_picks = [](i64 explicit_use_count) {
        ScenarioProfile profile;
        profile.visit("https://www.amazon.example/book/dp/123/"sv, "A Book"sv, 7);
        profile.visit("https://www.amazon.example/"sv, "Amazon"sv, 2);
        profile.learn("ama"sv, "https://www.amazon.example/book/dp/123/"sv, explicit_use_count, 2);
        return profile;
    };

    Scenario picked_once(profile_with_picks(1));
    picked_once.type("ama"sv);
    EXPECT_EQ(picked_once.harness.display_text, "amazon.example"sv);
    EXPECT_EQ(picked_once.harness.selection_start, 3u);
    EXPECT(walk_page_address(picked_once.return_destination()) == "amazon.example/"sv);
    EXPECT_EQ(picked_once.rows_for("amazon.example/book/dp/123"sv), 1u);

    Scenario picked_twice(profile_with_picks(2));
    picked_twice.type("ama"sv);
    EXPECT_EQ(picked_twice.harness.display_text, "amazon.example/book/dp/123/"sv);
    EXPECT_EQ(picked_twice.harness.selection_start, 3u);
    EXPECT(walk_page_address(picked_twice.return_destination()) == "amazon.example/book/dp/123/"sv);
}

TEST_CASE(scenario_a_remote_address_of_an_unvisited_site_is_a_search)
{
    Scenario scenario({});
    scenario.type("goog"sv, { "google.com"_string, "google maps"_string });

    EXPECT_EQ(scenario.top_row().text, "goog"sv);
    EXPECT(scenario.top_row_is_highlighted());
    EXPECT(scenario.icon_is_search());
    auto const& rows = scenario.harness.omnibox.suggestions();
    EXPECT(rows.contains([](auto const& row) { return row.text == "google.com"sv && row.source == AutocompleteSuggestionSource::Search; }));
    EXPECT_EQ(scenario.rows_for("google.com"sv), 0u);
}

TEST_CASE(scenario_a_remote_address_merges_with_the_visited_page)
{
    ScenarioProfile profile;
    profile.visit("https://www.google.com/"sv, "Google"sv, 6);
    Scenario scenario(move(profile));
    scenario.type("goog"sv, { "google.com"_string });

    EXPECT_EQ(scenario.rows_for("google.com"sv), 1u);
    EXPECT(scenario.top_row().favicon_png.has_value());
    EXPECT_EQ(scenario.harness.display_text, "google.com"sv);
    EXPECT(scenario.icon_is_page_with_favicon());
}

TEST_CASE(scenario_a_typed_address_goes_to_that_address)
{
    Scenario scenario({});
    scenario.type("ladybird.org"sv, { "ladybird.org"_string });

    EXPECT(scenario.top_row_is_highlighted());
    EXPECT_EQ(scenario.top_row().source, AutocompleteSuggestionSource::LiteralURL);
    EXPECT_EQ(scenario.rows_for("ladybird.org"sv), 1u);
    EXPECT(!scenario.return_searches());
    EXPECT(!scenario.icon_is_search());
}

TEST_CASE(scenario_without_a_search_engine_offers_to_choose_one)
{
    ScenarioProfile profile;
    profile.search_engine = {};
    Scenario scenario(move(profile));
    scenario.type("red panda"sv);

    EXPECT_EQ(scenario.top_row().source, AutocompleteSuggestionSource::ChooseSearchEngine);
    EXPECT(scenario.top_row_is_highlighted());
    EXPECT(scenario.icon_is_search());
}

TEST_CASE(scenario_the_icon_stays_while_typing_through_a_completion)
{
    Scenario scenario(reddit_profile());
    scenario.type("red"sv);
    EXPECT(scenario.icon_is_page_with_favicon());

    // The next key press keeps the completion, and Return still goes to it before its suggestions arrive.
    scenario.harness.press_key('d');
    EXPECT_EQ(scenario.harness.display_text, "reddit.com"sv);
    EXPECT(scenario.harness.omnibox.has_pending_suggestions());
    EXPECT(scenario.icon_is_page_with_favicon());
    scenario.deliver();
    EXPECT(scenario.icon_is_page_with_favicon());

    // Backspacing over the completion shows the search at once, while its suggestions are on their way.
    scenario.harness.press_backspace();
    EXPECT(scenario.harness.omnibox.has_pending_suggestions());
    EXPECT(scenario.icon_is_search());
}

TEST_CASE(scenario_backspacing_a_completion_without_a_search_engine_offers_to_choose_one)
{
    auto profile = reddit_profile();
    profile.search_engine = {};
    Scenario scenario(move(profile));
    scenario.type("red"sv);
    EXPECT_EQ(scenario.harness.display_text, "reddit.com"sv);

    scenario.backspace();
    EXPECT_EQ(scenario.harness.display_text, "red"sv);
    EXPECT_EQ(scenario.top_row().source, AutocompleteSuggestionSource::ChooseSearchEngine);
    EXPECT(scenario.top_row_is_highlighted());
    EXPECT_EQ(scenario.return_destination(), "about:settings#search"sv);
}

TEST_CASE(scenario_typed_text_that_looks_like_an_address_also_offers_a_search)
{
    Scenario scenario({});
    scenario.type("setup.py"sv);

    EXPECT_EQ(scenario.top_row().source, AutocompleteSuggestionSource::LiteralURL);
    EXPECT(scenario.top_row_is_highlighted());
    EXPECT(!scenario.return_searches());
    EXPECT(!scenario.icon_is_search());

    auto const& rows = scenario.harness.omnibox.suggestions();
    EXPECT_EQ(rows.size(), 2u);
    if (rows.size() >= 2) {
        EXPECT_EQ(rows[1].source, AutocompleteSuggestionSource::Search);
        EXPECT_EQ(rows[1].text, "setup.py"sv);
    }

    // Choosing the search searches.
    scenario.harness.omnibox.select_next_suggestion();
    EXPECT(scenario.return_searches());
    EXPECT(scenario.icon_is_search());

    // The search goes to the search engine, though its text reads as an address.
    auto const& search_engine = scenario.profile.search_engine;
    auto search_url = WebView::url_for_omnibox_destination("setup.py"sv, WebView::OmniboxDestinationKind::Search, search_engine, WebView::AppendTLD::No);
    EXPECT(search_url.has_value() && search_url->serialize() == search_engine->format_search_query_for_navigation("setup.py"sv));
    auto address_url = WebView::url_for_omnibox_destination("setup.py"sv, WebView::OmniboxDestinationKind::URL, search_engine, WebView::AppendTLD::No);
    EXPECT(address_url.has_value() && address_url->serialize() == "https://setup.py/"sv);
}

TEST_CASE(scenario_a_typed_address_and_the_same_remote_phrase_offer_one_search)
{
    Scenario scenario({});
    scenario.type("setup.py"sv, { "setup.py"_string });

    auto const& rows = scenario.harness.omnibox.suggestions();
    EXPECT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows.first().source, AutocompleteSuggestionSource::LiteralURL);
}

TEST_CASE(scenario_typed_text_with_a_scheme_does_not_offer_a_search)
{
    Scenario scenario({});
    scenario.type("https://setup.py"sv);

    EXPECT_EQ(scenario.top_row().source, AutocompleteSuggestionSource::LiteralURL);
    EXPECT(!scenario.harness.omnibox.suggestions().contains([](auto const& row) { return row.source == AutocompleteSuggestionSource::Search; }));
}
