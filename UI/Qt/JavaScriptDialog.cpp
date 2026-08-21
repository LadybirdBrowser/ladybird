/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/JavaScriptDialog.h>

#include <AK/Platform.h>
#include <AK/StdLibExtras.h>
#include <QApplication>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFocusEvent>
#include <QFrame>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QShortcut>
#include <QShowEvent>
#include <QVBoxLayout>

namespace Ladybird {

static constexpr int DIALOG_MAX_WIDTH = 520;
static constexpr int DIALOG_MIN_WIDTH = 320;
static constexpr int DIALOG_MAX_MESSAGE_HEIGHT = 240;
static constexpr int DIALOG_MARGIN = 24;
static constexpr int PANEL_HORIZONTAL_PADDING = 20;

JavaScriptDialog::JavaScriptDialog(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("LadybirdJavaScriptDialogOverlay");
    setAttribute(Qt::WA_StyledBackground);
#if defined(AK_OS_MACOS) || defined(USE_DIRECTX) || defined(USE_VULKAN_DMABUF_IMAGES)
    // Native rendering surfaces stack above ordinary child widgets. Match their native stacking level so the dialog
    // remains above the page.
    setAttribute(Qt::WA_NativeWindow);
#endif
    setFocusPolicy(Qt::NoFocus);
    setAcceptDrops(true);
    setMouseTracking(true);
    setGeometry(parent->rect());
    parent->installEventFilter(this);
    QApplication::instance()->installEventFilter(this);

    auto* overlay_layout = new QVBoxLayout(this);
    overlay_layout->setContentsMargins(DIALOG_MARGIN, DIALOG_MARGIN, DIALOG_MARGIN, DIALOG_MARGIN);
    overlay_layout->addStretch();

    m_panel = new QFrame(this);
    m_panel->setObjectName("LadybirdJavaScriptDialogPanel");
    m_panel->setAttribute(Qt::WA_StyledBackground);
    m_panel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);

    auto* panel_layout = new QVBoxLayout(m_panel);
    panel_layout->setContentsMargins(PANEL_HORIZONTAL_PADDING, 18, PANEL_HORIZONTAL_PADDING, 18);
    panel_layout->setSpacing(12);

    m_title_label = new QLabel(m_panel);
    m_title_label->setObjectName("LadybirdJavaScriptDialogTitle");
    panel_layout->addWidget(m_title_label);

    m_message_scroll_area = new QScrollArea(m_panel);
    m_message_scroll_area->setObjectName("LadybirdJavaScriptDialogMessageArea");
    m_message_scroll_area->setFrameShape(QFrame::NoFrame);
    m_message_scroll_area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_message_scroll_area->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_message_scroll_area->setWidgetResizable(true);
    m_message_scroll_area->viewport()->setObjectName("LadybirdJavaScriptDialogMessageViewport");

    m_message_label = new QLabel(m_message_scroll_area);
    m_message_label->setObjectName("LadybirdJavaScriptDialogMessage");
    m_message_label->setTextFormat(Qt::PlainText);
    m_message_label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_message_label->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    m_message_label->setWordWrap(true);
    m_message_label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_message_scroll_area->setWidget(m_message_label);
    panel_layout->addWidget(m_message_scroll_area);

    m_prompt_text = new QLineEdit(m_panel);
    m_prompt_text->setObjectName("LadybirdJavaScriptDialogPrompt");
    panel_layout->addWidget(m_prompt_text);

    m_button_box = new QDialogButtonBox(m_panel);
    m_button_box->setObjectName("LadybirdJavaScriptDialogButtons");
    panel_layout->addWidget(m_button_box);

    overlay_layout->addWidget(m_panel, 0, Qt::AlignHCenter);
    overlay_layout->addStretch();

    QObject::connect(m_button_box, &QDialogButtonBox::accepted, this, [this] {
        accept();
    });
    QObject::connect(m_button_box, &QDialogButtonBox::rejected, this, [this] {
        dismiss();
    });
    QObject::connect(m_prompt_text, &QLineEdit::returnPressed, this, [this] {
        accept();
    });

    new QShortcut(QKeySequence(Qt::Key_Escape), this, [this] { dismiss(); }, Qt::WidgetWithChildrenShortcut);

    update_chrome_style();
    hide();
}

void JavaScriptDialog::show_alert(QString const& title, QString const& message)
{
    open(Type::Alert, title, message);
}

void JavaScriptDialog::show_confirm(QString const& title, QString const& message)
{
    open(Type::Confirm, title, message);
}

void JavaScriptDialog::show_prompt(QString const& title, QString const& message, QString const& default_text)
{
    open(Type::Prompt, title, message, default_text);
}

void JavaScriptDialog::open(Type type, QString const& title, QString const& message, QString const& default_text)
{
    if (m_type.has_value())
        return;
    VERIFY(!m_parent_focus_policy.has_value());

    m_previous_focus_widget = QApplication::focusWidget();
    m_parent_focus_policy = parentWidget()->focusPolicy();
    parentWidget()->setFocusPolicy(Qt::NoFocus);
    m_type = type;
    m_title_label->setText(title);
    m_message_label->setText(message);
    m_prompt_text->setVisible(type == Type::Prompt);
    m_prompt_text->setText(default_text);

    QDialogButtonBox::StandardButtons buttons { QDialogButtonBox::Ok };
    if (type != Type::Alert)
        buttons |= QDialogButtonBox::Cancel;
    m_button_box->setStandardButtons(buttons);

    for (auto* button : m_button_box->buttons()) {
        button->installEventFilter(this);
        if (auto* push_button = qobject_cast<QPushButton*>(button))
            push_button->setAutoDefault(type != Type::Prompt);
    }
    auto* ok_button = m_button_box->button(QDialogButtonBox::Ok);
    auto* cancel_button = m_button_box->button(QDialogButtonBox::Cancel);
    if (ok_button)
        ok_button->setDefault(type != Type::Prompt);

    if (type == Type::Prompt && ok_button)
        QWidget::setTabOrder(m_prompt_text, ok_button);
    if (ok_button && cancel_button)
        QWidget::setTabOrder(ok_button, cancel_button);

    update_geometry_constraints();
    show();
    raise();
    focus_dialog();

    if (type == Type::Prompt && ok_button)
        ok_button->setDefault(false);
}

void JavaScriptDialog::set_prompt_text(QString const& text)
{
    if (m_type != Type::Prompt)
        return;
    m_prompt_text->setText(text);
}

void JavaScriptDialog::accept()
{
    if (!m_type.has_value())
        return;
    complete(true);
}

void JavaScriptDialog::dismiss()
{
    if (!m_type.has_value())
        return;
    complete(m_type == Type::Alert);
}

void JavaScriptDialog::reset()
{
    if (!m_type.has_value())
        return;

    m_type.clear();
    restore_focus_after_close();
}

void JavaScriptDialog::complete(bool accepted)
{
    auto type = m_type.release_value();
    auto prompt_text = m_prompt_text->text();
    restore_focus_after_close();

    if (on_complete)
        on_complete(type, accepted, prompt_text);
}

void JavaScriptDialog::restore_focus_after_close()
{
    auto* focus_widget = QApplication::focusWidget();
    auto dialog_had_focus = focus_widget == this || isAncestorOf(focus_widget);
    hide();
    restore_parent_focus_policy();

    auto previous_focus_widget = m_previous_focus_widget;
    m_previous_focus_widget = nullptr;
    if (dialog_had_focus && previous_focus_widget && previous_focus_widget->isVisible())
        previous_focus_widget->setFocus();
}

bool JavaScriptDialog::event(QEvent* event)
{
    if (event->type() == QEvent::PaletteChange)
        update_chrome_style();

    switch (event->type()) {
    case QEvent::ContextMenu:
    case QEvent::DragEnter:
    case QEvent::DragLeave:
    case QEvent::DragMove:
    case QEvent::Drop:
    case QEvent::KeyPress:
    case QEvent::KeyRelease:
    case QEvent::MouseButtonDblClick:
    case QEvent::MouseButtonPress:
    case QEvent::MouseButtonRelease:
    case QEvent::MouseMove:
    case QEvent::NativeGesture:
    case QEvent::TabletMove:
    case QEvent::TabletPress:
    case QEvent::TabletRelease:
    case QEvent::TouchBegin:
    case QEvent::TouchCancel:
    case QEvent::TouchEnd:
    case QEvent::TouchUpdate:
    case QEvent::Wheel:
        event->accept();
        return true;
    default:
        break;
    }

    return QWidget::event(event);
}

bool JavaScriptDialog::eventFilter(QObject* object, QEvent* event)
{
    if (object == parentWidget() && event->type() == QEvent::Resize)
        setGeometry(parentWidget()->rect());

    if (m_type.has_value() && event->type() == QEvent::FocusIn) {
        auto* widget = qobject_cast<QWidget*>(object);
        if (widget && is_blocked_content_widget(*widget)) {
            auto reason = static_cast<QFocusEvent*>(event)->reason();
            if (reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason)
                focusNextPrevChild(reason == Qt::TabFocusReason);
            else
                focus_dialog();
            return true;
        }
    }

    if (m_type.has_value() && event->type() == QEvent::KeyPress) {
        auto* button = qobject_cast<QPushButton*>(object);
        if (button && is_dialog_widget(*button) && m_button_box->buttons().contains(button)) {
            auto& key_event = static_cast<QKeyEvent&>(*event);
            switch (key_event.key()) {
            case Qt::Key_Enter:
            case Qt::Key_Return:
                button->click();
                return true;
            case Qt::Key_Down:
            case Qt::Key_Right:
                return focusNextPrevChild(true);
            case Qt::Key_Left:
            case Qt::Key_Up:
                return focusNextPrevChild(false);
            default:
                break;
            }
        }
    }

    return QWidget::eventFilter(object, event);
}

bool JavaScriptDialog::focusNextPrevChild(bool next)
{
    auto* current = QApplication::focusWidget();
    if (!current)
        return true;

    auto* candidate = next ? current->nextInFocusChain() : current->previousInFocusChain();
    while (candidate != current) {
        if (candidate->isVisible() && candidate->isEnabled() && (candidate->focusPolicy() & Qt::TabFocus) && !is_blocked_content_widget(*candidate)) {
            candidate->setFocus(next ? Qt::TabFocusReason : Qt::BacktabFocusReason);
            return true;
        }
        candidate = next ? candidate->nextInFocusChain() : candidate->previousInFocusChain();
    }

    return true;
}

bool JavaScriptDialog::is_dialog_widget(QWidget const& widget) const
{
    return &widget == this || isAncestorOf(&widget);
}

bool JavaScriptDialog::is_blocked_content_widget(QWidget const& widget) const
{
    return !is_dialog_widget(widget) && (&widget == parentWidget() || parentWidget()->isAncestorOf(&widget));
}

void JavaScriptDialog::restore_parent_focus_policy()
{
    if (!m_parent_focus_policy.has_value())
        return;
    parentWidget()->setFocusPolicy(m_parent_focus_policy.release_value());
}

void JavaScriptDialog::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    update_geometry_constraints();
}

void JavaScriptDialog::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    raise();
    focus_dialog();
}

void JavaScriptDialog::focus_dialog()
{
    if (!m_type.has_value())
        return;

    if (m_type == Type::Prompt) {
        m_prompt_text->setFocus(Qt::OtherFocusReason);
        m_prompt_text->selectAll();
        return;
    }

    if (auto* ok_button = m_button_box->button(QDialogButtonBox::Ok))
        ok_button->setFocus(Qt::OtherFocusReason);
}

void JavaScriptDialog::update_chrome_style()
{
    if (m_is_updating_chrome_style)
        return;

    m_is_updating_chrome_style = true;
    setStyleSheet(ChromeStyle::javascript_dialog_style_sheet(palette()));
    m_is_updating_chrome_style = false;
}

void JavaScriptDialog::update_geometry_constraints()
{
    auto available_width = max(0, width() - DIALOG_MARGIN * 2);
    m_panel->setMinimumWidth(min(DIALOG_MIN_WIDTH, available_width));
    m_panel->setMaximumWidth(min(DIALOG_MAX_WIDTH, available_width));

    auto message_width = max(1, min(DIALOG_MAX_WIDTH - PANEL_HORIZONTAL_PADDING * 2, available_width - PANEL_HORIZONTAL_PADDING * 2));
    auto desired_message_height = max(0, m_message_label->heightForWidth(message_width));
    m_message_label->setMinimumHeight(desired_message_height);

    // Exclude the message area while measuring the height required by the other dialog controls.
    m_message_scroll_area->setFixedHeight(0);
    auto reserved_height = m_panel->sizeHint().height();
    auto available_message_height = max(0, height() - DIALOG_MARGIN * 2 - reserved_height);
    m_message_scroll_area->setFixedHeight(min(desired_message_height, min(DIALOG_MAX_MESSAGE_HEIGHT, available_message_height)));
}

}
