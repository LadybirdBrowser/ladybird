/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "QuarantineManagerDialog.h"
#include <QDateTime>
#include <QFileDialog>
#include <QGroupBox>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTextEdit>
#include <QTextStream>
#include <QVBoxLayout>

namespace Ladybird {

QuarantineManagerDialog::QuarantineManagerDialog(QWidget* parent)
    : QDialog(parent)
{
    setWindowTitle("Quarantine Manager");
    resize(1000, 600);

    auto* layout = new QVBoxLayout(this);

    // Title and info section
    auto* title_label = new QLabel("<h2>Quarantine Manager</h2>", this);
    layout->addWidget(title_label);

    // Quarantine directory path
    m_directory_label = new QLabel("Quarantine Directory: Loading...", this);
    m_directory_label->setWordWrap(true);
    m_directory_label->setStyleSheet("QLabel { color: gray; font-size: 10pt; }");
    layout->addWidget(m_directory_label);

    // Filter controls
    auto* filter_layout = new QHBoxLayout();
    auto* filter_label = new QLabel("Search:", this);
    filter_layout->addWidget(filter_label);

    m_filter_edit = new QLineEdit(this);
    m_filter_edit->setPlaceholderText("Filter by filename, URL, or threat type...");
    connect(m_filter_edit, &QLineEdit::textChanged, this, &QuarantineManagerDialog::apply_filter);
    filter_layout->addWidget(m_filter_edit);

    m_clear_filter_button = new QPushButton(this);
    m_clear_filter_button->setText("Clear");
    connect(m_clear_filter_button, &QPushButton::clicked, [this]() {
        m_filter_edit->clear();
    });
    filter_layout->addWidget(m_clear_filter_button);

    layout->addLayout(filter_layout);

    // Table
    m_table = new QTableWidget(this);
    m_table->setColumnCount(6);
    m_table->setHorizontalHeaderLabels({ "Filename", "Origin", "Date", "Size", "Threat Type", "SHA256" });
    m_table->horizontalHeader()->setStretchLastSection(false);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch); // Filename stretches
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch); // Origin stretches
    m_table->horizontalHeader()->setSectionResizeMode(5, QHeaderView::Interactive); // SHA256 interactive
    m_table->setSortingEnabled(true);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::ExtendedSelection); // Allow multi-select
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);

    // Double-click to view metadata
    connect(m_table, &QTableWidget::itemDoubleClicked, this, &QuarantineManagerDialog::on_view_metadata_clicked);

    layout->addWidget(m_table);

    // Action buttons
    auto* action_layout = new QHBoxLayout();

    m_view_metadata_button = new QPushButton("View Metadata", this);
    connect(m_view_metadata_button, &QPushButton::clicked, this, &QuarantineManagerDialog::on_view_metadata_clicked);
    action_layout->addWidget(m_view_metadata_button);

    m_restore_button = new QPushButton("Restore Selected", this);
    m_restore_button->setStyleSheet("QPushButton { background-color: #4CAF50; color: white; }");
    connect(m_restore_button, &QPushButton::clicked, this, &QuarantineManagerDialog::on_restore_clicked);
    action_layout->addWidget(m_restore_button);

    m_delete_button = new QPushButton("Delete Selected", this);
    m_delete_button->setStyleSheet("QPushButton { background-color: #f44336; color: white; }");
    connect(m_delete_button, &QPushButton::clicked, this, &QuarantineManagerDialog::on_delete_clicked);
    action_layout->addWidget(m_delete_button);

    m_export_button = new QPushButton("Export to CSV", this);
    connect(m_export_button, &QPushButton::clicked, this, &QuarantineManagerDialog::on_export_button_clicked);
    action_layout->addWidget(m_export_button);

    action_layout->addStretch();

    m_delete_all_button = new QPushButton("Delete All", this);
    m_delete_all_button->setStyleSheet("QPushButton { background-color: #9C27B0; color: white; }");
    connect(m_delete_all_button, &QPushButton::clicked, this, &QuarantineManagerDialog::on_delete_all_clicked);
    action_layout->addWidget(m_delete_all_button);

    layout->addLayout(action_layout);

    // Close button
    auto* button_layout = new QHBoxLayout();
    button_layout->addStretch();
    auto* close_button = new QPushButton("Close", this);
    connect(close_button, &QPushButton::clicked, this, &QDialog::accept);
    button_layout->addWidget(close_button);
    layout->addLayout(button_layout);
}

void QuarantineManagerDialog::set_quarantine_entries(Vector<QuarantineEntry> const& entries)
{
    m_entries.clear();
    for (auto const& entry : entries) {
        m_entries.append(entry);
    }

    // Initially, filtered entries = all entries
    m_filtered_entries = m_entries;

    populate_table();
}

void QuarantineManagerDialog::set_quarantine_directory(ByteString const& directory)
{
    m_quarantine_directory = directory;
    m_directory_label->setText(QString("Quarantine Directory: %1").arg(QString::fromUtf8(directory.characters())));
}

void QuarantineManagerDialog::populate_table()
{
    m_table->setSortingEnabled(false);
    m_table->setRowCount(0);

    for (auto const& entry : m_filtered_entries) {
        int row = m_table->rowCount();
        m_table->insertRow(row);

        // Filename
        m_table->setItem(row, 0, new QTableWidgetItem(QString::fromUtf8(entry.filename.characters())));

        // Origin (URL)
        m_table->setItem(row, 1, new QTableWidgetItem(QString::fromUtf8(entry.original_url.characters())));

        // Date
        auto date_str = format_timestamp(entry.detection_time);
        m_table->setItem(row, 2, new QTableWidgetItem(QString::fromUtf8(date_str.characters())));

        // Size
        auto size_str = format_bytes(entry.file_size);
        m_table->setItem(row, 3, new QTableWidgetItem(QString::fromUtf8(size_str.characters())));

        // Threat Type (concatenate rule names)
        ByteString threat_type;
        if (entry.rule_names.is_empty()) {
            threat_type = "Unknown";
        } else {
            for (size_t i = 0; i < entry.rule_names.size(); ++i) {
                if (i > 0)
                    threat_type = ByteString::formatted("{}, {}", threat_type, entry.rule_names[i]);
                else
                    threat_type = entry.rule_names[i];
            }
        }
        m_table->setItem(row, 4, new QTableWidgetItem(QString::fromUtf8(threat_type.characters())));

        // SHA256 (truncate for display)
        auto sha256_display = entry.sha256.length() > 16
            ? ByteString::formatted("{}...", entry.sha256.substring(0, 16))
            : entry.sha256;
        auto* sha256_item = new QTableWidgetItem(QString::fromUtf8(sha256_display.characters()));
        sha256_item->setToolTip(QString::fromUtf8(entry.sha256.characters())); // Full hash in tooltip
        m_table->setItem(row, 5, sha256_item);

        // Store quarantine_id in the first item's user data for later retrieval
        m_table->item(row, 0)->setData(Qt::UserRole, QString::fromUtf8(entry.quarantine_id.characters()));
    }

    m_table->setSortingEnabled(true);
}

void QuarantineManagerDialog::apply_filter()
{
    QString filter_text = m_filter_edit->text().toLower();

    if (filter_text.isEmpty()) {
        m_filtered_entries = m_entries;
    } else {
        m_filtered_entries.clear();
        for (auto const& entry : m_entries) {
            bool matches = false;

            ByteString filter_str(filter_text.toUtf8().data());

            // Check filename
            if (entry.filename.to_lowercase().contains(filter_str, AK::CaseSensitivity::CaseInsensitive)) {
                matches = true;
            }

            // Check URL
            if (entry.original_url.to_lowercase().contains(filter_str, AK::CaseSensitivity::CaseInsensitive)) {
                matches = true;
            }

            // Check rule names
            for (auto const& rule : entry.rule_names) {
                if (rule.to_lowercase().contains(filter_str, AK::CaseSensitivity::CaseInsensitive)) {
                    matches = true;
                    break;
                }
            }

            // Check SHA256
            if (entry.sha256.to_lowercase().contains(filter_str, AK::CaseSensitivity::CaseInsensitive)) {
                matches = true;
            }

            if (matches) {
                m_filtered_entries.append(entry);
            }
        }
    }

    populate_table();
}

void QuarantineManagerDialog::on_restore_clicked()
{
    auto selected_items = m_table->selectedItems();
    if (selected_items.isEmpty()) {
        QMessageBox::information(this, "No Selection", "Please select at least one quarantined file to restore.");
        return;
    }

    // Get unique rows
    QSet<int> selected_rows;
    for (auto* item : selected_items) {
        selected_rows.insert(item->row());
    }

    // Confirm restore
    auto count = selected_rows.size();
    auto result = QMessageBox::question(
        this,
        "Confirm Restore",
        QString("Are you sure you want to restore %1 file(s) from quarantine?\n\n"
                "The files will be restored to the Downloads directory.").arg(count),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No
    );

    if (result == QMessageBox::Yes) {
        // Emit restore signal for each selected file
        for (int row : selected_rows) {
            auto* item = m_table->item(row, 0);
            if (item) {
                auto quarantine_id = item->data(Qt::UserRole).toString().toUtf8();
                emit restore_requested(ByteString(quarantine_id.data()));
            }
        }

        QMessageBox::information(this, "Restore Initiated",
            QString("Restore operation initiated for %1 file(s).").arg(count));
    }
}

void QuarantineManagerDialog::on_delete_clicked()
{
    auto selected_items = m_table->selectedItems();
    if (selected_items.isEmpty()) {
        QMessageBox::information(this, "No Selection", "Please select at least one quarantined file to delete.");
        return;
    }

    // Get unique rows
    QSet<int> selected_rows;
    for (auto* item : selected_items) {
        selected_rows.insert(item->row());
    }

    // Confirm delete
    auto count = selected_rows.size();
    auto result = QMessageBox::warning(
        this,
        "Confirm Delete",
        QString("Are you sure you want to permanently delete %1 file(s) from quarantine?\n\n"
                "This action cannot be undone.").arg(count),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No
    );

    if (result == QMessageBox::Yes) {
        // Emit delete signal for each selected file
        for (int row : selected_rows) {
            auto* item = m_table->item(row, 0);
            if (item) {
                auto quarantine_id = item->data(Qt::UserRole).toString().toUtf8();
                emit delete_requested(ByteString(quarantine_id.data()));
            }
        }

        QMessageBox::information(this, "Delete Complete",
            QString("Deleted %1 file(s) from quarantine.").arg(count));

        // Refresh the table by removing deleted rows
        QList<int> rows_to_remove = selected_rows.values();
        std::sort(rows_to_remove.begin(), rows_to_remove.end(), std::greater<int>());
        for (int row : rows_to_remove) {
            m_table->removeRow(row);
        }
    }
}

void QuarantineManagerDialog::on_view_metadata_clicked()
{
    auto selected_items = m_table->selectedItems();
    if (selected_items.isEmpty()) {
        QMessageBox::information(this, "No Selection", "Please select a quarantined file to view its metadata.");
        return;
    }

    // Get the first selected row
    int row = selected_items.first()->row();
    auto* item = m_table->item(row, 0);
    if (!item)
        return;

    auto quarantine_id = item->data(Qt::UserRole).toString().toUtf8();

    // Find the entry
    QuarantineEntry const* selected_entry = nullptr;
    for (auto const& entry : m_filtered_entries) {
        if (entry.quarantine_id == ByteString(quarantine_id.data())) {
            selected_entry = &entry;
            break;
        }
    }

    if (!selected_entry) {
        QMessageBox::warning(this, "Error", "Could not find metadata for selected file.");
        return;
    }

    // Create metadata dialog
    QDialog metadata_dialog(this);
    metadata_dialog.setWindowTitle("Quarantine Metadata");
    metadata_dialog.resize(700, 500);

    auto* dialog_layout = new QVBoxLayout(&metadata_dialog);

    // Title
    auto* title = new QLabel(QString("<h3>Metadata for: %1</h3>").arg(QString::fromUtf8(selected_entry->filename.characters())));
    dialog_layout->addWidget(title);

    // Metadata text
    auto* metadata_text = new QTextEdit(&metadata_dialog);
    metadata_text->setReadOnly(true);

    QString metadata_content;
    metadata_content += QString("<b>Quarantine ID:</b> %1<br>").arg(QString::fromUtf8(selected_entry->quarantine_id.characters()));
    metadata_content += QString("<b>Filename:</b> %1<br>").arg(QString::fromUtf8(selected_entry->filename.characters()));
    metadata_content += QString("<b>Original URL:</b> %1<br>").arg(QString::fromUtf8(selected_entry->original_url.characters()));
    metadata_content += QString("<b>Detection Time:</b> %1<br>").arg(QString::fromUtf8(format_timestamp(selected_entry->detection_time).characters()));
    metadata_content += QString("<b>File Size:</b> %1<br>").arg(QString::fromUtf8(format_bytes(selected_entry->file_size).characters()));
    metadata_content += QString("<b>SHA256:</b> %1<br><br>").arg(QString::fromUtf8(selected_entry->sha256.characters()));

    metadata_content += "<b>Threat Rules Matched:</b><br>";
    if (selected_entry->rule_names.is_empty()) {
        metadata_content += "<i>None recorded</i><br>";
    } else {
        metadata_content += "<ul>";
        for (auto const& rule : selected_entry->rule_names) {
            metadata_content += QString("<li>%1</li>").arg(QString::fromUtf8(rule.characters()));
        }
        metadata_content += "</ul>";
    }

    metadata_text->setHtml(metadata_content);
    dialog_layout->addWidget(metadata_text);

    // Close button
    auto* button_layout = new QHBoxLayout();
    button_layout->addStretch();
    auto* close_button = new QPushButton("Close", &metadata_dialog);
    connect(close_button, &QPushButton::clicked, &metadata_dialog, &QDialog::accept);
    button_layout->addWidget(close_button);
    dialog_layout->addLayout(button_layout);

    metadata_dialog.exec();
}

void QuarantineManagerDialog::on_export_button_clicked()
{
    if (m_filtered_entries.is_empty()) {
        QMessageBox::information(this, "No Data", "No quarantined files to export.");
        return;
    }

    QString filename = QFileDialog::getSaveFileName(
        this,
        "Export Quarantine Data",
        "quarantine_export.csv",
        "CSV Files (*.csv);;All Files (*)"
    );

    if (filename.isEmpty())
        return;

    QFile file(filename);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QMessageBox::critical(this, "Export Failed", "Could not open file for writing.");
        return;
    }

    QTextStream stream(&file);

    // CSV header
    stream << "Quarantine ID,Filename,Original URL,Detection Time,File Size (bytes),SHA256,Threat Types\n";

    // CSV data
    for (auto const& entry : m_filtered_entries) {
        // Escape fields that might contain commas or quotes
        auto escape_csv = [](ByteString const& str) -> QString {
            QString qstr = QString::fromUtf8(str.characters());
            if (qstr.contains(',') || qstr.contains('"') || qstr.contains('\n')) {
                qstr.replace('"', "\"\"");
                return QString("\"%1\"").arg(qstr);
            }
            return qstr;
        };

        ByteString threat_types;
        for (size_t i = 0; i < entry.rule_names.size(); ++i) {
            if (i > 0)
                threat_types = ByteString::formatted("{}; {}", threat_types, entry.rule_names[i]);
            else
                threat_types = entry.rule_names[i];
        }

        stream << escape_csv(entry.quarantine_id) << ","
               << escape_csv(entry.filename) << ","
               << escape_csv(entry.original_url) << ","
               << escape_csv(entry.detection_time) << ","
               << entry.file_size << ","
               << escape_csv(entry.sha256) << ","
               << escape_csv(threat_types) << "\n";
    }

    file.close();
    QMessageBox::information(this, "Export Complete",
        QString("Successfully exported %1 entries to:\n%2").arg(m_filtered_entries.size()).arg(filename));
}

void QuarantineManagerDialog::on_delete_all_clicked()
{
    if (m_entries.is_empty()) {
        QMessageBox::information(this, "No Files", "There are no quarantined files to delete.");
        return;
    }

    auto count = m_entries.size();
    auto result = QMessageBox::warning(
        this,
        "Confirm Delete All",
        QString("Are you sure you want to permanently delete ALL %1 quarantined file(s)?\n\n"
                "This action cannot be undone.").arg(count),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No
    );

    if (result == QMessageBox::Yes) {
        // Emit delete signal for all files
        for (auto const& entry : m_entries) {
            emit delete_requested(entry.quarantine_id);
        }

        // Clear the table
        m_table->setRowCount(0);
        m_entries.clear();
        m_filtered_entries.clear();

        QMessageBox::information(this, "Delete Complete",
            QString("Deleted all %1 file(s) from quarantine.").arg(count));
    }
}

ByteString QuarantineManagerDialog::format_bytes(size_t bytes) const
{
    if (bytes < 1024)
        return ByteString::formatted("{} B", bytes);
    else if (bytes < 1024 * 1024)
        return ByteString::formatted("{:.2f} KB", bytes / 1024.0);
    else if (bytes < 1024 * 1024 * 1024)
        return ByteString::formatted("{:.2f} MB", bytes / (1024.0 * 1024.0));
    else
        return ByteString::formatted("{:.2f} GB", bytes / (1024.0 * 1024.0 * 1024.0));
}

ByteString QuarantineManagerDialog::format_timestamp(ByteString const& iso_timestamp) const
{
    // Parse ISO 8601 timestamp and format it nicely
    // Expected format: YYYY-MM-DDTHH:MM:SS.sssZ or similar
    auto qstr = QString::fromUtf8(iso_timestamp.characters());
    auto datetime = QDateTime::fromString(qstr, Qt::ISODate);

    if (datetime.isValid()) {
        return ByteString(datetime.toString("yyyy-MM-dd HH:mm:ss").toUtf8().data());
    }

    // Fallback: return as-is if parsing fails
    return iso_timestamp;
}

}
