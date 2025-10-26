/*
 * Copyright (c) 2025, the Ladybird Browser Contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibIPC/ProxyConfig.h>
#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>

namespace Ladybird {

class ProxySettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit ProxySettingsDialog(QWidget* parent = nullptr);

    IPC::ProxyConfig get_proxy_config() const;
    void set_proxy_config(IPC::ProxyConfig const& config);

private slots:
    void on_proxy_type_changed(int index);
    void on_auth_checkbox_toggled(bool checked);
    void on_test_button_clicked();
    void on_save_button_clicked();
    void on_cancel_button_clicked();

private:
    void setup_ui();
    void update_auth_fields_visibility();
    bool validate_inputs();

    // UI components
    QComboBox* m_proxy_type_combo;
    QLineEdit* m_host_edit;
    QSpinBox* m_port_spinbox;
    QCheckBox* m_auth_checkbox;
    QLineEdit* m_username_edit;
    QLineEdit* m_password_edit;
    QPushButton* m_test_button;
    QPushButton* m_save_button;
    QPushButton* m_cancel_button;
    QLabel* m_status_label;
};

}
