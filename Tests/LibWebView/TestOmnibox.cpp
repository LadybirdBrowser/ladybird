/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWebView/Omnibox.h>
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
        .favicon_base64_png = {},
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
        .favicon_base64_png = {},
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
        on_suggestions(query_ids.last(), move(suggestions));
    }

    void deliver(WebView::AutocompleteQueryID query_id, Vector<AutocompleteSuggestion> suggestions)
    {
        on_suggestions(query_id, move(suggestions));
    }

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
        };
        omnibox.on_commit = [this](String text) {
            commits.append(move(text));
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
    }

    static NonnullOwnPtr<ScriptedProvider> adopt_provider(Harness& self)
    {
        auto scripted_provider = make<ScriptedProvider>();
        self.provider = scripted_provider.ptr();
        return scripted_provider;
    }

    void begin_editing(StringView text = ""sv)
    {
        display_text = MUST(String::from_utf8(text));
        selection_start = {};
        omnibox.begin_editing(display_text);
    }

    void press_key(char code_point)
    {
        auto text_before_selection = selected_text_range_prefix();
        StringBuilder builder;
        builder.append(text_before_selection);
        builder.append(code_point);
        display_text = MUST(builder.to_string());
        selection_start = {};
        omnibox.text_edited(display_text, true);
    }

    void press_backspace()
    {
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

    String display_text;
    Optional<size_t> selection_start;
    Vector<String> commits;

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
        "about:bookmarks"sv,
        "about:downloads"sv,
        "about:history"sv,
        "about:newtab"sv,
        "about:processes"sv,
        "about:settings"sv,
        "about:version"sv,
    };

    auto suggestions = WebView::web_ui_autocomplete_suggestions("about:"sv);

    EXPECT_EQ(WebView::WebUI::pages().size(), expected_urls.size());
    EXPECT_EQ(suggestions.size(), expected_urls.size());
    for (size_t index = 0; index < expected_urls.size(); ++index) {
        EXPECT_EQ(suggestions[index].source, WebView::AutocompleteSuggestionSource::WebUI);
        EXPECT_EQ(suggestions[index].text, expected_urls[index]);
    }
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
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) });
    EXPECT_EQ(harness.display_text, "t"sv);

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

TEST_CASE(enter_with_stale_results_commits_the_current_input_immediately)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.thee.example/"sv), search_row("t"sv) });
    harness.press_key('h');
    EXPECT_EQ(harness.display_text, "thee.example"sv);

    // Enter with a stale popup never waits for a provider response that may arrive after the user's
    // intent has changed.
    harness.omnibox.return_pressed();
    EXPECT_EQ(harness.commits.size(), 1u);
    EXPECT_EQ(harness.commits.last(), "th"sv);

    // Later deliveries may update the popup if editing is still active, but cannot trigger a second
    // navigation.
    harness.provider->deliver({ search_row("th"sv) });
    harness.provider->deliver({ history_row("https://www.thee.example/"sv), search_row("th"sv) });
    EXPECT_EQ(harness.commits.size(), 1u);
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
    EXPECT_EQ(harness.omnibox.selected_suggestion(), 1u);

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
    EXPECT_EQ(harness.omnibox.selected_suggestion(), 1u);
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
    EXPECT_EQ(harness.omnibox.selected_suggestion(), 0u);
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
    EXPECT_EQ(harness.omnibox.selected_suggestion(), 1u);

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
    EXPECT_EQ(harness.commits.last(), "th"sv);

    // Provider updates may continue until the chrome's commit handler ends editing, but no response is
    // armed to perform another navigation.
    harness.provider->deliver({});
    harness.provider->deliver({ history_row("https://www.thee.example/"sv), search_row("th"sv) });
    EXPECT_EQ(harness.commits.size(), 1u);
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

TEST_CASE(committing_may_reentrantly_end_editing)
{
    Harness harness;
    harness.begin_editing();

    // The real chrome clears focus while handling a commit, which calls back into end_editing.
    harness.omnibox.on_commit = [&](String text) {
        harness.commits.append(move(text));
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
