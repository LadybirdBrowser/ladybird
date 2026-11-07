/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/kmalloc.h>
#include <LibURL/Forward.h>
#include <LibWebView/Forward.h>
#include <LibWebView/ProcessSnapshot.h>

#include <QDialog>
#include <QIcon>

class QAction;
class QLabel;
class QPushButton;
class QLineEdit;
class QTimer;
class QTreeWidget;
class QTreeWidgetItem;

namespace WebView {

struct ProcessPageSnapshot;

}

namespace Ladybird {

QString task_manager_site_label(URL::URL const&);

// The Task Manager lists the browser's own processes, then each site with the processes that show it, then the spare
// processes kept ready for the next site.
class ProcessManagerWindow final : public QDialog {
public:
    AK_ALLOC_WITH_KMALLOC;

    // Where the window gets what it shows, and how it ends a process.
    struct Source {
        Function<Vector<WebView::ProcessSnapshot>()> take_snapshots;
        Function<bool(u64 child_id)> end_process;
        Function<Optional<String>(WebView::ProcessPageSnapshot const&)> favicon_hash;
        Function<Optional<ByteBuffer>(String const& favicon_hash)> favicon_png;
    };

    explicit ProcessManagerWindow(Source);

    // Called as the window is shown, and as its title changes, so the platform can hide the title and place the window
    // controls in the window's title bar.
    Function<void()> on_title_bar_change;
    // Called as the empty part of the title bar is pressed, where the window's content covers the platform's own title
    // bar, so the platform can start moving the window. Returns whether it did.
    Function<bool()> on_title_bar_drag;

private:
    virtual bool event(QEvent*) override;
    virtual void showEvent(QShowEvent*) override;
    virtual void hideEvent(QHideEvent*) override;
    virtual void changeEvent(QEvent*) override;
    void update_style();
    void refresh();
    void apply_filter();
    void update_actions();
    void end_selection();
    void show_context_menu(QPoint const&);
    QIcon favicon_for(WebView::ProcessPageSnapshot const&);

    Source m_source;
    // The kind of each process in the last refresh, by its child ID.
    HashMap<u64, WebView::ProcessType> m_process_types;
    QAction* m_end_action { nullptr };
    QAction* m_copy_pid_action { nullptr };
    QPushButton* m_end_button { nullptr };
    bool m_is_updating_style { false };
    QTreeWidget* m_processes { nullptr };
    QLineEdit* m_search { nullptr };
    QAction* m_search_icon { nullptr };
    QLabel* m_summary { nullptr };
    QTimer* m_timer { nullptr };
    QWidget* m_title_bar { nullptr };
    QLabel* m_title { nullptr };
    QWidget* m_footer { nullptr };
    QIcon m_browser_icon;
    QIcon m_document_icon;
    QIcon m_network_icon;
    QIcon m_compositor_icon;
    QIcon m_hub_icon;
    QIcon m_image_icon;
    QIcon m_service_icon;
    HashMap<String, QIcon> m_favicons;
    HashTable<String> m_expanded_sites;
};

}
