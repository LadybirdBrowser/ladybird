/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
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

    OpenAboutPage,
    OpenProcessesPage,
    OpenSettingsPage,
    ToggleDevTools,
    ViewSource,

    OpenInNewTab,
    CopyURL,

    OpenImage,
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
    ClearCache,
    ClearCookies,
    SpoofUserAgent,
    NavigatorCompatibilityMode,
    EnableScripting,
    EnableContentFiltering,
    BlockPopUps,
};

class WEBVIEW_API Action
    : public RefCounted<Action>
    , public Weakable<Action> {
public:
    static NonnullRefPtr<Action> create(Variant<StringView, String> text, ActionID id, Function<void()> action);
    static NonnullRefPtr<Action> create_checkable(Variant<StringView, String> text, ActionID id, Function<void()> action);

    void activate() { m_action(); }

    StringView text() const
    {
        return m_text.visit([](auto const& text) -> StringView { return text; });
    }
    void set_text(Variant<StringView, String>);

    StringView tooltip() const { return *m_tooltip; }
    void set_tooltip(StringView);

    ActionID id() const { return m_id; }

    bool enabled() const { return m_enabled; }
    void set_enabled(bool);

    bool visible() const { return m_visible; }
    void set_visible(bool);

    bool is_checkable() const { return m_checked.has_value(); }
    bool checked() const { return *m_checked; }
    void set_checked(bool);

    struct Observer {
        virtual ~Observer() = default;

        virtual void on_text_changed(Action&) { }
        virtual void on_tooltip_changed(Action&) { }
        virtual void on_enabled_state_changed(Action&) { }
        virtual void on_visible_state_changed(Action&) { }
        virtual void on_checked_state_changed(Action&) { }
    };

    void add_observer(NonnullOwnPtr<Observer>);
    void remove_observer(Observer const& observer);

    void set_group(Badge<Menu>, Menu& group) { m_group = group; }

private:
    Action(Variant<StringView, String> text, ActionID id, Function<void()> action)
        : m_text(move(text))
        , m_id(id)
        , m_action(move(action))
    {
    }

    void set_checked_internal(bool checked);

    Variant<StringView, String> m_text;
    Optional<StringView> m_tooltip;
    ActionID m_id;

    bool m_enabled { true };
    bool m_visible { true };
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

    static NonnullRefPtr<Menu> create(StringView name);
    static NonnullRefPtr<Menu> create_group(StringView name);

    void add_action(NonnullRefPtr<Action> action);
    void add_submenu(NonnullRefPtr<Menu> submenu) { m_items.append(move(submenu)); }
    void add_separator() { m_items.append(Separator {}); }

    StringView title() const { return m_title; }

    Span<MenuItem> items() { return m_items; }
    ReadonlySpan<MenuItem> items() const { return m_items; }

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
    explicit Menu(StringView title)
        : m_title(title)
    {
    }

    StringView m_title;
    Vector<MenuItem> m_items;

    bool m_is_group { false };
};

}
