/*
 * Copyright (c) 2025, the Ladybird Browser Contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/ProxyValidator.h>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QMessageBox>
#include <QVBoxLayout>
#include <UI/Qt/ProxySettingsDialog.h>

namespace Ladybird {

ProxySettingsDialog::ProxySettingsDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("Proxy Settings");
    setModal(true);
    resize(400, 350);

    setup_ui();
}

void ProxySettingsDialog::setup_ui()
{
    auto* main_layout = new QVBoxLayout(this);

    // Create form layout for input fields
    auto* form_layout = new QFormLayout();

    // Proxy Type dropdown
    m_proxy_type_combo = new QComboBox(this);
    m_proxy_type_combo->addItem("SOCKS5H (DNS via proxy)", static_cast<int>(IPC::ProxyType::SOCKS5H));
    m_proxy_type_combo->addItem("SOCKS5 (local DNS)", static_cast<int>(IPC::ProxyType::SOCKS5));
    m_proxy_type_combo->addItem("HTTP", static_cast<int>(IPC::ProxyType::HTTP));
    m_proxy_type_combo->addItem("HTTPS", static_cast<int>(IPC::ProxyType::HTTPS));
    m_proxy_type_combo->setCurrentIndex(0);  // Default to SOCKS5H (most secure)
    connect(m_proxy_type_combo, QOverload<int>::of(&QComboBox::currentIndexChanged),
        this, &ProxySettingsDialog::on_proxy_type_changed);
    form_layout->addRow("Proxy Type:", m_proxy_type_combo);

    // Host input
    m_host_edit = new QLineEdit(this);
    m_host_edit->setPlaceholderText("e.g., localhost or 192.168.1.100");
    form_layout->addRow("Host:", m_host_edit);

    // Port input
    m_port_spinbox = new QSpinBox(this);
    m_port_spinbox->setRange(1, 65535);
    m_port_spinbox->setValue(1080);  // Default SOCKS5 port
    form_layout->addRow("Port:", m_port_spinbox);

    // Authentication checkbox
    m_auth_checkbox = new QCheckBox("Authentication Required", this);
    connect(m_auth_checkbox, &QCheckBox::toggled, this, &ProxySettingsDialog::on_auth_checkbox_toggled);
    form_layout->addRow("", m_auth_checkbox);

    // Username input (initially hidden)
    m_username_edit = new QLineEdit(this);
    m_username_edit->setPlaceholderText("Username");
    m_username_edit->setVisible(false);
    form_layout->addRow("Username:", m_username_edit);

    // Password input (initially hidden)
    m_password_edit = new QLineEdit(this);
    m_password_edit->setPlaceholderText("Password");
    m_password_edit->setEchoMode(QLineEdit::Password);
    m_password_edit->setVisible(false);
    form_layout->addRow("Password:", m_password_edit);

    main_layout->addLayout(form_layout);

    // Status label for test results
    m_status_label = new QLabel(this);
    m_status_label->setWordWrap(true);
    m_status_label->setStyleSheet("QLabel { color: #666; }");
    main_layout->addWidget(m_status_label);

    // Add stretch to push buttons to bottom
    main_layout->addStretch();

    // Buttons
    auto* button_layout = new QHBoxLayout();

    m_test_button = new QPushButton("Test Connection", this);
    connect(m_test_button, &QPushButton::clicked, this, &ProxySettingsDialog::on_test_button_clicked);
    button_layout->addWidget(m_test_button);

    button_layout->addStretch();

    m_save_button = new QPushButton("Save", this);
    m_save_button->setDefault(true);
    connect(m_save_button, &QPushButton::clicked, this, &ProxySettingsDialog::on_save_button_clicked);
    button_layout->addWidget(m_save_button);

    m_cancel_button = new QPushButton("Cancel", this);
    connect(m_cancel_button, &QPushButton::clicked, this, &ProxySettingsDialog::on_cancel_button_clicked);
    button_layout->addWidget(m_cancel_button);

    main_layout->addLayout(button_layout);
}

void ProxySettingsDialog::on_proxy_type_changed([[maybe_unused]] int index)
{
    // Update default port based on proxy type
    auto type = static_cast<IPC::ProxyType>(m_proxy_type_combo->currentData().toInt());
    switch (type) {
    case IPC::ProxyType::SOCKS5:
    case IPC::ProxyType::SOCKS5H:
        m_port_spinbox->setValue(1080);
        break;
    case IPC::ProxyType::HTTP:
        m_port_spinbox->setValue(3128);  // Common Squid default
        break;
    case IPC::ProxyType::HTTPS:
        m_port_spinbox->setValue(3129);
        break;
    case IPC::ProxyType::None:
        break;
    }
}

void ProxySettingsDialog::on_auth_checkbox_toggled([[maybe_unused]] bool checked)
{
    update_auth_fields_visibility();
}

void ProxySettingsDialog::update_auth_fields_visibility()
{
    bool auth_enabled = m_auth_checkbox->isChecked();
    m_username_edit->setVisible(auth_enabled);
    m_password_edit->setVisible(auth_enabled);

    // Find the labels for username/password rows and show/hide them
    auto* form = qobject_cast<QFormLayout*>(layout()->itemAt(0)->layout());
    if (form) {
        // Update layout to show/hide username/password rows
        form->labelForField(m_username_edit)->setVisible(auth_enabled);
        form->labelForField(m_password_edit)->setVisible(auth_enabled);
    }
}

void ProxySettingsDialog::on_test_button_clicked()
{
    if (!validate_inputs()) {
        return;
    }

    // Build proxy config from current inputs
    auto config = get_proxy_config();

    // Test the connection using ProxyValidator
    // NOTE: This validation makes a synchronous TCP connection which may take several seconds
    // This is acceptable here because:
    // - User explicitly clicked "Test Connection" button
    // - They expect to wait for the test result
    // - This is a rare operation (only during proxy configuration)
    m_status_label->setText("Testing connection (may take a few seconds)...");
    m_status_label->setStyleSheet("QLabel { color: #666; }");
    m_test_button->setEnabled(false);

    // Run proxy test
    auto result = IPC::ProxyValidator::test_proxy(config);

    if (result.is_error()) {
        m_status_label->setText(QString("Connection failed: %1")
                                     .arg(QString::fromUtf8(result.error().string_literal().bytes().data())));
        m_status_label->setStyleSheet("QLabel { color: #D32F2F; }");  // Red color
    } else {
        m_status_label->setText("Connection successful!");
        m_status_label->setStyleSheet("QLabel { color: #388E3C; }");  // Green color
    }

    m_test_button->setEnabled(true);
}

void ProxySettingsDialog::on_save_button_clicked()
{
    if (!validate_inputs()) {
        return;
    }

    accept();  // Close dialog with Accepted result
}

void ProxySettingsDialog::on_cancel_button_clicked()
{
    reject();  // Close dialog with Rejected result
}

bool ProxySettingsDialog::validate_inputs()
{
    // Validate host
    if (m_host_edit->text().trimmed().isEmpty()) {
        QMessageBox::warning(this, "Invalid Input", "Please enter a proxy host.");
        m_host_edit->setFocus();
        return false;
    }

    // Validate port (spinbox already enforces range 1-65535)

    // Validate authentication fields if enabled
    if (m_auth_checkbox->isChecked()) {
        if (m_username_edit->text().trimmed().isEmpty()) {
            QMessageBox::warning(this, "Invalid Input", "Please enter a username or disable authentication.");
            m_username_edit->setFocus();
            return false;
        }
    }

    return true;
}

IPC::ProxyConfig ProxySettingsDialog::get_proxy_config() const
{
    IPC::ProxyConfig config;

    // Get proxy type from combo box
    config.type = static_cast<IPC::ProxyType>(m_proxy_type_combo->currentData().toInt());

    // Get host and port
    config.host = m_host_edit->text().trimmed().toUtf8().data();
    config.port = m_port_spinbox->value();

    // Get authentication if enabled
    if (m_auth_checkbox->isChecked()) {
        config.username = m_username_edit->text().trimmed().toUtf8().data();
        if (!m_password_edit->text().isEmpty()) {
            config.password = m_password_edit->text().toUtf8().data();
        }
    }

    return config;
}

void ProxySettingsDialog::set_proxy_config(IPC::ProxyConfig const& config)
{
    // Set proxy type
    for (int i = 0; i < m_proxy_type_combo->count(); ++i) {
        if (static_cast<IPC::ProxyType>(m_proxy_type_combo->itemData(i).toInt()) == config.type) {
            m_proxy_type_combo->setCurrentIndex(i);
            break;
        }
    }

    // Set host and port
    m_host_edit->setText(QString::fromUtf8(config.host.characters()));
    m_port_spinbox->setValue(config.port);

    // Set authentication fields
    if (config.username.has_value()) {
        m_auth_checkbox->setChecked(true);
        m_username_edit->setText(QString::fromUtf8(config.username->characters()));
        if (config.password.has_value()) {
            m_password_edit->setText(QString::fromUtf8(config.password->characters()));
        }
    } else {
        m_auth_checkbox->setChecked(false);
    }

    update_auth_fields_visibility();
}

}
