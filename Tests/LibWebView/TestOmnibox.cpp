/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWebView/Omnibox.h>

namespace {

using WebView::AutocompleteResultKind;
using WebView::AutocompleteSuggestion;
using WebView::AutocompleteSuggestionSection;
using WebView::AutocompleteSuggestionSource;
using WebView::Omnibox;

AutocompleteSuggestion row(AutocompleteSuggestionSource source, AutocompleteSuggestionSection section, StringView text)
{
    return { .source = source, .section = section, .text = MUST(String::from_utf8(text)), .title = {}, .subtitle = {}, .favicon_base64_png = {} };
}

AutocompleteSuggestion history_row(StringView url)
{
    return row(AutocompleteSuggestionSource::History, AutocompleteSuggestionSection::History, url);
}

AutocompleteSuggestion search_row(StringView query)
{
    return row(AutocompleteSuggestionSource::Search, AutocompleteSuggestionSection::SearchSuggestions, query);
}

AutocompleteSuggestion literal_row(StringView url)
{
    return row(AutocompleteSuggestionSource::LiteralURL, AutocompleteSuggestionSection::None, url);
}

class ScriptedProvider final : public WebView::OmniboxSuggestionProvider {
public:
    virtual void query(String query, size_t) override
    {
        queries.append(move(query));
    }

    virtual void cancel() override
    {
        ++cancel_count;
    }

    void deliver(Vector<AutocompleteSuggestion> suggestions, AutocompleteResultKind result_kind)
    {
        on_suggestions(move(suggestions), result_kind);
    }

    Vector<String> queries;
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

TEST_CASE(fast_typing_with_top_hit_flips_activates_the_visible_completion)
{
    Harness harness;
    harness.begin_editing();

    // "t" completes to the top history hit for "t".
    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.twin.example/"sv), search_row("t"sv) }, AutocompleteResultKind::Intermediate);
    EXPECT_EQ(harness.display_text, "twin.example"sv);
    EXPECT_EQ(harness.selection_start, 1u);
    EXPECT(harness.omnibox.is_popup_visible());

    // "th" no longer matches it; the completion breaks, and a different suggestion takes over.
    harness.press_key('h');
    EXPECT_EQ(harness.display_text, "th"sv);
    harness.provider->deliver({ history_row("https://www.thee.example/"sv), search_row("th"sv) }, AutocompleteResultKind::Intermediate);
    EXPECT_EQ(harness.display_text, "thee.example"sv);

    // The user keeps typing into and past that suggestion.
    harness.press_key('e');
    EXPECT_EQ(harness.display_text, "thee.example"sv);
    EXPECT_EQ(harness.selection_start, 3u);
    harness.provider->deliver({ history_row("https://www.thee.example/"sv), search_row("the"sv) }, AutocompleteResultKind::Intermediate);

    harness.press_key('v');
    EXPECT_EQ(harness.display_text, "thev"sv);
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("thev"sv) }, AutocompleteResultKind::Intermediate);
    EXPECT_EQ(harness.display_text, "thev.example"sv);
    EXPECT_EQ(harness.selection_start, 4u);

    // The popup still shows rows for "t" (intermediate updates do not repaint it), so Enter re-queries and
    // must activate the completed suggestion, not search for the typed prefix.
    EXPECT_EQ(harness.omnibox.suggestions().first().text, "https://www.twin.example/"sv);
    harness.omnibox.return_pressed();
    EXPECT(harness.commits.is_empty());
    EXPECT_EQ(harness.provider->queries.last(), "thev"sv);
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("thev"sv) }, AutocompleteResultKind::Intermediate);

    EXPECT_EQ(harness.commits.size(), 1u);
    EXPECT_EQ(harness.commits.last(), "https://www.thev.example/"sv);
}

TEST_CASE(editing_the_completion_submits_the_typed_text)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) }, AutocompleteResultKind::Intermediate);
    EXPECT_EQ(harness.display_text, "thev.example"sv);

    // Typing something that breaks the completion means Enter takes the text literally.
    harness.press_key('x');
    EXPECT_EQ(harness.display_text, "tx"sv);
    harness.provider->deliver({ search_row("tx"sv) }, AutocompleteResultKind::Intermediate);
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
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) }, AutocompleteResultKind::Intermediate);
    EXPECT_EQ(harness.display_text, "thev.example"sv);

    // Backspace deletes the selected completion; the suggestion must not come back for the same query.
    harness.press_backspace();
    EXPECT_EQ(harness.display_text, "t"sv);
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) }, AutocompleteResultKind::Intermediate);
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
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) }, AutocompleteResultKind::Intermediate);
    harness.press_backspace();
    EXPECT_EQ(harness.display_text, "t"sv);

    // A new query means the user is typing again; completion resumes and Enter activates it.
    harness.press_key('h');
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("th"sv) }, AutocompleteResultKind::Intermediate);
    EXPECT_EQ(harness.display_text, "thev.example"sv);
    EXPECT_EQ(harness.selection_start, 2u);

    harness.omnibox.return_pressed();
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("th"sv) }, AutocompleteResultKind::Intermediate);
    EXPECT_EQ(harness.commits.size(), 1u);
    EXPECT_EQ(harness.commits.last(), "https://www.thev.example/"sv);
}

TEST_CASE(intermediate_results_do_not_repaint_a_visible_popup)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.twin.example/"sv), search_row("t"sv) }, AutocompleteResultKind::Intermediate);
    EXPECT_EQ(harness.omnibox.suggestions().size(), 2u);
    EXPECT_EQ(harness.omnibox.suggestions().first().text, "https://www.twin.example/"sv);

    harness.press_key('h');
    harness.provider->deliver({ history_row("https://www.thee.example/"sv), search_row("th"sv) }, AutocompleteResultKind::Intermediate);
    EXPECT_EQ(harness.omnibox.suggestions().first().text, "https://www.twin.example/"sv);

    harness.provider->deliver({ history_row("https://www.thee.example/"sv), search_row("th"sv), search_row("th zzz"sv) }, AutocompleteResultKind::Final);
    EXPECT_EQ(harness.omnibox.suggestions().size(), 3u);
    EXPECT_EQ(harness.omnibox.suggestions().first().text, "https://www.thee.example/"sv);

    // The popup is now fresh, so Enter activates the selected row without another engine round-trip.
    auto queries_before_return = harness.provider->queries.size();
    harness.omnibox.return_pressed();
    EXPECT_EQ(harness.provider->queries.size(), queries_before_return);
    EXPECT_EQ(harness.commits.size(), 1u);
    EXPECT_EQ(harness.commits.last(), "https://www.thee.example/"sv);
}

TEST_CASE(pending_activation_waits_for_a_result_it_can_act_on)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.thee.example/"sv), search_row("t"sv) }, AutocompleteResultKind::Intermediate);
    harness.press_key('h');
    EXPECT_EQ(harness.display_text, "thee.example"sv);

    // Enter with a stale popup: nothing to activate yet.
    harness.omnibox.return_pressed();
    EXPECT(harness.commits.is_empty());

    // An intermediate whose selected row merely equals the query is not worth activating; the final
    // result may still bring a better row.
    harness.provider->deliver({ search_row("th"sv) }, AutocompleteResultKind::Intermediate);
    EXPECT(harness.commits.is_empty());

    harness.provider->deliver({ history_row("https://www.thee.example/"sv), search_row("th"sv) }, AutocompleteResultKind::Final);
    EXPECT_EQ(harness.commits.size(), 1u);
    EXPECT_EQ(harness.commits.last(), "https://www.thee.example/"sv);
}

TEST_CASE(escape_keeps_an_automatic_completion_but_closes_the_popup)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) }, AutocompleteResultKind::Intermediate);
    EXPECT_EQ(harness.display_text, "thev.example"sv);

    auto cancels_before_escape = harness.provider->cancel_count;
    EXPECT_EQ(harness.omnibox.escape_pressed(), Omnibox::EscapeAction::ClosedPopup);
    EXPECT(!harness.omnibox.is_popup_visible());
    EXPECT_EQ(harness.display_text, "thev.example"sv);
    EXPECT_EQ(harness.provider->cancel_count, cancels_before_escape + 1);

    // With the popup gone, Enter submits what is on display.
    harness.omnibox.return_pressed();
    EXPECT_EQ(harness.commits.last(), "thev.example"sv);

    EXPECT_EQ(harness.omnibox.escape_pressed(), Omnibox::EscapeAction::EndEditing);
}

TEST_CASE(escape_restores_the_query_over_a_preview)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), history_row("https://odd.example/t"sv), search_row("t"sv) }, AutocompleteResultKind::Intermediate);

    // Hovering a row that cannot extend the query previews it in full.
    harness.omnibox.suggestion_hovered(1);
    EXPECT_EQ(harness.display_text, "https://odd.example/t"sv);
    EXPECT_EQ(harness.selection_start, 0u);

    EXPECT_EQ(harness.omnibox.escape_pressed(), Omnibox::EscapeAction::ClosedPopup);
    EXPECT_EQ(harness.display_text, "t"sv);
}

TEST_CASE(arrow_keys_walk_the_suggestions_and_enter_activates_the_choice)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) }, AutocompleteResultKind::Final);
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

TEST_CASE(a_user_chosen_row_wins_over_edited_text)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) }, AutocompleteResultKind::Final);

    // Break the completion so Enter would take the text literally...
    harness.press_key('x');
    harness.provider->deliver({ history_row("https://www.txt.example/"sv), search_row("tx"sv) }, AutocompleteResultKind::Final);

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
    harness.provider->deliver({ literal_row("t.example/path"sv), history_row("https://www.thev.example/"sv) }, AutocompleteResultKind::Final);
    EXPECT_EQ(harness.display_text, "t"sv);
    EXPECT_EQ(harness.omnibox.selected_suggestion(), 0u);

    harness.omnibox.return_pressed();
    EXPECT_EQ(harness.commits.last(), "t.example/path"sv);
}

TEST_CASE(a_top_row_that_does_not_match_the_prefix_is_only_highlighted)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://odd.example/t"sv), search_row("t"sv) }, AutocompleteResultKind::Final);
    EXPECT_EQ(harness.display_text, "t"sv);
    EXPECT_EQ(harness.omnibox.selected_suggestion(), 0u);

    // The top row is the default action even without a completion.
    harness.omnibox.return_pressed();
    EXPECT_EQ(harness.commits.last(), "https://odd.example/t"sv);
}

TEST_CASE(a_fresh_non_prefix_top_row_wins_after_editing_a_completion)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) }, AutocompleteResultKind::Final);
    EXPECT_EQ(harness.display_text, "thev.example"sv);

    // The user types over the completion, and the fresh top hit is selected because it matched by title,
    // not because its URL can inline-complete the typed query.
    for (auto code_point : "itle match"sv)
        harness.press_key(code_point);
    harness.provider->deliver({ history_row("https://news.ycombinator.com/"sv), search_row("title match"sv) }, AutocompleteResultKind::Final);
    EXPECT_EQ(harness.display_text, "title match"sv);
    EXPECT_EQ(harness.omnibox.selected_suggestion(), 0u);

    harness.omnibox.return_pressed();
    EXPECT_EQ(harness.commits.size(), 1u);
    EXPECT_EQ(harness.commits.last(), "https://news.ycombinator.com/"sv);
}

TEST_CASE(clicking_a_suggestion_commits_it)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) }, AutocompleteResultKind::Final);

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

    harness.provider->deliver({ history_row("https://www.thev.example/"sv) }, AutocompleteResultKind::Final);
    EXPECT(!harness.omnibox.is_popup_visible());
    EXPECT(harness.commits.is_empty());
}

TEST_CASE(showing_all_suggestions_does_not_complete_over_the_address)
{
    Harness harness;
    harness.begin_editing("https://site.example/page"sv);
    harness.omnibox.show_all_suggestions();
    EXPECT_EQ(harness.provider->queries.last(), "https://site.example/page"sv);

    harness.provider->deliver({ history_row("https://site.example/page"sv), history_row("https://site.example/page2"sv) }, AutocompleteResultKind::Final);
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
    harness.provider->deliver({ history_row("https://www.thev.example/"sv) }, AutocompleteResultKind::Intermediate);
    EXPECT_EQ(harness.display_text, "thev.example"sv);

    harness.provider->deliver({}, AutocompleteResultKind::Final);
    EXPECT(!harness.omnibox.is_popup_visible());
    EXPECT_EQ(harness.display_text, "t"sv);
}

TEST_CASE(no_completion_is_applied_when_the_cursor_is_not_at_the_end)
{
    Harness harness;
    harness.begin_editing();

    harness.omnibox.text_edited("t"_string, false);
    harness.display_text = "t"_string;
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) }, AutocompleteResultKind::Final);
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
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) }, AutocompleteResultKind::Intermediate);
    EXPECT_EQ(harness.display_text, "thev.example"sv);

    // A click elsewhere in the window dismisses the popup without ending the editing session.
    harness.omnibox.popup_dismissed();
    EXPECT(!harness.omnibox.is_popup_visible());
    EXPECT_EQ(harness.display_text, "t"sv);
    EXPECT(harness.omnibox.is_editing());
}

TEST_CASE(escape_cancels_a_pending_activation)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.thee.example/"sv), search_row("t"sv) }, AutocompleteResultKind::Intermediate);
    harness.press_key('h');

    // Enter with stale rows arms a pending activation; Escape must disarm it.
    harness.omnibox.return_pressed();
    EXPECT_EQ(harness.omnibox.escape_pressed(), Omnibox::EscapeAction::ClosedPopup);

    // A later delivery for the same query (for example after the focus shortcut re-queries it) must not
    // fire the navigation the user cancelled.
    harness.omnibox.show_all_suggestions();
    harness.provider->deliver({ history_row("https://www.thee.example/"sv), search_row("th"sv) }, AutocompleteResultKind::Final);
    EXPECT(harness.commits.is_empty());
}

TEST_CASE(a_vanished_preview_row_stops_being_displayed)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), history_row("https://odd.example/t"sv), search_row("t"sv) }, AutocompleteResultKind::Final);

    harness.omnibox.suggestion_hovered(1);
    EXPECT_EQ(harness.display_text, "https://odd.example/t"sv);

    // The previewed row is gone from the refreshed results; the bar must not keep showing text that
    // Enter would no longer act on.
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) }, AutocompleteResultKind::Final);
    EXPECT_EQ(harness.display_text, "t"sv);

    // With no selected row left, Enter submits exactly what the bar shows.
    harness.omnibox.return_pressed();
    EXPECT_EQ(harness.commits.last(), "t"sv);
}

TEST_CASE(a_surviving_preview_row_remains_the_users_choice)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), history_row("https://odd.example/t"sv), search_row("t"sv) }, AutocompleteResultKind::Final);

    // Break the completion so Enter would otherwise take the query literally, then preview a row by
    // arrowing away and back onto it.
    harness.press_key('x');
    harness.provider->deliver({ history_row("https://odd.example/tx"sv), search_row("tx"sv) }, AutocompleteResultKind::Final);
    EXPECT(harness.omnibox.select_next_suggestion());
    EXPECT(harness.omnibox.select_previous_suggestion());
    EXPECT_EQ(harness.display_text, "https://odd.example/tx"sv);

    // A refresh that still contains the previewed row keeps it selected as the user's choice.
    harness.provider->deliver({ history_row("https://odd.example/tx"sv), search_row("tx"sv) }, AutocompleteResultKind::Final);
    EXPECT_EQ(harness.omnibox.selected_suggestion(), 0u);

    harness.omnibox.return_pressed();
    EXPECT_EQ(harness.commits.last(), "https://odd.example/tx"sv);
}

TEST_CASE(showing_all_suggestions_adopts_an_active_completion)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) }, AutocompleteResultKind::Final);
    EXPECT_EQ(harness.display_text, "thev.example"sv);

    // The focus shortcut queries whatever the bar displays; results must not truncate the bar back to
    // the typed prefix.
    harness.omnibox.show_all_suggestions();
    EXPECT_EQ(harness.provider->queries.last(), "thev.example"sv);
    harness.provider->deliver({ history_row("https://www.thev.example/"sv) }, AutocompleteResultKind::Final);
    EXPECT_EQ(harness.display_text, "thev.example"sv);
}

TEST_CASE(arrow_keys_reopen_a_dismissed_popup)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) }, AutocompleteResultKind::Final);

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
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) }, AutocompleteResultKind::Final);
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
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) }, AutocompleteResultKind::Final);
    EXPECT_EQ(harness.display_text, "t"sv);
    EXPECT(!harness.omnibox.is_popup_visible());

    harness.omnibox.set_suspended(false);
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) }, AutocompleteResultKind::Final);
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
    harness.provider->deliver({ history_row("https://odd.example/"sv), search_row("od"sv) }, AutocompleteResultKind::Final);
    EXPECT_EQ(harness.display_text, "od"sv);
}

TEST_CASE(final_results_repaint_a_visible_popup)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.twin.example/"sv), search_row("t"sv) }, AutocompleteResultKind::Intermediate);
    EXPECT(harness.visible_popup);
    EXPECT_EQ(harness.visible_rows.first().text, "https://www.twin.example/"sv);

    // The final result set replaces the rows under an already-open popup; the chrome must be told.
    harness.provider->deliver({ history_row("https://www.twin.example/"sv), search_row("t"sv), search_row("t zzz"sv) }, AutocompleteResultKind::Final);
    EXPECT_EQ(harness.visible_rows.size(), 3u);
    EXPECT_EQ(harness.visible_rows.last().text, "t zzz"sv);
    EXPECT_EQ(harness.visible_selection, 0u);
}

TEST_CASE(a_user_highlighted_completion_survives_a_refresh)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) }, AutocompleteResultKind::Final);

    // Walk away and back so the highlight is a deliberate act.
    EXPECT(harness.omnibox.select_next_suggestion());
    EXPECT(harness.omnibox.select_previous_suggestion());
    EXPECT_EQ(harness.display_text, "thev.example"sv);

    // A refresh that keeps the chosen row selected keeps it the user's choice, so Escape restores the
    // typed text instead of preserving the completion.
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) }, AutocompleteResultKind::Final);
    EXPECT_EQ(harness.omnibox.escape_pressed(), Omnibox::EscapeAction::ClosedPopup);
    EXPECT_EQ(harness.display_text, "t"sv);
}

TEST_CASE(pending_activation_survives_an_empty_intermediate_result)
{
    Harness harness;
    harness.begin_editing();

    harness.press_key('t');
    harness.provider->deliver({ history_row("https://www.thee.example/"sv), search_row("t"sv) }, AutocompleteResultKind::Intermediate);
    harness.press_key('h');
    harness.omnibox.return_pressed();
    EXPECT(harness.commits.is_empty());

    // An empty intermediate result (no local matches, remote still pending) must not eat the activation
    // the final result may still satisfy.
    harness.provider->deliver({}, AutocompleteResultKind::Intermediate);
    EXPECT(harness.commits.is_empty());

    harness.provider->deliver({ history_row("https://www.thee.example/"sv), search_row("th"sv) }, AutocompleteResultKind::Final);
    EXPECT_EQ(harness.commits.size(), 1u);
    EXPECT_EQ(harness.commits.last(), "https://www.thee.example/"sv);
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
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) }, AutocompleteResultKind::Intermediate);
    EXPECT_EQ(harness.display_text, "thev.example"sv);

    // The user clicks into the middle of the completed text; a late delivery must leave it alone even
    // though a completion is currently on display.
    harness.omnibox.cursor_moved(false);
    harness.provider->deliver({ history_row("https://www.thevx.example/"sv), search_row("t"sv) }, AutocompleteResultKind::Final);
    EXPECT_EQ(harness.display_text, "thev.example"sv);
    EXPECT(!harness.omnibox.selected_suggestion().has_value());
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
    harness.provider->deliver({ history_row("https://www.thev.example/"sv), search_row("t"sv) }, AutocompleteResultKind::Final);

    harness.omnibox.return_pressed();
    EXPECT_EQ(harness.commits.size(), 1u);
    EXPECT_EQ(harness.commits.last(), "https://www.thev.example/"sv);
    EXPECT(!harness.omnibox.is_editing());
    EXPECT(!harness.omnibox.is_popup_visible());
}
