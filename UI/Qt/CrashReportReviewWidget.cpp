/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/CrashReportReviewWidget.h>
#include <UI/Qt/StringUtils.h>

#include <QApplication>
#include <QCheckBox>
#include <QEvent>
#include <QFontDatabase>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QStyle>
#include <QVBoxLayout>

namespace Ladybird {

static constexpr int PANEL_PADDING = 16;
static constexpr int BUTTON_SPACING = 12;

static QLabel* create_label(QString const& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

static QLabel* create_heading(QString const& text, qreal scale, QWidget* parent)
{
    auto* label = create_label(text, parent);
    auto font = label->font();
    font.setPointSizeF(font.pointSizeF() * scale);
    font.setBold(true);
    label->setFont(font);
    return label;
}

// A button that shows or hides the content below it, which starts out hidden. It is drawn as plain text, flush with the
// text around it, as styles frame buttons on hover and press and indent them. A triangle in the same font shows whether
// the content is shown, as styles size arrows to the icon size. Keyboard focus shows in the accent color.
static QPushButton* create_disclosure(QString const& text, QWidget* content, QWidget* parent)
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
        button->setText(QStringLiteral("%1\uFE0E %2").arg(expanded ? QChar(0x25BC) : QChar(0x25B6), text));
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
static QVBoxLayout* create_group_layout(QWidget* group)
{
    auto* layout = new QVBoxLayout(group);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(CrashReportReviewWidget::item_spacing);
    return layout;
}

static void clear_form(QFormLayout& form)
{
    while (form.rowCount() > 0)
        form.removeRow(0);
}

static void add_field(QFormLayout& form, WebView::CrashReportReview::Field const& field, QWidget* parent)
{
    // Labels stay on one line, so each value starts level with its label however far it wraps.
    auto* label = create_label(qstring_from_ak_string(field.label), parent);
    label->setWordWrap(false);
    auto* value = create_label(qstring_from_ak_string(field.value), parent);
    if (field.is_code)
        value->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
    form.addRow(label, value);
}

QLabel* CrashReportReviewWidget::create_title(QString const& text, QWidget* parent)
{
    return create_heading(text, 1.5, parent);
}

CrashReportReviewWidget::CrashReportReviewWidget(QString exit_text, QWidget* parent, WebView::CrashReportStore& store)
    : QWidget(parent)
    , m_review(store)
{
    setObjectName("LadybirdCrashReportReview");

    // Only the page on screen is laid out, so the widget is only as tall as the page it shows.
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    m_review_page = new QWidget(this);
    auto* review_layout = new QVBoxLayout(m_review_page);
    review_layout->setContentsMargins(0, 0, 0, 0);
    review_layout->setSpacing(CrashReportReviewWidget::group_spacing);

    auto* description = new QWidget(m_review_page);
    auto* description_layout = create_group_layout(description);
    description_layout->addWidget(create_label(tr("What were you doing when it crashed? (optional)"), description));
    m_description_edit = new QPlainTextEdit(description);
    m_description_edit->setObjectName("CrashReportDescription");
    m_description_edit->setPlaceholderText(tr("A few words about what you were doing can help us reproduce it."));
    m_description_edit->setTabChangesFocus(true);
    m_description_edit->setFixedHeight(m_description_edit->fontMetrics().lineSpacing() * 5);
    description_layout->addWidget(m_description_edit);
    review_layout->addWidget(description);

    m_website_option = new QWidget(m_review_page);
    auto* website_layout = create_group_layout(m_website_option);
    m_include_website = new QCheckBox(tr("Include this website URL in the report"), m_website_option);
    m_include_website->setObjectName("CrashReportIncludeWebsite");
    m_website = new QLineEdit(m_website_option);
    m_website->setObjectName("CrashReportWebsite");
    m_website->setMaxLength(WebView::CrashReportReview::maximum_url_bytes);
    m_website->setEnabled(false);
    QObject::connect(m_include_website, &QCheckBox::toggled, m_website, &QLineEdit::setEnabled);
    // The URL lines up with the checkbox's label, under the option it belongs to.
    auto website_indent = style()->pixelMetric(QStyle::PM_IndicatorWidth, nullptr, m_include_website)
        + style()->pixelMetric(QStyle::PM_CheckBoxLabelSpacing, nullptr, m_include_website);
    auto* website_row = new QHBoxLayout;
    website_row->addSpacing(website_indent);
    website_row->addWidget(m_website);
    website_layout->addWidget(m_include_website);
    website_layout->addLayout(website_row);
    review_layout->addWidget(m_website_option);

    auto* contents = new QWidget(m_review_page);
    auto* contents_layout = create_group_layout(contents);
    auto* contents_summary = create_label(tr("Includes technical crash details, your Ladybird version and platform."), contents);
    contents_summary->setForegroundRole(QPalette::PlaceholderText);
    contents_layout->addWidget(contents_summary);
    m_details = new QFrame(contents);
    m_details->setObjectName("CrashReportDetails");
    auto* details_layout = create_group_layout(m_details);
    details_layout->setContentsMargins(PANEL_PADDING, PANEL_PADDING, PANEL_PADDING, PANEL_PADDING);
    m_fields = new QFormLayout;
    m_fields->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    m_fields->setLabelAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_fields->setHorizontalSpacing(CrashReportReviewWidget::group_spacing / 2);
    m_fields->setVerticalSpacing(CrashReportReviewWidget::item_spacing);
    details_layout->addLayout(m_fields);
    details_layout->addSpacing(CrashReportReviewWidget::item_spacing);
    auto* show_report_button = new QPushButton(tr("Open full report (.txt)"), m_details);
    show_report_button->setAutoDefault(false);
    show_report_button->setObjectName("CrashReportShowReportButton");
    QObject::connect(show_report_button, &QPushButton::clicked, this, [this] {
        if (auto result = m_review.show_report(); result.is_error())
            warnln("Could not open the crash report: {}", result.error());
    });
    details_layout->addWidget(show_report_button, 0, Qt::AlignLeft);
    auto* details_toggle = create_disclosure(tr("Report details"), m_details, contents);
    // The toggle comes before what it shows, although it is created after it.
    QWidget::setTabOrder(details_toggle, show_report_button);
    contents_layout->addWidget(details_toggle);
    contents_layout->addWidget(m_details);
    review_layout->addWidget(contents);

    m_validation = create_label({}, m_review_page);
    m_validation->setObjectName("CrashReportValidation");
    m_validation->hide();
    review_layout->addWidget(m_validation);

    auto* send_button = new QPushButton(tr("Send report"), m_review_page);
    send_button->setObjectName("CrashReportSendButton");
    send_button->setDefault(true);
    auto* decline_button = new QPushButton(tr("Don’t send"), m_review_page);
    decline_button->setObjectName("CrashReportDeclineButton");
    auto* review_buttons = new QHBoxLayout;
    review_buttons->setSpacing(BUTTON_SPACING);
    review_buttons->addWidget(send_button);
    review_buttons->addWidget(decline_button);
    review_buttons->addStretch();
    auto* review_exit_button = new QPushButton(exit_text, m_review_page);
    review_exit_button->setObjectName("CrashReportReviewExitButton");
    review_buttons->addWidget(review_exit_button);
    // Keyboard focus starts on leaving, so a stray key press never sends a report.
    setFocusProxy(review_exit_button);
    review_layout->addLayout(review_buttons);
    review_layout->addStretch();
    layout->addWidget(m_review_page);

    m_status_page = new QWidget(this);
    auto* status_layout = new QVBoxLayout(m_status_page);
    status_layout->setContentsMargins(0, 0, 0, 0);
    status_layout->setSpacing(CrashReportReviewWidget::group_spacing);

    // The outcome is a section of the crash screen rather than a headline of its own.
    auto* outcome = new QWidget(m_status_page);
    auto* outcome_layout = create_group_layout(outcome);
    m_status_title = create_heading({}, 1, outcome);
    m_status_title->setObjectName("CrashReportStatusTitle");
    outcome_layout->addWidget(m_status_title);
    m_progress = new QProgressBar(outcome);
    m_progress->setObjectName("CrashReportProgress");
    m_progress->setRange(0, 100);
    m_progress->setTextVisible(false);
    outcome_layout->addWidget(m_progress);
    m_status_message = create_label({}, outcome);
    m_status_message->setObjectName("CrashReportStatusMessage");
    m_status_message->setForegroundRole(QPalette::PlaceholderText);
    outcome_layout->addWidget(m_status_message);
    status_layout->addWidget(outcome);

    m_retry_button = new QPushButton(tr("Try sending again"), m_status_page);
    m_retry_button->setObjectName("CrashReportRetryButton");
    m_next_button = new QPushButton(tr("Review the next crash report"), m_status_page);
    m_next_button->setObjectName("CrashReportNextButton");
    m_exit_button = new QPushButton(exit_text, m_status_page);
    m_exit_button->setObjectName("CrashReportExitButton");
    auto* status_buttons = new QHBoxLayout;
    status_buttons->setSpacing(BUTTON_SPACING);
    status_buttons->addWidget(m_retry_button);
    status_buttons->addWidget(m_next_button);
    status_buttons->addWidget(m_exit_button);
    status_buttons->addStretch();
    status_layout->addLayout(status_buttons);
    status_layout->addStretch();
    m_status_page->hide();
    layout->addWidget(m_status_page);

    QObject::connect(send_button, &QPushButton::clicked, this, [this] { send(); });
    QObject::connect(decline_button, &QPushButton::clicked, this, [this] {
        show_declined();
        if (on_answered)
            on_answered();
    });
    QObject::connect(m_retry_button, &QPushButton::clicked, this, [this] {
        show_progress(WebView::CrashReportSubmission::Stage::Preparing);
        VERIFY(!m_review.send(m_description, m_url).has_value());
    });
    QObject::connect(m_next_button, &QPushButton::clicked, this, [this] {
        if (auto result = open_report(); result.is_error()) {
            warnln("Could not open the next crash report: {}", result.error());
            if (on_exit)
                on_exit();
        }
    });
    for (auto* button : { review_exit_button, m_exit_button }) {
        QObject::connect(button, &QPushButton::clicked, this, [this] {
            if (on_exit)
                on_exit();
        });
    }

    m_review.on_progress = [this](auto stage) { show_progress(stage); };
    m_review.on_retry = [this](String const& reason, u32 retry_number, u32 maximum_retries, u32 delay_seconds) {
        show_retry(reason, retry_number, maximum_retries, delay_seconds);
    };
    m_review.on_sent = [this] { show_sent(); };
    m_review.on_failed = [this](auto failure, String const& reason) { show_failure(failure, reason); };

    update_chrome_style();
}

// Tab moves through every control of the review and wraps around within it. The platform's own focus chain would skip
// buttons where the system limits Tab to text fields, as macOS does by default, and lead into the web view behind a
// tab's crash screen, which keeps Tab for the page.
bool CrashReportReviewWidget::focusNextPrevChild(bool next)
{
    Vector<QWidget*> controls;
    for (auto* widget = nextInFocusChain(); widget != this; widget = widget->nextInFocusChain()) {
        if (isAncestorOf(widget) && widget->isVisibleTo(this) && widget->isEnabled() && (widget->focusPolicy() & Qt::TabFocus))
            controls.append(widget);
    }
    if (controls.is_empty())
        return QWidget::focusNextPrevChild(next);

    auto current = controls.find_first_index(window()->focusWidget());
    size_t index = 0;
    if (current.has_value())
        index = next ? (*current + 1) % controls.size() : (*current + controls.size() - 1) % controls.size();
    else if (!next)
        index = controls.size() - 1;

    auto* control = controls[index];
    control->setFocus(next ? Qt::TabFocusReason : Qt::BacktabFocusReason);
    for (auto* ancestor = parentWidget(); ancestor; ancestor = ancestor->parentWidget()) {
        if (auto* scroll_area = qobject_cast<QScrollArea*>(ancestor)) {
            scroll_area->ensureWidgetVisible(control);
            break;
        }
    }
    return true;
}

bool CrashReportReviewWidget::event(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange)
        update_chrome_style();
    return QWidget::event(event);
}

void CrashReportReviewWidget::update_chrome_style()
{
    // Restyling changes the palette, which would bring us back here.
    if (m_is_updating_chrome_style)
        return;
    m_is_updating_chrome_style = true;

    // Only the fields get a stronger placeholder color. The muted lines keep the color the crash screen uses for its own.
    for (QWidget* field : { static_cast<QWidget*>(m_description_edit), static_cast<QWidget*>(m_website) }) {
        auto field_palette = field->palette();
        field_palette.setColor(QPalette::PlaceholderText, ChromeStyle::crash_report_review_placeholder_text(palette()));
        field->setPalette(field_palette);
    }
    setStyleSheet(ChromeStyle::crash_report_review_style_sheet(palette()));

    m_is_updating_chrome_style = false;
}

ErrorOr<void> CrashReportReviewWidget::open_report(Optional<ByteString> const& name, Optional<String> const& website)
{
    auto report = TRY(m_review.open(name));

    clear_form(*m_fields);
    for (auto const& field : report.fields)
        add_field(*m_fields, field, m_details);

    // Without a website to offer, the option has nothing to include.
    m_website_option->setVisible(website.has_value());
    m_website->setText(website.has_value() ? qstring_from_ak_string(*website) : QString {});
    m_include_website->setChecked(false);
    m_website->setEnabled(false);

    m_description_edit->clear();
    m_validation->hide();
    show_page(*m_review_page);
    return {};
}

void CrashReportReviewWidget::send()
{
    m_description = ak_string_from_qstring(m_description_edit->toPlainText());
    m_url.clear();
    if (m_include_website->isChecked()) {
        m_website->setText(m_website->text().trimmed());
        m_url = ak_string_from_qstring(m_website->text());
    }

    Optional<StringView> url;
    if (m_url.has_value())
        url = m_url->bytes_as_string_view();
    // Checked before anything changes on screen, as the review checks the same when it sends.
    if (auto error = WebView::CrashReportReview::validate(m_description, url); error.has_value()) {
        m_validation->setText(qstring_from_ak_string(*error));
        m_validation->show();
        return;
    }

    m_validation->hide();
    show_progress(WebView::CrashReportSubmission::Stage::Preparing);
    if (on_answered)
        on_answered();
    VERIFY(!m_review.send(m_description, m_url).has_value());
}

void CrashReportReviewWidget::show_page(QWidget& page)
{
    m_review_page->setVisible(&page == m_review_page);
    m_status_page->setVisible(&page == m_status_page);
}

void CrashReportReviewWidget::show_status(QString const& title, QString const& message,
    Vector<QPushButton*> const& actions, QPushButton* primary)
{
    m_status_title->setText(title);
    m_status_message->setText(message);
    for (auto* button : { m_retry_button, m_next_button, m_exit_button }) {
        button->setVisible(actions.contains_slow(button));
        button->setDefault(button == primary);
    }
    show_page(*m_status_page);
    if (primary)
        primary->setFocus(Qt::OtherFocusReason);
}

void CrashReportReviewWidget::show_progress(WebView::CrashReportSubmission::Stage stage)
{
    using Stage = WebView::CrashReportSubmission::Stage;

    auto percent = [&] {
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
    }();

    // Leaving stays possible while the report is on its way, which carries on behind the screen that follows.
    show_status(tr("Sending crash report"), stage == Stage::Sending ? tr("Sending crash report…") : tr("Preparing report…"),
        { m_exit_button });
    m_progress->setValue(percent);
    m_progress->show();
}

void CrashReportReviewWidget::show_retry(String const& reason, u32 retry_number, u32 maximum_retries, u32 delay_seconds)
{
    m_status_message->setText(tr("%1 Trying again in %2 seconds (%3 of %4)…")
            .arg(qstring_from_ak_string(reason))
            .arg(delay_seconds)
            .arg(retry_number)
            .arg(maximum_retries));
}

void CrashReportReviewWidget::show_sent()
{
    m_progress->setValue(100);
    show_outcome(tr("Report sent"), tr("Thank you for helping Ladybird."));
}

void CrashReportReviewWidget::show_declined()
{
    m_progress->hide();
    show_outcome(tr("Report not sent"), tr("This report stays on your device and is not sent."));
}

void CrashReportReviewWidget::show_failure(WebView::CrashReportSubmission::Failure failure, String const& reason)
{
    m_progress->hide();

    // Only sending can succeed on another attempt. A report that could not be prepared would fail the same way again.
    if (failure == WebView::CrashReportSubmission::Failure::Sending) {
        show_status(tr("Couldn’t send report"), qstring_from_ak_string(reason), { m_retry_button, m_exit_button },
            m_retry_button);
        return;
    }
    show_outcome(tr("Couldn’t send report"), qstring_from_ak_string(reason));
}

// Once the report is answered, the review moves on to the next one waiting, or is left.
void CrashReportReviewWidget::show_outcome(QString const& title, QString const& message)
{
    if (m_review.has_next_report())
        show_status(title, message, { m_next_button, m_exit_button }, m_next_button);
    else
        show_status(title, message, { m_exit_button }, m_exit_button);
}

}
