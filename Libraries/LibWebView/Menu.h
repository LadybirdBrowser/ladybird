/*
 * Copyright (c) 2025-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <AK/Weakable.h>
#include <LibGfx/Point.h>
#include <LibWebView/Forward.h>

namespace WebView {

enum class ActionID {
    NavigateBack,
    NavigateForward,
    Reload,

    CopySelection,
    Paste,
    SelectAll,

    SearchSelectedText,

    TakeVisibleScreenshot,
    TakeFullScreenshot,

    ToggleBookmark,
    ToggleBookmarkViaToolbar,
    ToggleBookmarksBar,
    BookmarkItem,

    OpenAboutPage,
    OpenProcessesPage,
    OpenSettingsPage,
    ToggleDevTools,
    ViewSource,

    OpenInNewTab,
    CopyURL,

    OpenImage,
    SaveImage,
    CopyImage,

    OpenAudio,
    OpenVideo,
    PlayMedia,
    PauseMedia,
    MuteMedia,
    UnmuteMedia,
    ShowControls,
    HideControls,
    ToggleMediaLoopState,
    EnterFullscreen,
    ExitFullscreen,

    ZoomIn,
    ZoomOut,
    ResetZoom,
    ResetZoomViaToolbar,

    PreferredColorScheme,
    PreferredContrast,
    PreferredMotion,

    DumpSessionHistoryTree,
    DumpDOMTree,
    DumpLayoutTree,
    DumpPaintTree,
    DumpStackingContextTree,
    DumpDisplayList,
    DumpStyleSheets,
    DumpStyles,
    DumpCSSErrors,
    DumpCookies,
    DumpLocalStorage,
    DumpGCGraph,
    ShowLineBoxBorders,
    CollectGarbage,
    SpoofUserAgent,
    NavigatorCompatibilityMode,
    EnableScripting,
    EnableContentFiltering,
    BlockPopUps,
};

using ActionText = Variant<StringView, String>;

inline StringView action_text_to_string_view(ActionText const& text)
{
    return text.visit([](auto const& text) -> StringView { return text; });
}

class WEBVIEW_API Action
    : public RefCounted<Action>
    , public Weakable<Action> {
public:
    static NonnullRefPtr<Action> create(ActionText text, ActionID id, Function<void()> action);
    static NonnullRefPtr<Action> create_checkable(ActionText text, ActionID id, Function<void()> action);

    void activate() { m_action(); }

    StringView text() const { return action_text_to_string_view(m_text); }
    void set_text(ActionText);

    StringView tooltip() const { return action_text_to_string_view(*m_tooltip); }
    void set_tooltip(ActionText);

    void set_base64_png_icon(Optional<String> base64_png_icon) { m_base64_png_icon = move(base64_png_icon); }
    Optional<String const&> base64_png_icon() const { return m_base64_png_icon; }

    ActionID id() const { return m_id; }

    bool enabled() const { return m_enabled; }
    void set_enabled(bool);

    bool visible() const { return m_visible; }
    void set_visible(bool);

    bool engaged() const { return m_engaged; }
    void set_engaged(bool);

    bool is_checkable() const { return m_checked.has_value(); }
    bool checked() const { return *m_checked; }
    void set_checked(bool);

    struct Observer {
        virtual ~Observer() = default;

        virtual void on_text_changed(Action&) { }
        virtual void on_tooltip_changed(Action&) { }
        virtual void on_enabled_state_changed(Action&) { }
        virtual void on_visible_state_changed(Action&) { }
        virtual void on_engaged_state_changed(Action&) { }
        virtual void on_checked_state_changed(Action&) { }
    };

    void add_observer(NonnullOwnPtr<Observer>);
    void remove_observer(Observer const& observer);

    void set_group(Badge<Menu>, Menu& group) { m_group = group; }

private:
    Action(ActionText text, ActionID id, Function<void()> action)
        : m_text(move(text))
        , m_id(id)
        , m_action(move(action))
    {
    }

    void set_checked_internal(bool checked);

    ActionText m_text;
    Optional<ActionText> m_tooltip;
    Optional<String> m_base64_png_icon;
    ActionID m_id;

    bool m_enabled { true };
    bool m_visible { true };
    bool m_engaged { false };
    Optional<bool> m_checked;

    Function<void()> m_action;
    Vector<NonnullOwnPtr<Observer>, 1> m_observers;

    WeakPtr<Menu> m_group;
};

struct WEBVIEW_API Separator { };

class WEBVIEW_API Menu
    : public RefCounted<Menu>
    , public Weakable<Menu> {
public:
    using MenuItem = Variant<NonnullRefPtr<Action>, NonnullRefPtr<Menu>, Separator>;

    static NonnullRefPtr<Menu> create(ActionText title);
    static NonnullRefPtr<Menu> create_group(ActionText title);

    void add_action(NonnullRefPtr<Action> action);
    void add_submenu(NonnullRefPtr<Menu> submenu) { m_items.append(move(submenu)); }
    void add_separator() { m_items.append(Separator {}); }

    size_t size() const { return m_items.size(); }
    void shrink(size_t size) { m_items.shrink(size); }

    StringView title() const { return action_text_to_string_view(m_title); }

    Span<MenuItem> items() { return m_items; }
    ReadonlySpan<MenuItem> items() const { return m_items; }

    void set_render_group_icon(bool render_group_icon) { m_render_group_icon = render_group_icon; }
    bool render_group_icon() const { return m_render_group_icon; }

    template<typename Callback>
    void for_each_action(Callback const& callback)
    {
        for (auto& item : m_items) {
            item.visit(
                [&](NonnullRefPtr<Action>& action) { callback(*action); },
                [&](NonnullRefPtr<Menu>& submenu) { submenu->for_each_action(callback); },
                [&](Separator) {});
        }
    }

    Function<void(Gfx::IntPoint)> on_activation;

private:
    explicit Menu(ActionText title)
        : m_title(move(title))
    {
    }

    ActionText m_title;
    Vector<MenuItem> m_items;

    bool m_is_group { false };
    bool m_render_group_icon { false };
};

}
