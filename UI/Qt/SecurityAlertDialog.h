/*
 * Copyright (c) 2025, Ladybird contributors
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <QCheckBox>
#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

namespace Ladybird {

class SecurityAlertDialog : public QDialog {
    Q_OBJECT

public:
    struct ThreatDetails {
        QString url;
        QString filename;
        QString rule_name;
        QString severity;
        QString description;
        QString file_hash;
        QString mime_type;
        qint64 file_size;
    };

    enum class UserDecision {
        Block,
        AllowOnce,
        AlwaysAllow,
        Quarantine
    };

    explicit SecurityAlertDialog(ThreatDetails const& details, QWidget* parent = nullptr);

    UserDecision decision() const { return m_decision; }
    bool should_remember() const { return m_remember_checkbox->isChecked(); }

signals:
    void userDecided(UserDecision decision);

private:
    void setup_ui();
    void on_block_clicked();
    void on_allow_once_clicked();
    void on_always_allow_clicked();
    void on_quarantine_clicked();

    QString severity_icon() const;
    QString severity_color() const;

    ThreatDetails m_details;
    UserDecision m_decision;

    // UI Components
    QLabel* m_title_label;
    QLabel* m_icon_label;
    QLabel* m_filename_label;
    QLabel* m_url_label;
    QLabel* m_rule_label;
    QLabel* m_severity_label;
    QLabel* m_description_label;
    QLabel* m_hash_label;
    QCheckBox* m_remember_checkbox;
    QPushButton* m_block_button;
    QPushButton* m_allow_once_button;
    QPushButton* m_always_allow_button;
    QPushButton* m_quarantine_button;
};

}
