/*
 * Copyright (c) 2025, Ladybird contributors
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "SecurityAlertDialog.h"
#include <QApplication>
#include <QFont>
#include <QFrame>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QStyle>

namespace Ladybird {

SecurityAlertDialog::SecurityAlertDialog(ThreatDetails const& details, QWidget* parent)
    : QDialog(parent)
    , m_details(details)
    , m_decision(UserDecision::Block)
{
    setWindowTitle("Security Threat Detected - Sentinel");
    setModal(true);
    resize(600, 450);
    setup_ui();
}

void SecurityAlertDialog::setup_ui()
{
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setSpacing(16);
    main_layout->setContentsMargins(20, 20, 20, 20);

    // Header with warning icon and title
    auto* header_layout = new QHBoxLayout();
    header_layout->setSpacing(12);

    m_icon_label = new QLabel(this);
    auto warning_icon = QApplication::style()->standardIcon(QStyle::SP_MessageBoxWarning);
    m_icon_label->setPixmap(warning_icon.pixmap(48, 48));
    header_layout->addWidget(m_icon_label);

    m_title_label = new QLabel("<b>Security Threat Detected</b>", this);
    QFont title_font = m_title_label->font();
    title_font.setPointSize(14);
    m_title_label->setFont(title_font);
    header_layout->addWidget(m_title_label, 1);

    main_layout->addLayout(header_layout);

    // Description
    auto* desc_label = new QLabel("Sentinel has detected malware in this download:", this);
    main_layout->addWidget(desc_label);

    // Threat details group
    auto* details_group = new QGroupBox("Threat Details", this);
    auto* details_layout = new QVBoxLayout(details_group);
    details_layout->setSpacing(8);

    // Filename
    auto* filename_layout = new QHBoxLayout();
    filename_layout->addWidget(new QLabel("<b>Filename:</b>", this));
    m_filename_label = new QLabel(m_details.filename, this);
    m_filename_label->setWordWrap(true);
    filename_layout->addWidget(m_filename_label, 1);
    details_layout->addLayout(filename_layout);

    // URL
    auto* url_layout = new QHBoxLayout();
    url_layout->addWidget(new QLabel("<b>URL:</b>", this));
    m_url_label = new QLabel(m_details.url, this);
    m_url_label->setWordWrap(true);
    m_url_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    url_layout->addWidget(m_url_label, 1);
    details_layout->addLayout(url_layout);

    // Rule name
    auto* rule_layout = new QHBoxLayout();
    rule_layout->addWidget(new QLabel("<b>Rule:</b>", this));
    m_rule_label = new QLabel(m_details.rule_name, this);
    rule_layout->addWidget(m_rule_label, 1);
    details_layout->addLayout(rule_layout);

    // Severity
    auto* severity_layout = new QHBoxLayout();
    severity_layout->addWidget(new QLabel("<b>Severity:</b>", this));
    m_severity_label = new QLabel(QString("<span style='color: %1;'>%2 %3</span>")
                                       .arg(severity_color())
                                       .arg(severity_icon())
                                       .arg(m_details.severity.toUpper()),
        this);
    severity_layout->addWidget(m_severity_label, 1);
    details_layout->addLayout(severity_layout);

    // Description
    auto* description_layout = new QVBoxLayout();
    description_layout->addWidget(new QLabel("<b>Description:</b>", this));
    m_description_label = new QLabel(m_details.description, this);
    m_description_label->setWordWrap(true);
    m_description_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    description_layout->addWidget(m_description_label);
    details_layout->addLayout(description_layout);

    // File hash (collapsed by default, selectable)
    if (!m_details.file_hash.isEmpty()) {
        auto* hash_layout = new QVBoxLayout();
        hash_layout->addWidget(new QLabel("<b>File Hash (SHA256):</b>", this));
        m_hash_label = new QLabel(QString("<small><tt>%1</tt></small>").arg(m_details.file_hash), this);
        m_hash_label->setWordWrap(true);
        m_hash_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        hash_layout->addWidget(m_hash_label);
        details_layout->addLayout(hash_layout);
    }

    main_layout->addWidget(details_group);

    // Separator
    auto* separator = new QFrame(this);
    separator->setFrameShape(QFrame::HLine);
    separator->setFrameShadow(QFrame::Sunken);
    main_layout->addWidget(separator);

    // Action prompt
    auto* action_label = new QLabel("<b>What would you like to do?</b>", this);
    main_layout->addWidget(action_label);

    // Action buttons
    auto* button_layout = new QHBoxLayout();
    button_layout->setSpacing(12);

    m_block_button = new QPushButton("🚫 Block", this);
    m_block_button->setToolTip("Block this download and delete the file");
    m_block_button->setAutoDefault(true);
    m_block_button->setDefault(true);
    connect(m_block_button, &QPushButton::clicked, this, &SecurityAlertDialog::on_block_clicked);
    button_layout->addWidget(m_block_button);

    m_allow_once_button = new QPushButton("✓ Allow Once", this);
    m_allow_once_button->setToolTip("Allow this download but ask again for similar threats");
    connect(m_allow_once_button, &QPushButton::clicked, this, &SecurityAlertDialog::on_allow_once_clicked);
    button_layout->addWidget(m_allow_once_button);

    m_always_allow_button = new QPushButton("✓ Always Allow", this);
    m_always_allow_button->setToolTip("Allow this download and create a policy to always allow it");
    connect(m_always_allow_button, &QPushButton::clicked, this, &SecurityAlertDialog::on_always_allow_clicked);
    button_layout->addWidget(m_always_allow_button);

    main_layout->addLayout(button_layout);

    // Remember decision checkbox
    m_remember_checkbox = new QCheckBox("Remember this decision (create policy)", this);
    m_remember_checkbox->setToolTip("Create a security policy based on this decision");
    m_remember_checkbox->setChecked(false);
    main_layout->addWidget(m_remember_checkbox);

    // Info label
    auto* info_label = new QLabel(
        "<small><i>Note: Blocking is recommended for detected threats. "
        "Only allow if you trust this file.</i></small>",
        this);
    info_label->setWordWrap(true);
    info_label->setStyleSheet("color: gray;");
    main_layout->addWidget(info_label);

    main_layout->addStretch();

    // Set focus to Block button by default
    m_block_button->setFocus();
}

void SecurityAlertDialog::on_block_clicked()
{
    m_decision = UserDecision::Block;
    emit userDecided(m_decision);
    accept();
}

void SecurityAlertDialog::on_allow_once_clicked()
{
    m_decision = UserDecision::AllowOnce;
    emit userDecided(m_decision);
    accept();
}

void SecurityAlertDialog::on_always_allow_clicked()
{
    m_decision = UserDecision::AlwaysAllow;

    // If "Always Allow" is clicked, automatically check "Remember"
    m_remember_checkbox->setChecked(true);

    emit userDecided(m_decision);
    accept();
}

QString SecurityAlertDialog::severity_icon() const
{
    auto severity_lower = m_details.severity.toLower();

    if (severity_lower == "critical")
        return "⚠️";
    else if (severity_lower == "high")
        return "🔴";
    else if (severity_lower == "medium")
        return "🟠";
    else if (severity_lower == "low")
        return "🟡";
    else
        return "ℹ️";
}

QString SecurityAlertDialog::severity_color() const
{
    auto severity_lower = m_details.severity.toLower();

    if (severity_lower == "critical")
        return "#d32f2f"; // Dark red
    else if (severity_lower == "high")
        return "#f44336"; // Red
    else if (severity_lower == "medium")
        return "#ff9800"; // Orange
    else if (severity_lower == "low")
        return "#ffc107"; // Amber
    else
        return "#2196f3"; // Blue
}

}
