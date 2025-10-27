/*
 * Copyright (c) 2025, Your Name <your.email@example.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Vector.h>
#include <QDialog>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>

namespace Ladybird {

class NetworkAuditDialog : public QDialog {
    Q_OBJECT

public:
    explicit NetworkAuditDialog(QWidget* parent = nullptr);
    ~NetworkAuditDialog() = default;

    void set_audit_data(Vector<ByteString> const& entries, size_t total_bytes_sent, size_t total_bytes_received);

private:
    struct AuditEntry {
        u64 timestamp_ms;
        ByteString method;
        ByteString url;
        u16 response_code;
        size_t bytes_sent;
        size_t bytes_received;
    };

    void populate_table();
    void apply_filter();
    void on_export_button_clicked();

    ByteString format_bytes(size_t bytes) const;
    ByteString format_timestamp(u64 timestamp_ms) const;

    QTableWidget* m_table { nullptr };
    QLineEdit* m_filter_edit { nullptr };
    QPushButton* m_clear_filter_button { nullptr };
    QPushButton* m_export_button { nullptr };

    Vector<AuditEntry> m_entries;
    Vector<AuditEntry> m_filtered_entries;
    size_t m_total_bytes_sent { 0 };
    size_t m_total_bytes_received { 0 };
};

}
