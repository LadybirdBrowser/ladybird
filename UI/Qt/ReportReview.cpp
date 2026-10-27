/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/ReportReview.h>
#include <UI/Qt/StringUtils.h>

#include <QApplication>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace Ladybird::ReportReview {

QLabel* create_label(QString const& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

QLabel* create_heading(QString const& text, qreal scale, QWidget* parent)
{
    auto* label = create_label(text, parent);
    auto font = label->font();
    font.setPointSizeF(font.pointSizeF() * scale);
    font.setBold(true);
    label->setFont(font);
    return label;
}

QLabel* create_title(QString const& text, QWidget* parent)
{
    auto* label = create_label(text, parent);
    auto font = label->font();
    font.setPixelSize(22);
    font.setWeight(QFont::DemiBold);
    label->setFont(font);
    return label;
}

// A button that shows or hides the content below it, which starts out hidden. It is drawn as plain text, flush with the
// text around it, as styles frame buttons on hover and press and indent them. A triangle in the same font shows whether
// the content is shown, as styles size arrows to the icon size. Keyboard focus shows in the accent color.
QPushButton* create_disclosure(QString const& text, QWidget* content, QWidget* parent)
{
    auto* button = new QPushButton(parent);
    button->setAutoDefault(false);
    button->setStyleSheet("QPushButton { border: none; background: transparent; padding: 0; margin: 0; text-align: left; }"
                          "QPushButton:focus { color: palette(highlight); text-decoration: underline; }");
    // Styles may give buttons a font of their own, which only a size set on the button itself overrides.
    auto font = QApplication::font("QLabel");
    if (font.pointSizeF() > 0)
        font.setPointSizeF(font.pointSizeF());
    else
        font.setPixelSize(font.pixelSize());
    button->setFont(font);

    auto update_text = [button, text](bool expanded) {
        // U+FE0E keeps the triangles in text presentation, where macOS would otherwise draw them as emoji.
        button->setText(QStringLiteral("%1︎ %2").arg(expanded ? QChar(0x25BC) : QChar(0x25B6), text));
    };
    update_text(false);
    content->hide();
    QObject::connect(button, &QPushButton::clicked, content, [content, update_text] {
        auto expanded = content->isHidden();
        update_text(expanded);
        content->setVisible(expanded);
    });
    return button;
}

// Items within a group sit close together, and the groups themselves further apart.
QVBoxLayout* create_group_layout(QWidget* group)
{
    auto* layout = new QVBoxLayout(group);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(item_spacing);
    return layout;
}

QFrame* create_details_panel(QWidget* parent)
{
    auto* details = new QFrame(parent);
    details->setObjectName("ReportDetails");
    auto* layout = create_group_layout(details);
    layout->setContentsMargins(panel_padding, panel_padding, panel_padding, panel_padding);
    return details;
}

QFormLayout* create_details_form()
{
    auto* form = new QFormLayout;
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
    form->setHorizontalSpacing(group_spacing / 2);
    form->setVerticalSpacing(item_spacing);
    return form;
}

void set_details(QFormLayout& form, ReadonlySpan<WebView::ReportDetail> details, QWidget* parent)
{
    while (form.rowCount() > 0)
        form.removeRow(0);

    for (auto const& detail : details) {
        // Labels stay on one line, so each value starts level with its label however far it wraps.
        auto* label = create_label(qstring_from_ak_string(detail.label), parent);
        label->setWordWrap(false);
        auto* value = create_label(qstring_from_ak_string(detail.value), parent);
        if (detail.is_code)
            value->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        form.addRow(label, value);
    }
}

// The full report is read in place of the review that offers to send it, exactly as it would be sent.
ReportTextPage create_report_text_page(QWidget* parent)
{
    auto* page = new QWidget(parent);
    auto* layout = create_group_layout(page);
    layout->addWidget(create_heading(QObject::tr("Full report"), 1, page));

    auto* text = new QPlainTextEdit(page);
    text->setObjectName("ReportText");
    text->setReadOnly(true);
    text->setLineWrapMode(QPlainTextEdit::NoWrap);
    text->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    text->setMinimumHeight(text->fontMetrics().lineSpacing() * 24);
    layout->addWidget(text);

    auto* back_button = new QPushButton(QObject::tr("Back"), page);
    back_button->setObjectName("ReportTextBackButton");
    back_button->setAutoDefault(false);
    layout->addSpacing(item_spacing);
    layout->addWidget(back_button, 0, Qt::AlignLeft);

    page->hide();
    return { page, text, back_button };
}

// Only the fields get a stronger placeholder color. The muted lines keep the color the screen around the review uses
// for its own.
void apply_style(QWidget& review, ReadonlySpan<QWidget*> fields)
{
    for (auto* field : fields) {
        auto field_palette = field->palette();
        field_palette.setColor(QPalette::PlaceholderText, ChromeStyle::report_review_placeholder_text(review.palette()));
        field->setPalette(field_palette);
    }
    review.setStyleSheet(ChromeStyle::report_review_style_sheet(review.palette()));
}

// Tab moves through every control of the review and wraps around within it. The platform's own focus chain would skip
// buttons where the system limits Tab to text fields, as macOS does by default, and lead into the web view behind a
// tab's crash screen, which keeps Tab for the page.
bool focus_next_control(QWidget& review, bool next)
{
    Vector<QWidget*> controls;
    for (auto* widget = review.nextInFocusChain(); widget != &review; widget = widget->nextInFocusChain()) {
        if (review.isAncestorOf(widget) && widget->isVisibleTo(&review) && widget->isEnabled() && (widget->focusPolicy() & Qt::TabFocus))
            controls.append(widget);
    }
    if (controls.is_empty())
        return false;

    auto current = controls.find_first_index(review.window()->focusWidget());
    size_t index = 0;
    if (current.has_value())
        index = next ? (*current + 1) % controls.size() : (*current + controls.size() - 1) % controls.size();
    else if (!next)
        index = controls.size() - 1;

    auto* control = controls[index];
    control->setFocus(next ? Qt::TabFocusReason : Qt::BacktabFocusReason);
    for (auto* ancestor = review.parentWidget(); ancestor; ancestor = ancestor->parentWidget()) {
        if (auto* scroll_area = qobject_cast<QScrollArea*>(ancestor)) {
            scroll_area->ensureWidgetVisible(control);
            break;
        }
    }
    return true;
}

// How far along a report is at each stage of sending it, which takes most of its time solving the proof of work.
int progress_percent(WebView::ReportSubmission::Stage stage)
{
    using Stage = WebView::ReportSubmission::Stage;

    switch (stage) {
    case Stage::Preparing:
        return 15;
    case Stage::RequestingChallenge:
        return 40;
    case Stage::SolvingProof:
        return 65;
    case Stage::Sending:
        return 85;
    }
    VERIFY_NOT_REACHED();
}

void show_actions(ReadonlySpan<QPushButton*> all_actions, ReadonlySpan<QPushButton*> actions, QPushButton* primary)
{
    for (auto* button : all_actions) {
        button->setVisible(actions.contains_slow(button));
        button->setDefault(button == primary);
    }
    if (auto* focused = primary ? primary : (actions.is_empty() ? nullptr : actions.first()))
        focused->setFocus(Qt::OtherFocusReason);
}

QString retry_message(String const& reason, u32 retry_number, u32 maximum_retries, u32 delay_seconds)
{
    return QObject::tr("%1 Trying again in %2 seconds (%3 of %4)…")
        .arg(qstring_from_ak_string(reason))
        .arg(delay_seconds)
        .arg(retry_number)
        .arg(maximum_retries);
}

}
