/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/Icon.h>
#include <UI/Qt/JavaScriptDialog.h>
#include <UI/Qt/Motion.h>
#include <UI/Qt/NativeTextEditingLineEdit.h>
#include <UI/Qt/Popover.h>
#include <UI/Qt/SystemAppearance.h>
#include <UI/Qt/Theme.h>
#if defined(AK_OS_MACOS)
#    include <UI/Qt/MacWindow.h>
#endif

#include <AK/Platform.h>
#include <AK/StdLibExtras.h>
#include <QApplication>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFocusEvent>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QShortcut>
#include <QShowEvent>
#include <QVBoxLayout>
#include <QVariantAnimation>

namespace Ladybird {

// The dialog is a sheet that hangs from the top of the page, over a dimmed page.
static constexpr int DIALOG_WIDTH = 440;
static constexpr int DIALOG_MAX_MESSAGE_HEIGHT = 240;
static constexpr int DIALOG_MARGIN = 24;
static constexpr int PANEL_HORIZONTAL_PADDING = 20;
#if defined(AK_OS_MACOS)
static constexpr int PANEL_CORNER_RADIUS = 12;
#endif
static constexpr int OPENING_SLIDE_DISTANCE = 12;

JavaScriptDialog::JavaScriptDialog(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("LadybirdJavaScriptDialogOverlay");
    setCursor(Qt::ArrowCursor);
    setAttribute(Qt::WA_StyledBackground);
#if defined(AK_OS_MACOS)
    // NB: The page is presented above anything Qt paints over it, and a native view over it cannot let it show through.
    //     So only the sheet is a native view, with its shadow cast by another, and the page view dims the page itself.
    //     This overlay only takes the input meant for the page.
#elif defined(USE_DIRECTX) || defined(USE_VULKAN_DMABUF_IMAGES)
    // Native rendering surfaces stack above ordinary child widgets. Match their native stacking level so the dialog
    // remains above the page.
    setAttribute(Qt::WA_DontCreateNativeAncestors);
    setAttribute(Qt::WA_NativeWindow);
#endif
    setFocusPolicy(Qt::NoFocus);
    setAcceptDrops(true);
    setMouseTracking(true);
    setGeometry(parent->rect());
    parent->installEventFilter(this);
    QApplication::instance()->installEventFilter(this);

    auto* overlay_layout = new QVBoxLayout(this);
    overlay_layout->setContentsMargins(DIALOG_MARGIN, 0, DIALOG_MARGIN, DIALOG_MARGIN);

    m_panel = new QFrame(this);
    m_panel->setObjectName("LadybirdJavaScriptDialogPanel");
    m_panel->setAttribute(Qt::WA_StyledBackground);
    m_panel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
#if defined(AK_OS_MACOS)
    m_panel->setAttribute(Qt::WA_DontCreateNativeAncestors);
    m_panel->setAttribute(Qt::WA_NativeWindow);
    m_panel_floating_shadow = &cast_floating_shadow(*m_panel, PANEL_CORNER_RADIUS, FloatingShadowTone::FollowTheme, FloatingShadowKind::Sheet);
#else
    m_panel_shadow = new QGraphicsDropShadowEffect(m_panel);
    m_panel_shadow->setBlurRadius(ChromeStyle::drop_shadow_blur_radius(40));
    m_panel_shadow->setOffset(0, 18);
    m_panel->setGraphicsEffect(m_panel_shadow);
#endif

    auto* panel_layout = new QVBoxLayout(m_panel);
    panel_layout->setContentsMargins(PANEL_HORIZONTAL_PADDING, 18, PANEL_HORIZONTAL_PADDING, 16);
    panel_layout->setSpacing(12);

    // The site's icon, then who is asking.
    auto* title_layout = new QHBoxLayout;
    title_layout->setSpacing(8);
    m_title_icon = new QLabel(m_panel);
    m_title_icon->setFixedSize(16, 16);
    title_layout->addWidget(m_title_icon);
    m_title_label = new QLabel(m_panel);
    m_title_label->setObjectName("LadybirdJavaScriptDialogTitle");
    title_layout->addWidget(m_title_label, 1);
    panel_layout->addLayout(title_layout);

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

    m_prompt_text = new NativeTextEditingLineEdit(m_panel);
    m_prompt_text->setObjectName("LadybirdJavaScriptDialogPrompt");
    panel_layout->addWidget(m_prompt_text);

    m_block_further_dialogs = new PopoverCheckBox("Don't let this site show more dialogs", m_panel);
    m_block_further_dialogs->setObjectName("LadybirdJavaScriptDialogBlockFurtherDialogs");
    panel_layout->addWidget(m_block_further_dialogs);

    m_button_box = new QDialogButtonBox(m_panel);
    m_button_box->setObjectName("LadybirdJavaScriptDialogButtons");
    panel_layout->addWidget(m_button_box);

    overlay_layout->addWidget(m_panel, 0, Qt::AlignHCenter | Qt::AlignTop);
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

void JavaScriptDialog::show_alert(QString const& title, QString const& message, OfferToBlockFurtherDialogs offer_to_block_further_dialogs)
{
    open(Type::Alert, title, message, {}, offer_to_block_further_dialogs);
}

void JavaScriptDialog::show_before_unload(QString const& title)
{
    open(Type::BeforeUnload, title, "Leave this page? Your changes may not be saved.");
}

void JavaScriptDialog::show_confirm(QString const& title, QString const& message, OfferToBlockFurtherDialogs offer_to_block_further_dialogs)
{
    open(Type::Confirm, title, message, {}, offer_to_block_further_dialogs);
}

void JavaScriptDialog::show_prompt(QString const& title, QString const& message, QString const& default_text, OfferToBlockFurtherDialogs offer_to_block_further_dialogs)
{
    open(Type::Prompt, title, message, default_text, offer_to_block_further_dialogs);
}

void JavaScriptDialog::set_title_icon(QIcon const& icon)
{
    // Like the tab strip, a site without an icon gets the globe.
    auto const& shown_icon = icon.isNull() ? create_chrome_icon(ChromeIcon::Globe, Theme::panel().muted) : icon;
    m_title_icon->setPixmap(shown_icon.pixmap(QSize(16, 16), devicePixelRatioF()));
}

bool JavaScriptDialog::blocks_further_dialogs() const
{
    return !m_block_further_dialogs->isHidden() && m_block_further_dialogs->isChecked();
}

void JavaScriptDialog::open(Type type, QString const& title, QString const& message, QString const& default_text, OfferToBlockFurtherDialogs offer_to_block_further_dialogs)
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
    m_block_further_dialogs->setChecked(false);
    m_block_further_dialogs->setVisible(offer_to_block_further_dialogs == OfferToBlockFurtherDialogs::Yes);

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
    if (type == Type::BeforeUnload) {
        VERIFY(ok_button);
        VERIFY(cancel_button);
        ok_button->setText("Leave");
        cancel_button->setText("Stay");
    }
    if (ok_button)
        ok_button->setDefault(type != Type::Prompt);

    if (type == Type::Prompt && ok_button)
        QWidget::setTabOrder(m_prompt_text, ok_button);
    if (ok_button && cancel_button)
        QWidget::setTabOrder(ok_button, cancel_button);

    // NB: A hidden native widget's platform window keeps the size it was created with (often Qt's default 100x30, from
    //     before the view was laid out) and reports that back as a resize, undoing the geometry the event filter set.
    //     So take the parent's rect again here, before laying out the panel.
    setGeometry(parentWidget()->rect());
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
    restore_parent_focus_policy();

    auto previous_focus_widget = m_previous_focus_widget;
    m_previous_focus_widget = nullptr;
    // NB: The focus moves before the dialog hides, since hiding it would hand the focus to the next widget in the chain.
    //     Without a widget to return to, the page the dialog came from takes the focus.
    if (dialog_had_focus) {
        if (previous_focus_widget && previous_focus_widget->isVisible())
            previous_focus_widget->setFocus();
        else if (parentWidget()->focusPolicy() != Qt::NoFocus)
            parentWidget()->setFocus();
    }
    hide();
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

#if defined(AK_OS_MACOS)
    round_appkit_view_bottom_corners(*m_panel, PANEL_CORNER_RADIUS);
#endif

    // The sheet slides 12 px down from the toolbar as it fades in, over the page as it dims. It only fades in if the
    // system asks for less motion.
    if (!m_opening_animation) {
        m_opening_animation = new QVariantAnimation(this);
        m_opening_animation->setDuration(Motion::BASE);
        m_opening_animation->setEasingCurve(Motion::enter_easing());
        m_opening_animation->setStartValue(0.0);
        m_opening_animation->setEndValue(1.0);
        QObject::connect(m_opening_animation, &QVariantAnimation::valueChanged, this, [this](QVariant const& value) {
            set_opening_progress(value.toReal());
        });
    }

    update_geometry_constraints();
    raise();
    set_opening_progress(0);
    m_opening_animation->stop();
    m_opening_animation->start();
    focus_dialog();
}

void JavaScriptDialog::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);
    if (m_opening_animation)
        m_opening_animation->stop();
    if (on_page_dim_change)
        on_page_dim_change(0);
}

void JavaScriptDialog::set_opening_progress(qreal progress)
{
    // NB: The sheet is placed by the layout, and slides from there.
    layout()->activate();
    auto offset = SystemAppearance::prefers_reduced_motion() ? 0 : qRound(-OPENING_SLIDE_DISTANCE * (1 - progress));
    m_panel->move(m_panel->x(), layout()->contentsMargins().top() + offset);

#if defined(AK_OS_MACOS)
    set_appkit_view_opacity(*m_panel, progress);
    set_floating_window_opacity(*m_panel_floating_shadow, progress);
    if (on_page_dim_change)
        on_page_dim_change(progress);
#else
    if (progress >= 1.0) {
        setGraphicsEffect(nullptr);
    } else {
        auto* effect = qobject_cast<QGraphicsOpacityEffect*>(graphicsEffect());
        if (!effect) {
            effect = new QGraphicsOpacityEffect(this);
            setGraphicsEffect(effect);
        }
        effect->setOpacity(progress);
    }
#endif
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
#if !defined(AK_OS_MACOS)
    if (m_panel_shadow)
        m_panel_shadow->setColor(QColor(0, 0, 0, Theme::is_dark() ? 140 : 56));
#endif
    m_is_updating_chrome_style = false;
}

void JavaScriptDialog::update_geometry_constraints()
{
    // The panel is as wide as the design's, unless the view is too narrow for it.
    auto panel_width = min(DIALOG_WIDTH, max(0, width() - DIALOG_MARGIN * 2));
    m_panel->setFixedWidth(panel_width);

    auto message_width = max(1, panel_width - PANEL_HORIZONTAL_PADDING * 2);
    auto desired_message_height = max(0, m_message_label->heightForWidth(message_width));
    m_message_label->setMinimumHeight(desired_message_height);

    // Exclude the message area while measuring the height required by the other dialog controls.
    m_message_scroll_area->setFixedHeight(0);
    auto reserved_height = m_panel->sizeHint().height();
    auto available_message_height = max(0, height() - DIALOG_MARGIN * 2 - reserved_height);
    m_message_scroll_area->setFixedHeight(min(desired_message_height, min(DIALOG_MAX_MESSAGE_HEIGHT, available_message_height)));
}

}
