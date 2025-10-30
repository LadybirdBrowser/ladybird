/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Vector.h>
#include <QDialog>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>

namespace Ladybird {

class QuarantineManagerDialog : public QDialog {
    Q_OBJECT

public:
    struct QuarantineEntry {
        ByteString quarantine_id;
        ByteString filename;
        ByteString original_url;
        ByteString detection_time;
        size_t file_size { 0 };
        ByteString sha256;
        Vector<ByteString> rule_names;
    };

    explicit QuarantineManagerDialog(QWidget* parent = nullptr);
    ~QuarantineManagerDialog() = default;

    void set_quarantine_entries(Vector<QuarantineEntry> const& entries);
    void set_quarantine_directory(ByteString const& directory);

signals:
    void restore_requested(ByteString quarantine_id);
    void delete_requested(ByteString quarantine_id);

private:
    void populate_table();
    void apply_filter();
    void on_restore_clicked();
    void on_delete_clicked();
    void on_view_metadata_clicked();
    void on_export_button_clicked();
    void on_delete_all_clicked();

    ByteString format_bytes(size_t bytes) const;
    ByteString format_timestamp(ByteString const& iso_timestamp) const;

    QTableWidget* m_table { nullptr };
    QLineEdit* m_filter_edit { nullptr };
    QLabel* m_directory_label { nullptr };
    QPushButton* m_clear_filter_button { nullptr };
    QPushButton* m_restore_button { nullptr };
    QPushButton* m_delete_button { nullptr };
    QPushButton* m_view_metadata_button { nullptr };
    QPushButton* m_export_button { nullptr };
    QPushButton* m_delete_all_button { nullptr };

    Vector<QuarantineEntry> m_entries;
    Vector<QuarantineEntry> m_filtered_entries;
    ByteString m_quarantine_directory;
};

}
