/*
 * Copyright (c) 2025, Your Name <your.email@example.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "NetworkAuditDialog.h"
#include <QDateTime>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTextStream>
#include <QVBoxLayout>

namespace Ladybird {

NetworkAuditDialog::NetworkAuditDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("Network Activity Audit Log");
    resize(900, 600);

    auto* layout = new QVBoxLayout(this);

    // Statistics label (will be updated when data is set)
    auto* stats_label = new QLabel("Loading audit data...", this);
    stats_label->setObjectName("stats_label");
    layout->addWidget(stats_label);

    // Filter controls
    auto* filter_layout = new QHBoxLayout();
    auto* filter_label = new QLabel("Filter:", this);
    filter_layout->addWidget(filter_label);

    m_filter_edit = new QLineEdit(this);
    m_filter_edit->setPlaceholderText("Search method, URL, or status...");
    connect(m_filter_edit, &QLineEdit::textChanged, this, &NetworkAuditDialog::apply_filter);
    filter_layout->addWidget(m_filter_edit);

    m_clear_filter_button = new QPushButton("Clear", this);
    connect(m_clear_filter_button, &QPushButton::clicked, [this]() {
        m_filter_edit->clear();
    });
    filter_layout->addWidget(m_clear_filter_button);

    layout->addLayout(filter_layout);

    // Table
    m_table = new QTableWidget(this);
    m_table->setColumnCount(6);
    m_table->setHorizontalHeaderLabels({ "Timestamp", "Method", "URL", "Status", "Sent", "Received" });
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch); // URL column stretches
    m_table->setSortingEnabled(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(m_table);

    // Export button
    m_export_button = new QPushButton("Export to CSV", this);
    connect(m_export_button, &QPushButton::clicked, this, &NetworkAuditDialog::on_export_button_clicked);
    layout->addWidget(m_export_button);

    // Close button
    auto* button_layout = new QHBoxLayout();
    button_layout->addStretch();
    auto* close_button = new QPushButton("Close", this);
    connect(close_button, &QPushButton::clicked, this, &QDialog::accept);
    button_layout->addWidget(close_button);
    layout->addLayout(button_layout);
}

void NetworkAuditDialog::set_audit_data(Vector<ByteString> const& entries, size_t total_bytes_sent, size_t total_bytes_received)
{
    m_total_bytes_sent = total_bytes_sent;
    m_total_bytes_received = total_bytes_received;

    // Parse serialized entries
    m_entries.clear();
    for (auto const& entry_str : entries) {
        auto parts = entry_str.split('|');
        if (parts.size() != 6)
            continue; // Skip malformed entries

        AuditEntry entry;
        entry.timestamp_ms = parts[0].to_number<u64>().value_or(0);
        entry.method = parts[1];
        entry.url = parts[2];
        entry.response_code = parts[3].to_number<u16>().value_or(0);
        entry.bytes_sent = parts[4].to_number<size_t>().value_or(0);
        entry.bytes_received = parts[5].to_number<size_t>().value_or(0);

        m_entries.append(entry);
    }

    // Initially, filtered entries = all entries
    m_filtered_entries = m_entries;

    // Update statistics label
    auto* stats_label = findChild<QLabel*>("stats_label");
    if (stats_label) {
        auto total_requests = m_entries.size();
        auto sent_formatted = format_bytes(m_total_bytes_sent);
        auto received_formatted = format_bytes(m_total_bytes_received);
        stats_label->setText(QString("Total Requests: %1 | Bytes Sent: %2 | Bytes Received: %3")
                                 .arg(total_requests)
                                 .arg(QString::fromUtf8(sent_formatted.characters()))
                                 .arg(QString::fromUtf8(received_formatted.characters())));
    }

    populate_table();
}

void NetworkAuditDialog::populate_table()
{
    m_table->setSortingEnabled(false);
    m_table->setRowCount(0);

    for (auto const& entry : m_filtered_entries) {
        int row = m_table->rowCount();
        m_table->insertRow(row);

        // Timestamp
        auto timestamp_str = format_timestamp(entry.timestamp_ms);
        m_table->setItem(row, 0, new QTableWidgetItem(QString::fromUtf8(timestamp_str.characters())));

        // Method
        m_table->setItem(row, 1, new QTableWidgetItem(QString::fromUtf8(entry.method.characters())));

        // URL
        m_table->setItem(row, 2, new QTableWidgetItem(QString::fromUtf8(entry.url.characters())));

        // Status
        auto status_str = entry.response_code == 0 ? ByteString("-") : ByteString::number(entry.response_code);
        m_table->setItem(row, 3, new QTableWidgetItem(QString::fromUtf8(status_str.characters())));

        // Bytes Sent
        auto sent_str = format_bytes(entry.bytes_sent);
        m_table->setItem(row, 4, new QTableWidgetItem(QString::fromUtf8(sent_str.characters())));

        // Bytes Received
        auto received_str = format_bytes(entry.bytes_received);
        m_table->setItem(row, 5, new QTableWidgetItem(QString::fromUtf8(received_str.characters())));
    }

    m_table->setSortingEnabled(true);
}

void NetworkAuditDialog::apply_filter()
{
    auto filter_text = m_filter_edit->text().toLower();
    if (filter_text.isEmpty()) {
        m_filtered_entries = m_entries;
    } else {
        m_filtered_entries.clear();
        for (auto const& entry : m_entries) {
            auto method_lower = entry.method.to_lowercase();
            auto url_lower = entry.url.to_lowercase();
            auto status_str = ByteString::number(entry.response_code);
            auto filter_byte_string = ByteString(filter_text.toUtf8().data());

            if (method_lower.contains(filter_byte_string) ||
                url_lower.contains(filter_byte_string) ||
                status_str.contains(filter_byte_string)) {
                m_filtered_entries.append(entry);
            }
        }
    }

    populate_table();
}

void NetworkAuditDialog::on_export_button_clicked()
{
    auto filename = QFileDialog::getSaveFileName(this, "Export Network Audit", "network_audit.csv", "CSV Files (*.csv)");
    if (filename.isEmpty())
        return;

    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::warning(this, "Export Failed", "Could not open file for writing.");
        return;
    }

    QTextStream out(&file);

    // Write CSV header
    out << "Timestamp,Method,URL,Status,Bytes Sent,Bytes Received\n";

    // Write data rows with proper CSV escaping
    for (auto const& entry : m_filtered_entries) {
        auto timestamp_str = format_timestamp(entry.timestamp_ms);
        auto status_str = entry.response_code == 0 ? ByteString("-") : ByteString::number(entry.response_code);

        // Escape CSV fields that contain commas, quotes, or newlines
        auto escape_csv = [](ByteString const& field) -> ByteString {
            if (field.contains(',') || field.contains('"') || field.contains('\n')) {
                auto escaped = field;
                escaped = escaped.replace("\""sv, "\"\""sv, ReplaceMode::All);
                return ByteString::formatted("\"{}\"", escaped);
            }
            return field;
        };

        out << QString::fromUtf8(escape_csv(timestamp_str).characters()) << ","
            << QString::fromUtf8(escape_csv(entry.method).characters()) << ","
            << QString::fromUtf8(escape_csv(entry.url).characters()) << ","
            << QString::fromUtf8(status_str.characters()) << ","
            << entry.bytes_sent << ","
            << entry.bytes_received << "\n";
    }

    file.close();

    QMessageBox::information(this, "Export Successful", QString("Exported %1 entries to %2").arg(m_filtered_entries.size()).arg(filename));
}

ByteString NetworkAuditDialog::format_bytes(size_t bytes) const
{
    if (bytes < 1024)
        return ByteString::formatted("{} B", bytes);
    else if (bytes < 1024 * 1024)
        return ByteString::formatted("{:.2f} KB", bytes / 1024.0);
    else
        return ByteString::formatted("{:.2f} MB", bytes / (1024.0 * 1024.0));
}

ByteString NetworkAuditDialog::format_timestamp(u64 timestamp_ms) const
{
    QDateTime dt = QDateTime::fromMSecsSinceEpoch(timestamp_ms);
    return ByteString(dt.toString("yyyy-MM-dd HH:mm:ss").toUtf8().data());
}

}
