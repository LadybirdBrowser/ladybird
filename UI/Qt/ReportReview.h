/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <AK/String.h>
#include <LibWebView/ReportDetail.h>
#include <LibWebView/ReportSubmission.h>

#include <QString>

class QFormLayout;
class QFrame;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QVBoxLayout;
class QWidget;

namespace Ladybird::ReportReview {

// The spacing of items within a group and of the groups themselves, which the screen around a review shares.
static constexpr int item_spacing = 8;
static constexpr int group_spacing = 28;
static constexpr int panel_padding = 16;
static constexpr int button_spacing = 12;

QLabel* create_label(QString const& text, QWidget* parent);
QLabel* create_heading(QString const& text, qreal scale, QWidget* parent);
QLabel* create_title(QString const& text, QWidget* parent);
QPushButton* create_disclosure(QString const& text, QWidget* content, QWidget* parent);
QVBoxLayout* create_group_layout(QWidget* group);

QFrame* create_details_panel(QWidget* parent);
QFormLayout* create_details_form();
void set_details(QFormLayout&, ReadonlySpan<WebView::ReportDetail>, QWidget* parent);

struct ReportTextPage {
    QWidget* page { nullptr };
    QPlainTextEdit* text { nullptr };
    QPushButton* back_button { nullptr };
};
ReportTextPage create_report_text_page(QWidget* parent);

void apply_style(QWidget& review, ReadonlySpan<QWidget*> fields);
bool focus_next_control(QWidget& review, bool next);

int progress_percent(WebView::ReportSubmission::Stage);
void show_actions(ReadonlySpan<QPushButton*> all_actions, ReadonlySpan<QPushButton*> actions, QPushButton* primary);
QString retry_message(String const& reason, u32 retry_number, u32 maximum_retries, u32 delay_seconds);

}
