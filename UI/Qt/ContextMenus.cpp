/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ImageFormats/PNGWriter.h>
#include <LibURL/Parser.h>
#include <LibWebView/SearchEngine.h>
#include <LibWebView/Settings.h>
#include <LibWebView/URL.h>
#include <UI/Qt/Application.h>
#include <UI/Qt/ContextMenus.h>
#include <UI/Qt/HubConnection.h>
#include <UI/Qt/RemoteView.h>

namespace Ladybird {

using WebView::ContextMenuKind;
using WebView::ContextMenuMediaAction;
using WebView::ContextMenuRequest;

// NB: A shown menu can outlive its view, so its actions find the view again when the user picks them.
template<typename Callback>
static Function<void()> on_view(u64 view_id, Callback callback)
{
    return [view_id, callback = move(callback)]() mutable {
        if (auto* view = RemoteView::find(view_id))
            callback(*view);
    };
}

static void copy_text(ByteString const& text)
{
    if (!text.is_empty())
        Application::the().set_clipboard_text(MUST(String::from_byte_string(text)));
}

static NonnullRefPtr<Action> create_copy_url_action(StringView text, URL::URL const& url)
{
    return Action::create(text, ActionID::CopyURL, [url]() {
        copy_text(WebView::url_text_to_copy(url));
    });
}

// A URL from the context menu is the page's, a link's or an image's, so it's navigated to from the page, unless the
// user selected it as text.
static void add_open_in_new_tab_action(Menu& menu, u64 view_id, StringView text, URL::URL const& url, Optional<Web::HTML::PreparedNavigationDescriptor> const& navigation)
{
    menu.add_action(Action::create(text, ActionID::OpenInNewTab, on_view(view_id, [url, navigation](RemoteView& view) {
        if (navigation.has_value())
            view.open_navigation_in_new_tab(*navigation, Web::HTML::ActivateTab::No);
        else
            view.open_url_in_new_tab(url, Web::HTML::ActivateTab::No);
    })));
}

static void add_open_url_actions(Menu& menu, RemoteView& view, URL::URL const& url, Optional<Web::HTML::PreparedNavigationDescriptor> const& navigation)
{
    auto view_id = view.view_id();
    add_open_in_new_tab_action(menu, view_id, "Open Link in New Tab"sv, url, navigation);
    if (view.is_private() == WebView::IsPrivate::No) {
        menu.add_action(Action::create("Open Link in New Window"sv, ActionID::OpenInNewWindow, on_view(view_id, [url, navigation](RemoteView& view) {
            if (navigation.has_value())
                view.open_navigation_in_new_window(*navigation, WebView::IsPrivate::No);
            else
                view.open_url_in_new_window(url, WebView::IsPrivate::No);
        })));
    }
    if (Application::the().supports_private_browsing_windows()) {
        menu.add_action(Action::create("Open Link in Private Window"sv, ActionID::OpenInNewPrivateWindow, on_view(view_id, [url, navigation](RemoteView& view) {
            if (!navigation.has_value()) {
                view.open_url_in_new_window(url, WebView::IsPrivate::Yes);
                return;
            }
            // A private window still gets the page as the navigation's initiator, but never the page's URL as its
            // referrer.
            auto private_navigation = *navigation;
            private_navigation.referrer_policy = Web::ReferrerPolicy::ReferrerPolicy::NoReferrer;
            view.open_navigation_in_new_window(move(private_navigation), WebView::IsPrivate::Yes);
        })));
    }
}

static void add_link_actions(Menu& menu, RemoteView& view, Web::HTML::PreparedNavigationDescriptor const& navigation)
{
    auto url = navigation.url;
    auto copy_url_text = [&] {
        switch (WebView::url_type(url)) {
        case WebView::URLType::Email:
            return "Copy Email Address"sv;
        case WebView::URLType::Telephone:
            return "Copy Phone Number"sv;
        case WebView::URLType::Other:
            return "Copy Link"sv;
        }
        VERIFY_NOT_REACHED();
    }();

    add_open_url_actions(menu, view, url, navigation);
    menu.add_separator();
    menu.add_action(create_copy_url_action(copy_url_text, url));
    menu.add_action(Action::create("Download Linked File"sv, ActionID::DownloadLinkedFile, on_view(view.view_id(), [url](RemoteView& view) {
        view.download_url(url, RemoteView::PromptForPath::No);
    })));
    menu.add_action(Action::create("Save Link As…"sv, ActionID::DownloadLinkedFileAs, on_view(view.view_id(), [url](RemoteView& view) {
        view.download_url(url, RemoteView::PromptForPath::Yes);
    })));
}

// Text can be edited only in a field, and copied only where there is some.
static void add_text_edit_actions(Menu& menu, u64 view_id, bool is_editable, bool has_selected_text)
{
    if (is_editable) {
        menu.add_action(Action::create("Cut"sv, ActionID::CutSelection, on_view(view_id, [](RemoteView& view) {
            HubConnection::the().async_view_cut_selection(view.view_id());
        })));
    }
    if (is_editable || has_selected_text) {
        menu.add_action(Action::create("Copy"sv, ActionID::CopySelection, on_view(view_id, [](RemoteView& view) {
            view.selected_text()->when_resolved(copy_text);
        })));
    }
    if (is_editable) {
        menu.add_action(Action::create("Paste"sv, ActionID::Paste, on_view(view_id, [](RemoteView& view) {
            HubConnection::the().async_view_paste(view.view_id(), Web::Clipboard::PasteMode::AllMimeTypes);
        })));
        menu.add_action(Action::create("Paste and Match Style"sv, ActionID::PasteAndMatchStyle, on_view(view_id, [](RemoteView& view) {
            HubConnection::the().async_view_paste(view.view_id(), Web::Clipboard::PasteMode::PlainTextOnly);
        })));
        menu.add_action(Action::create("Select All"sv, ActionID::SelectAll, on_view(view_id, [](RemoteView& view) {
            HubConnection::the().async_view_select_all(view.view_id());
        })));
    }
}

static void add_look_up_action(Menu& menu, u64 view_id, ContextMenuRequest const& request)
{
    if (!request.dictionary_lookup.has_value())
        return;

    menu.add_action(Action::create("Look Up"sv, ActionID::LookUpSelectedText, on_view(view_id, [lookup = *request.dictionary_lookup, position = request.dictionary_lookup_position](RemoteView& view) {
        if (view.on_request_dictionary_lookup)
            view.on_request_dictionary_lookup(lookup, position);
    })));
}

static void add_search_action(Menu& menu, u64 view_id, Optional<String> const& selected_text)
{
    auto const& search_engine = Application::settings().search_engine_settings().engine;
    if (!search_engine.has_value() || !selected_text.has_value())
        return;

    auto url = URL::Parser::basic_parse(search_engine->format_search_query_for_navigation(*selected_text));
    VERIFY(url.has_value());

    menu.add_action(Action::create(search_engine->format_search_query_for_display(*selected_text), ActionID::SearchSelectedText, on_view(view_id, [url = url.release_value()](RemoteView& view) {
        view.open_url_in_new_tab(url, Web::HTML::ActivateTab::Yes);
    })));
}

static void add_spelling_actions(Menu& menu, u64 view_id, ContextMenuRequest const& request)
{
    if (!request.misspelled_word.has_value())
        return;

    for (auto const& guess : request.spelling_guesses) {
        menu.add_action(Action::create(MUST(guess.utf16_view().to_utf8()), ActionID::SpellingGuess, [view_id, guess]() {
            HubConnection::the().async_view_replace_context_menu_misspelling(view_id, guess);
        }));
    }
    if (request.spelling_guesses.is_empty()) {
        auto no_guesses_action = Action::create("No Guesses Found"sv, ActionID::NoSpellingGuesses, [] { });
        no_guesses_action->set_enabled(false);
        menu.add_action(move(no_guesses_action));
    }
    menu.add_separator();
    menu.add_action(Action::create("Ignore Spelling"sv, ActionID::IgnoreSpelling, [view_id, word = *request.misspelled_word]() {
        HubConnection::the().async_view_ignore_misspelling(view_id, word);
    }));
    menu.add_action(Action::create("Learn Spelling"sv, ActionID::LearnSpelling, [view_id, word = *request.misspelled_word]() {
        HubConnection::the().async_view_learn_misspelling(view_id, word);
    }));
}

static void add_copy_link_to_highlight_action(Menu& menu, RemoteView& view, ContextMenuRequest const& request)
{
    if (request.makes_link_to_highlight)
        menu.add_action(view.copy_link_to_highlight_action());
}

// The element is the one under the cursor when the context menu opened, so a page that changes while the menu is open
// still has the element the user clicked on.
static void add_inspect_element_action(Menu& menu, u64 view_id, ContextMenuRequest const& request)
{
    if (!request.can_inspect_element)
        return;
    menu.add_separator();
    menu.add_action(Action::create("Inspect Element"sv, ActionID::InspectElement, [view_id]() {
        HubConnection::the().async_view_inspect_context_menu_element(view_id);
    }));
}

static NonnullRefPtr<Menu> create_page_context_menu(RemoteView& view, ContextMenuRequest const& request)
{
    auto view_id = view.view_id();

    // The page's own items are left out where the menu is about the text.
    auto is_about_page = !request.is_editable && !request.selected_text.has_value();

    auto menu = Menu::create("Page Context Menu"sv);
    add_spelling_actions(*menu, view_id, request);
    menu->add_separator();
    add_look_up_action(*menu, view_id, request);
    if (is_about_page) {
        menu->add_action(view.navigate_back_action());
        menu->add_action(view.navigate_forward_action());
        menu->add_action(Action::create("Reload"sv, ActionID::Reload, on_view(view_id, [](RemoteView& view) { view.reload(); })));
    }
    menu->add_separator();
    add_text_edit_actions(*menu, view_id, request.is_editable, request.selected_text.has_value());
    menu->add_separator();
    add_search_action(*menu, view_id, request.selected_text);
    add_copy_link_to_highlight_action(*menu, view, request);
    menu->add_separator();
    if (is_about_page) {
        auto view_source_action = Action::create("View Page Source"sv, ActionID::ViewSource, [view_id] {
            HubConnection::the().async_view_get_source(view_id);
        });
        view_source_action->set_enabled(view.can_view_source());
        menu->add_action(move(view_source_action));
    }
    auto report_website_issue_action = Action::create("Report Website Issue…"sv, ActionID::ReportWebsiteIssue, on_view(view_id, [](RemoteView& view) {
        view.report_website_issue();
    }));
    report_website_issue_action->set_enabled(view.can_report_website_issue());
    menu->add_action(move(report_website_issue_action));
    menu->add_separator();
    if (is_about_page) {
        menu->add_action(Action::create("Take Screenshot..."sv, ActionID::TakeScreenshot, on_view(view_id, [](RemoteView& view) {
            if (view.on_request_screenshot)
                view.on_request_screenshot();
        })));
    }
    add_inspect_element_action(*menu, view_id, request);
    return menu;
}

static NonnullRefPtr<Menu> create_selected_text_link_context_menu(RemoteView& view, ContextMenuRequest const& request, URL::URL const& url)
{
    auto view_id = view.view_id();

    auto menu = Menu::create("Selected Text Link Context Menu"sv);
    add_look_up_action(*menu, view_id, request);
    add_open_url_actions(*menu, view, url, {});
    menu->add_separator();
    add_text_edit_actions(*menu, view_id, request.is_editable, true);
    menu->add_separator();
    add_search_action(*menu, view_id, request.selected_text);
    add_copy_link_to_highlight_action(*menu, view, request);
    add_inspect_element_action(*menu, view_id, request);
    return menu;
}

static NonnullRefPtr<Menu> create_link_context_menu(RemoteView& view, ContextMenuRequest const& request)
{
    auto menu = Menu::create("Link Context Menu"sv);
    add_link_actions(*menu, view, *request.link_navigation);
    add_copy_link_to_highlight_action(*menu, view, request);
    add_inspect_element_action(*menu, view.view_id(), request);
    return menu;
}

static NonnullRefPtr<Menu> create_image_context_menu(RemoteView& view, ContextMenuRequest const& request)
{
    auto view_id = view.view_id();
    auto const& navigation = *request.navigation;
    auto url = navigation.url;

    auto copy_image_action = Action::create("Copy Image"sv, ActionID::CopyImage, [bitmap = request.image_bitmap]() {
        if (!bitmap.has_value() || !bitmap->is_valid())
            return;

        auto encoded = Gfx::PNGWriter::encode(*bitmap->bitmap());
        if (encoded.is_error())
            return;

        Application::the().insert_clipboard_item({ { { "image/png"_string, ByteString { encoded.value().bytes() } } } });
    });
    copy_image_action->set_enabled(request.image_bitmap.has_value());

    auto menu = Menu::create("Image Context Menu"sv);
    if (request.link_navigation.has_value()) {
        add_link_actions(*menu, view, *request.link_navigation);
        menu->add_separator();
    }
    add_open_in_new_tab_action(*menu, view_id, "Open Image in New Tab"sv, url, navigation);
    menu->add_action(Action::create("Open Image"sv, ActionID::OpenImage, on_view(view_id, [navigation](RemoteView& view) {
        view.load(navigation);
    })));
    menu->add_separator();
    menu->add_action(move(copy_image_action));
    menu->add_action(create_copy_url_action("Copy Image Address"sv, url));
    menu->add_action(Action::create("Save Image As…"sv, ActionID::SaveImage, on_view(view_id, [url, image_mime_type = request.image_mime_type](RemoteView& view) {
        view.download_url(url, RemoteView::PromptForPath::Yes, image_mime_type);
    })));
    add_inspect_element_action(*menu, view_id, request);
    return menu;
}

static NonnullRefPtr<Menu> create_media_context_menu(RemoteView& view, ContextMenuRequest const& request)
{
    auto view_id = view.view_id();
    auto const& media = *request.media;

    auto send = [view_id](ContextMenuMediaAction action) {
        return [view_id, action]() {
            HubConnection::the().async_view_do_context_menu_media_action(view_id, action);
        };
    };

    auto menu = Menu::create("Media Context Menu"sv);
    if (media.is_playing)
        menu->add_action(Action::create("Pause"sv, ActionID::PauseMedia, send(ContextMenuMediaAction::TogglePlay)));
    else
        menu->add_action(Action::create("Play"sv, ActionID::PlayMedia, send(ContextMenuMediaAction::TogglePlay)));
    if (media.is_muted)
        menu->add_action(Action::create("Unmute"sv, ActionID::UnmuteMedia, send(ContextMenuMediaAction::ToggleMute)));
    else
        menu->add_action(Action::create("Mute"sv, ActionID::MuteMedia, send(ContextMenuMediaAction::ToggleMute)));
    if (media.has_user_agent_controls)
        menu->add_action(Action::create("Hide Controls"sv, ActionID::HideControls, send(ContextMenuMediaAction::ToggleControls)));
    else
        menu->add_action(Action::create("Show Controls"sv, ActionID::ShowControls, send(ContextMenuMediaAction::ToggleControls)));
    auto loop_action = Action::create_checkable("Loop"sv, ActionID::ToggleMediaLoopState, send(ContextMenuMediaAction::ToggleLoop));
    loop_action->set_checked(media.is_looping);
    menu->add_action(move(loop_action));
    menu->add_separator();
    if (media.is_picture_in_picture)
        menu->add_action(Action::create("Exit Picture in Picture"sv, ActionID::ExitPictureInPicture, send(ContextMenuMediaAction::TogglePictureInPicture)));
    else if (media.can_enter_picture_in_picture)
        menu->add_action(Action::create("Picture in Picture"sv, ActionID::EnterPictureInPicture, send(ContextMenuMediaAction::TogglePictureInPicture)));
    if (media.is_video && media.is_fullscreen)
        menu->add_action(Action::create("Exit Full Screen"sv, ActionID::ExitFullscreen, send(ContextMenuMediaAction::ToggleFullscreen)));
    else if (media.is_video)
        menu->add_action(Action::create("Full Screen"sv, ActionID::EnterFullscreen, send(ContextMenuMediaAction::ToggleFullscreen)));
    menu->add_separator();
    if (media.media_url.has_value()) {
        auto url = *media.media_url;
        add_open_in_new_tab_action(*menu, view_id, media.is_video ? "Open Video in New Tab"sv : "Open Audio in New Tab"sv, url, request.navigation);
        if (request.navigation.has_value()) {
            menu->add_action(Action::create(media.is_video ? "Open Video"sv : "Open Audio"sv, media.is_video ? ActionID::OpenVideo : ActionID::OpenAudio, on_view(view_id, [navigation = *request.navigation](RemoteView& view) {
                view.load(navigation);
            })));
        }
        menu->add_action(create_copy_url_action(media.is_video ? "Copy Video Address"sv : "Copy Audio Address"sv, url));
    }
    add_inspect_element_action(*menu, view_id, request);
    return menu;
}

NonnullRefPtr<Menu> create_context_menu(RemoteView& view, ContextMenuRequest const& request)
{
    switch (request.kind) {
    case ContextMenuKind::Page:
        if (request.selected_text.has_value()) {
            if (auto url = WebView::url_from_text(*request.selected_text); url.has_value())
                return create_selected_text_link_context_menu(view, request, *url);
        }
        return create_page_context_menu(view, request);
    case ContextMenuKind::Link:
        if (request.link_navigation.has_value())
            return create_link_context_menu(view, request);
        break;
    case ContextMenuKind::Image:
        if (request.navigation.has_value())
            return create_image_context_menu(view, request);
        break;
    case ContextMenuKind::Media:
        if (request.media.has_value())
            return create_media_context_menu(view, request);
        break;
    }
    return create_page_context_menu(view, request);
}

}
