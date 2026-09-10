/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QDialog>

class QLabel;
class QLineEdit;
class QTabBar;
class QTimer;
class QTreeWidget;

namespace Ladybird {

class ProcessManagerWindow final : public QDialog {
public:
    ProcessManagerWindow();

private:
    virtual void showEvent(QShowEvent*) override;
    virtual void hideEvent(QHideEvent*) override;
    virtual void changeEvent(QEvent*) override;
    void update_style();
    void refresh();
    void apply_filter();

    bool m_is_updating_style { false };
    QTreeWidget* m_processes { nullptr };
    QLineEdit* m_search { nullptr };
    QTabBar* m_filters { nullptr };
    QLabel* m_process_count { nullptr };
    QLabel* m_summary { nullptr };
    QTimer* m_timer { nullptr };
};

}
