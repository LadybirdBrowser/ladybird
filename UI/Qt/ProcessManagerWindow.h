/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibURL/Forward.h>
#include <LibWebView/Forward.h>

#include <QDialog>

class QAction;
class QLabel;
class QPushButton;
class QLineEdit;
class QTimer;
class QTreeWidget;

namespace Ladybird {

QString task_manager_site_label(URL::URL const&);

class ProcessManagerWindow final : public QDialog {
public:
    explicit ProcessManagerWindow(WebView::ProcessManager&);

private:
    virtual void showEvent(QShowEvent*) override;
    virtual void hideEvent(QHideEvent*) override;
    virtual void changeEvent(QEvent*) override;
    void update_style();
    void refresh();
    void apply_filter();
    void update_actions();
    void end_selected_process();
    void show_context_menu(QPoint const&);

    WebView::ProcessManager& m_process_manager;
    QAction* m_end_process_action { nullptr };
    QAction* m_copy_pid_action { nullptr };
    QPushButton* m_end_process_button { nullptr };
    bool m_is_updating_style { false };
    QTreeWidget* m_processes { nullptr };
    QLineEdit* m_search { nullptr };
    QLabel* m_process_count { nullptr };
    QLabel* m_summary { nullptr };
    QTimer* m_timer { nullptr };
};

}
