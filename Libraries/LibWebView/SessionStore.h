/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibDatabase/Forward.h>
#include <LibWebView/Export.h>

namespace WebView {

struct WEBVIEW_API SessionTab {
    String url;
};

struct WEBVIEW_API SessionWindow {
    i64 id { -1 };
    Vector<SessionTab> tabs;
    size_t active_tab_index { 0 };
    Optional<int> x;
    Optional<int> y;
    Optional<int> width;
    Optional<int> height;
    bool maximized { false };
};

/*
 * Session Restore Behavior
 * ===========================================================================================
 * Context \ Action   | Click Window 'X' (One by One) | Cmd + Q / Menu Quit | Crash / SIGKILL
 * -------------------+-------------------------------+---------------------+-----------------
 * Single Window      | Save as "Last Extinct Window" | Save current window | Trigger crash
 * (Only 1 remaining) | Restore that single window.   | Restore it on next  | recovery flow
 *                    |                               | boot.               | on next boot.
 * -------------------+-------------------------------+---------------------+-----------------
 * Multiple Windows   | Move closed window to         | Save ALL active     | Trigger crash
 * (e.g. A, B, C)     | RecentlyClosed queue. Next    | windows. Next boot  | recovery flow
 *                    | boot ONLY restores C.         | restores A, B, and C| restores ALL.
 * ===========================================================================================
 */
class WEBVIEW_API SessionStore {
    AK_MAKE_NONCOPYABLE(SessionStore);
    AK_MAKE_NONMOVABLE(SessionStore);

public:
    static ErrorOr<NonnullOwnPtr<SessionStore>> create(Database::Database&);

    ~SessionStore();

    // On start up
    Vector<SessionWindow> load_session();

    i64 insert_window(SessionWindow const&);
    void update_window(i64 window_id, SessionWindow const&);
    void delete_window(i64 window_id);

    void insert_tab(i64 window_id, size_t tab_index, String const& url);
    void update_tab_url(i64 window_id, size_t tab_index, String const& url);
    void delete_tab(i64 window_id, size_t tab_index);
    void set_active_tab(i64 window_id, size_t active_tab_index);

    void clear();

    // FIXME: Maybe we should move "Recently Closed" tracking from HistoryStore into SessionStore
private:
    struct Statements {
        Database::StatementID clear_windows { 0 };
        Database::StatementID clear_tabs { 0 };
        Database::StatementID insert_window { 0 };
        Database::StatementID update_window { 0 };
        Database::StatementID delete_window { 0 };
        Database::StatementID insert_tab { 0 };
        Database::StatementID update_tab_url { 0 };
        Database::StatementID delete_tab { 0 };
        Database::StatementID move_tabs_down { 0 };
        Database::StatementID set_active_tab { 0 };
        Database::StatementID get_windows { 0 };
        Database::StatementID get_tabs_for_window { 0 };
        Database::StatementID count_windows { 0 };
        Database::StatementID last_insert_rowid { 0 };
    };

    SessionStore(Database::Database&, Statements&&);

    Database::Database& m_database;
    Statements m_statements;
};

}
