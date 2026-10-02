/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibWebView/Application.h>
#include <LibWebView/PictureInPictureManager.h>
#include <LibWebView/PictureInPictureWindow.h>
#include <LibWebView/WebContentClient.h>
#include <LibWebView/WebContentPage.h>

namespace WebView {

PictureInPictureManager& PictureInPictureManager::the()
{
    static auto& manager = *new PictureInPictureManager;
    return manager;
}

String PictureInPictureManager::open_window(WebContentPage& requesting_page, CanonicalTraversable& traversable, Gfx::IntSize video_size)
{
    // A page that already has a window replaces it itself, so only other pages are told that they lost theirs.
    if (auto index = index_of_window_for(requesting_page); index.has_value())
        retire_window(*index);

    // Test mode loads unrelated pages side by side, so there they do not take the window from one another.
    if (Application::web_content_options().is_test_mode == IsTestMode::No) {
        while (!m_windows.is_empty()) {
            auto page = m_windows.last().page;
            retire_window(m_windows.size() - 1);
            if (page->is_open())
                page->async_picture_in_picture_window_did_close();
        }
    }

    auto window = Application::the().create_picture_in_picture_window(requesting_page, traversable, video_size);
    if (!window)
        return {};

    window->on_resize = [this, window = window.ptr()](Gfx::IntSize size) {
        if (auto index = index_of(*window); index.has_value() && m_windows[*index].page->is_open())
            m_windows[*index].page->async_picture_in_picture_window_did_resize(size);
    };
    window->on_close = [this, window = window.ptr()] {
        window_did_close(*window);
    };
    window->on_page_close = [this, window = window.ptr()] {
        window_page_did_close(*window);
    };

    auto& opened_window = *window;
    m_windows.append({ requesting_page, window.release_nonnull() });
    requesting_page.async_did_open_picture_in_picture_window(opened_window.size());
    return opened_window.handle();
}

void PictureInPictureManager::close_window(WebContentPage& page)
{
    if (auto index = index_of_window_for(page); index.has_value())
        retire_window(*index);
}

Optional<size_t> PictureInPictureManager::index_of_window_for(WebContentPage const& page) const
{
    return m_windows.find_first_index_if([&](auto const& window) { return window.page.ptr() == &page; });
}

Optional<size_t> PictureInPictureManager::index_of(PictureInPictureWindow const& window) const
{
    return m_windows.find_first_index_if([&](auto const& entry) { return entry.window.ptr() == &window; });
}

// The page that a window shows outlives the window's place as another page's window, so a retired window stays hidden
// until the page that it shows has closed.
void PictureInPictureManager::retire_window(size_t index)
{
    auto window = m_windows.take(index).window;
    window->hide();
    m_retired_windows.append(move(window));
}

void PictureInPictureManager::window_did_close(PictureInPictureWindow& window)
{
    auto index = index_of(window);
    if (!index.has_value())
        return;
    auto page = m_windows[*index].page;
    retire_window(*index);

    if (page->is_open())
        page->async_picture_in_picture_window_did_close();
}

void PictureInPictureManager::window_page_did_close(PictureInPictureWindow& window)
{
    OwnPtr<PictureInPictureWindow> closed_window;
    if (auto index = index_of(window); index.has_value()) {
        closed_window = m_windows.take(*index).window;
    } else if (auto index = m_retired_windows.find_first_index_if([&](auto const& retired_window) { return retired_window.ptr() == &window; }); index.has_value()) {
        closed_window = m_retired_windows.take(*index);
    }

    // The window's page closes from within a callback of its view, so the window is destroyed once that returns.
    Core::deferred_invoke([closed_window = move(closed_window)] { });
}

}
