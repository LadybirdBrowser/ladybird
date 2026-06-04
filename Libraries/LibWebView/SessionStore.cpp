/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibDatabase/Database.h>
#include <LibWebView/SessionStore.h>

namespace WebView {

ErrorOr<NonnullOwnPtr<SessionStore>> SessionStore::create(Database::Database& database)
{
    Statements statements {};

    auto create_windows_table = TRY(database.prepare_statement(R"#(
        CREATE TABLE IF NOT EXISTS SessionWindows (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            active_tab_index INTEGER NOT NULL DEFAULT 0,
            x INTEGER,
            y INTEGER,
            width INTEGER,
            height INTEGER,
            maximized INTEGER NOT NULL DEFAULT 0
        );
    )#"sv));
    database.execute_statement(create_windows_table, {});

    auto create_tabs_table = TRY(database.prepare_statement(R"#(
        CREATE TABLE IF NOT EXISTS SessionTabs (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            window_id INTEGER NOT NULL REFERENCES SessionWindows(id) ON DELETE CASCADE,
            tab_index INTEGER NOT NULL,
            url TEXT NOT NULL
        );
    )#"sv));
    database.execute_statement(create_tabs_table, {});

    statements.clear_windows = TRY(database.prepare_statement("DELETE FROM SessionWindows;"sv));
    statements.clear_tabs = TRY(database.prepare_statement("DELETE FROM SessionTabs;"sv));
    statements.insert_window = TRY(database.prepare_statement(R"#(
        INSERT INTO SessionWindows (active_tab_index, x, y, width, height, maximized)
        VALUES (?, ?, ?, ?, ?, ?);
    )#"sv));
    statements.update_window = TRY(database.prepare_statement(R"#(
        UPDATE SessionWindows
        SET x = ?, y = ?, width = ?, height = ?, maximized = ?
        WHERE id = ?;
    )#"sv));
    statements.delete_window = TRY(database.prepare_statement("DELETE FROM SessionWindows WHERE id = ?;"sv));
    statements.insert_tab = TRY(database.prepare_statement(R"#(
        INSERT INTO SessionTabs (window_id, tab_index, url)
        VALUES (?, ?, ?);
    )#"sv));
    statements.update_tab_url = TRY(database.prepare_statement(R"#(
        UPDATE SessionTabs SET url = ? WHERE window_id = ? AND tab_index = ?;
    )#"sv));
    statements.delete_tab = TRY(database.prepare_statement(R"#(
        DELETE FROM SessionTabs WHERE window_id = ? AND tab_index = ?;
    )#"sv));
    statements.move_tabs_down = TRY(database.prepare_statement(R"#(
        UPDATE SessionTabs
        SET tab_index = tab_index - 1
        WHERE window_id = ? AND tab_index > ?;
    )#"sv));
    statements.set_active_tab = TRY(database.prepare_statement(R"#(
        UPDATE SessionWindows SET active_tab_index = ? WHERE id = ?;
    )#"sv));
    statements.get_windows = TRY(database.prepare_statement(R"#(
        SELECT id, active_tab_index, x, y, width, height, maximized
        FROM SessionWindows
        ORDER BY id ASC;
    )#"sv));
    statements.get_tabs_for_window = TRY(database.prepare_statement(R"#(
        SELECT url
        FROM SessionTabs
        WHERE window_id = ?
        ORDER BY tab_index ASC;
    )#"sv));
    statements.last_insert_rowid = TRY(database.prepare_statement("SELECT last_insert_rowid();"sv));

    return adopt_own(*new SessionStore { database, move(statements) });
}

SessionStore::SessionStore(Database::Database& database, Statements&& statements)
    : m_database(database)
    , m_statements(move(statements))
{
}

SessionStore::~SessionStore() = default;

Vector<SessionWindow> SessionStore::load_session()
{
    Vector<SessionWindow> windows;

    m_database.execute_statement(m_statements.get_windows, [&](Database::StatementID) {
        SessionWindow window;
        window.id = m_database.result_column<i64>(m_statements.get_windows, 0);
        window.active_tab_index = static_cast<size_t>(m_database.result_column<i64>(m_statements.get_windows, 1));

        auto x = m_database.result_column<i64>(m_statements.get_windows, 2);
        auto y = m_database.result_column<i64>(m_statements.get_windows, 3);
        auto w = m_database.result_column<i64>(m_statements.get_windows, 4);
        auto h = m_database.result_column<i64>(m_statements.get_windows, 5);
        auto maximized = m_database.result_column<i64>(m_statements.get_windows, 6);

        if (x >= 0)
            window.x = static_cast<int>(x);
        if (y >= 0)
            window.y = static_cast<int>(y);
        if (w >= 0)
            window.width = static_cast<int>(w);
        if (h >= 0)
            window.height = static_cast<int>(h);
        window.maximized = (maximized != 0);

        m_database.execute_statement(
            m_statements.get_tabs_for_window,
            [&](Database::StatementID) {
                auto url = m_database.result_column<String>(m_statements.get_tabs_for_window, 0);
                window.tabs.append(SessionTab { AK::move(url) });
            },
            window.id);

        windows.append(AK::move(window));
    });

    return windows;
}

i64 SessionStore::insert_window(SessionWindow const& window)
{
    int x = window.x.has_value() ? *window.x : -1;
    int y = window.y.has_value() ? *window.y : -1;
    int width = window.width.has_value() ? *window.width : -1;
    int height = window.height.has_value() ? *window.height : -1;
    int maximized = window.maximized ? 1 : 0;

    m_database.execute_statement(
        m_statements.insert_window,
        {},
        static_cast<i64>(window.active_tab_index),
        x, y, width, height, maximized);
    i64 new_id = -1;

    m_database.execute_statement(
        m_statements.last_insert_rowid,
        [&](Database::StatementID) {
            new_id = m_database.result_column<i64>(m_statements.last_insert_rowid, 0);
        });

    for (size_t tab_idx = 0; tab_idx < window.tabs.size(); ++tab_idx) {
        m_database.execute_statement(
            m_statements.insert_tab,
            {},
            new_id,
            static_cast<i64>(tab_idx),
            window.tabs[tab_idx].url);
    }

    return new_id;
}

void SessionStore::update_window(i64 window_id, SessionWindow const& window)
{
    int x = window.x.has_value() ? *window.x : -1;
    int y = window.y.has_value() ? *window.y : -1;
    int width = window.width.has_value() ? *window.width : -1;
    int height = window.height.has_value() ? *window.height : -1;
    int maximized = window.maximized ? 1 : 0;

    m_database.execute_statement(m_statements.update_window, {}, x, y, width, height, maximized, window_id);
}

void SessionStore::delete_window(i64 window_id)
{
    m_database.execute_statement(m_statements.delete_window, {}, window_id);
}

void SessionStore::insert_tab(i64 window_id, size_t tab_index, String const& url)
{
    m_database.execute_statement(m_statements.insert_tab, {}, window_id, static_cast<i64>(tab_index), url);
}

void SessionStore::update_tab_url(i64 window_id, size_t tab_index, String const& url)
{
    m_database.execute_statement(m_statements.update_tab_url, {}, url, window_id, static_cast<i64>(tab_index));
}

void SessionStore::delete_tab(i64 window_id, size_t tab_index)
{
    m_database.execute_statement(m_statements.delete_tab, {}, window_id, static_cast<i64>(tab_index));
    // Shift remaining tabs down
    m_database.execute_statement(m_statements.move_tabs_down, {}, window_id, static_cast<i64>(tab_index));
}

void SessionStore::set_active_tab(i64 window_id, size_t active_tab_index)
{
    m_database.execute_statement(m_statements.set_active_tab, {}, static_cast<i64>(active_tab_index), window_id);
}

void SessionStore::clear()
{
    m_database.execute_statement(m_statements.clear_tabs, {});
    m_database.execute_statement(m_statements.clear_windows, {});
}

}
