/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/PasskeyPopover.h>
#include <UI/Qt/StringUtils.h>
#include <UI/Qt/SystemAppearance.h>
#include <UI/Qt/Theme.h>

#include <QApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>
#include <QVariantAnimation>

namespace Ladybird {

static constexpr int PASSKEY_PROMPT_WIDTH = 352;
static constexpr int PASSKEY_PROMPT_MARGIN = 16;
// The width that the text of an option has: the card without its margins, the padding of the row and its icon.
// NB: A list with more options than this scrolls, so that the popover fits in the window. The last row that shows is
//     cut off, which tells the user that there are more.
static constexpr size_t PASSKEY_OPTIONS_SHOWN_AT_ONCE = 6;
static constexpr int PASSKEY_OPTION_ROW_HEIGHT = 38;
static constexpr int PASSKEY_OPTION_TEXT_WIDTH = PASSKEY_PROMPT_WIDTH - 2 * PASSKEY_PROMPT_MARGIN - 64;
// The shortest PIN that a security key takes.
static constexpr int SHORTEST_SECURITY_KEY_PIN = 4;

// A security key with rings that spread out from it, while the browser waits for the user to touch theirs.
class TouchIndicator final : public QWidget {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit TouchIndicator(QWidget* parent)
        : QWidget(parent)
    {
        setFixedHeight(2 * (DISC_RADIUS + RING_TRAVEL) + 8);

        m_animation = new QVariantAnimation(this);
        m_animation->setStartValue(0.0);
        m_animation->setEndValue(1.0);
        m_animation->setDuration(1800);
        m_animation->setLoopCount(-1);
        QObject::connect(m_animation, &QVariantAnimation::valueChanged, this, [this](QVariant const& value) {
            m_progress = value.toReal();
            update();
        });
    }

private:
    static constexpr int DISC_RADIUS = 24;
    static constexpr int RING_TRAVEL = 14;
    static constexpr int ICON_SIZE = 24;

    virtual void showEvent(QShowEvent* event) override
    {
        QWidget::showEvent(event);
        if (!SystemAppearance::prefers_reduced_motion())
            m_animation->start();
    }

    virtual void hideEvent(QHideEvent* event) override
    {
        QWidget::hideEvent(event);
        m_animation->stop();
    }

    virtual void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);

        auto const& panel = Theme::panel();
        auto center = QRectF(rect()).center();

        auto paint_ring = [&](qreal travel) {
            auto color = panel.link;
            color.setAlphaF(0.45 * (1 - travel));
            painter.setPen(QPen(color, 1.5));
            painter.setBrush(Qt::NoBrush);
            auto radius = DISC_RADIUS + RING_TRAVEL * travel;
            painter.drawEllipse(center, radius, radius);
        };
        if (m_animation->state() == QAbstractAnimation::Running) {
            // Two rings, half a turn apart.
            paint_ring(m_progress);
            paint_ring(m_progress < 0.5 ? m_progress + 0.5 : m_progress - 0.5);
        } else {
            paint_ring(0.4);
        }

        painter.setPen(Qt::NoPen);
        painter.setBrush(panel.information);
        painter.drawEllipse(center, DISC_RADIUS, DISC_RADIUS);

        auto icon = create_chrome_icon(ChromeIcon::SecurityKey, panel.link).pixmap(QSize(ICON_SIZE, ICON_SIZE), devicePixelRatioF());
        painter.drawPixmap(QPointF(center.x() - ICON_SIZE / 2.0, center.y() - ICON_SIZE / 2.0), icon);
    }

    QVariantAnimation* m_animation { nullptr };
    qreal m_progress { 0 };
};

PasskeyPopover::PasskeyPopover(QWidget* parent)
    : Popover(parent)
{
    setAccessibleName("Passkey");
    card().setObjectName("LadybirdPasskeyPrompt");
    card().setFixedWidth(PASSKEY_PROMPT_WIDTH);

    auto* layout = new QVBoxLayout(&card());
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
}

void PasskeyPopover::update_chrome_style(QPalette const& palette)
{
    setPalette(palette);
    setStyleSheet(ChromeStyle::passkey_prompt_style_sheet(palette));
    update_icons();
    update();
}

void PasskeyPopover::update_icons()
{
    if (m_icon_tile)
        m_icon_tile->setPixmap(create_chrome_icon(m_icon, Theme::panel().link).pixmap(QSize(18, 18), devicePixelRatioF()));
    for (auto& update_icon : m_icon_updaters)
        update_icon();
}

// Every step is laid out like a message popover: an icon in a tile before the title, what the step is about below
// that, and then its controls.
QVBoxLayout& PasskeyPopover::begin_step(Step step, ChromeIcon icon, QString const& title, QString const& body)
{
    // A step that takes the place of one the user did not answer leaves that one without an answer.
    give_no_answer();

    ++m_step_serial;
    m_step = step;
    m_icon = icon;
    m_icon_updaters.clear();
    m_pin_field = nullptr;
    m_pin_submit_button = nullptr;

    if (m_content) {
        card().layout()->removeWidget(m_content);
        m_content->hide();
        m_content->deleteLater();
    }
    m_content = new QWidget(&card());
    card().layout()->addWidget(m_content);

    auto* layout = new QVBoxLayout(m_content);
    layout->setContentsMargins(PASSKEY_PROMPT_MARGIN, PASSKEY_PROMPT_MARGIN, PASSKEY_PROMPT_MARGIN, PASSKEY_PROMPT_MARGIN);
    layout->setSpacing(6);

    auto* header = new QHBoxLayout;
    header->setSpacing(12);
    m_icon_tile = new QLabel(m_content);
    m_icon_tile->setObjectName("LadybirdPasskeyPromptIcon");
    m_icon_tile->setFixedSize(32, 32);
    m_icon_tile->setAlignment(Qt::AlignCenter);
    header->addWidget(m_icon_tile, 0, Qt::AlignTop);

    auto* text = new QVBoxLayout;
    text->setSpacing(2);
    auto* title_label = new QLabel(title, m_content);
    title_label->setObjectName("LadybirdPasskeyPromptTitle");
    title_label->setTextFormat(Qt::PlainText);
    title_label->setWordWrap(true);
    text->addWidget(title_label);
    if (!body.isEmpty()) {
        auto* body_label = new QLabel(body, m_content);
        body_label->setObjectName("LadybirdPasskeyPromptBody");
        body_label->setTextFormat(Qt::PlainText);
        body_label->setWordWrap(true);
        text->addWidget(body_label);
    }
    header->addLayout(text, 1);
    layout->addLayout(header);
    layout->addSpacing(8);

    return *layout;
}

QPushButton& PasskeyPopover::add_button(QVBoxLayout& layout, char const* object_name, QString const& text)
{
    auto* button = new QPushButton(text, m_content);
    button->setObjectName(object_name);
    // NB: The platform style lays out a push button by a frame of its own rather than by the button, which would
    //     make it overlap its neighbors.
    button->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    layout.addWidget(button);
    return *button;
}

void PasskeyPopover::present(QWidget* widget_to_focus, TakeFocus take_focus)
{
    update_icons();
    m_content->show();
    card().layout()->activate();
    card().adjustSize();
    adjustSize();
    if (on_layout_change)
        on_layout_change();
    set_leaves_keyboard_to_page(take_focus == TakeFocus::No);
    if (m_suspended)
        return;
    show();
    if (on_layout_change)
        on_layout_change();
    if (take_focus == TakeFocus::No)
        return;
    // A step with one button shows no focus ring on it: Return and Escape both leave such a step.
    if (widget_to_focus)
        widget_to_focus->setFocus();
    else
        setFocus();
}

void PasskeyPopover::show_choice(WebView::PasskeyChoiceKind kind, QString const& site, Vector<Utf16String> const& options, WebView::PasskeyChoiceCallback on_choice)
{
    if (options.is_empty()) {
        auto& layout = begin_step(Step::Choice, ChromeIcon::Passkey, QStringLiteral("No passkey for %1").arg(site),
            QStringLiteral("Passkeys from other browsers and from iCloud Keychain are not available in Ladybird. Sign in another way, then create a passkey here."));
        m_on_choice = AK::move(on_choice);
        auto& close = add_button(layout, "LadybirdPasskeyPromptClose", QStringLiteral("OK"));
        QObject::connect(&close, &QPushButton::clicked, this, [this] { leave(); });
        present(nullptr);
        return;
    }

    bool is_other_ways = kind == WebView::PasskeyChoiceKind::OtherWaysToSignIn || kind == WebView::PasskeyChoiceKind::OtherWaysToCreate;
    bool is_for_new_passkey = kind == WebView::PasskeyChoiceKind::Authenticator || kind == WebView::PasskeyChoiceKind::OtherWaysToCreate;
    auto body = is_for_new_passkey ? QStringLiteral("Choose where to keep the passkey.") : QStringLiteral("Choose a passkey to sign in with.");
    if (is_other_ways)
        body = QStringLiteral("Ladybird could not confirm that it is you.");
    auto& layout = begin_step(Step::Choice, ChromeIcon::Passkey,
        is_for_new_passkey ? QStringLiteral("Create a passkey for %1").arg(site) : QStringLiteral("Sign in to %1").arg(site), body);
    m_on_choice = AK::move(on_choice);

    auto* rows_widget = new QWidget(m_content);
    auto* rows = new QVBoxLayout(rows_widget);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(2);
    if (options.size() > PASSKEY_OPTIONS_SHOWN_AT_ONCE) {
        auto* scroll_area = new QScrollArea(m_content);
        scroll_area->setFrameShape(QFrame::NoFrame);
        scroll_area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        scroll_area->setWidgetResizable(true);
        scroll_area->setWidget(rows_widget);
        // NB: QScrollArea fills the background of the widget it scrolls, but the list sits on the card.
        rows_widget->setAutoFillBackground(false);
        scroll_area->viewport()->setAutoFillBackground(false);
        scroll_area->setFixedHeight(static_cast<int>(PASSKEY_OPTIONS_SHOWN_AT_ONCE) * PASSKEY_OPTION_ROW_HEIGHT + PASSKEY_OPTION_ROW_HEIGHT / 2);
        layout.addWidget(scroll_area);
    } else {
        layout.addWidget(rows_widget);
    }

    QPushButton* first_option = nullptr;
    for (size_t index = 0; index < options.size(); ++index) {
        bool is_last = index + 1 == options.size();
        auto icon = ChromeIcon::Person;
        if (is_for_new_passkey || is_other_ways)
            icon = index == 0 ? ChromeIcon::Passkey : ChromeIcon::SecurityKey;
        else if (kind == WebView::PasskeyChoiceKind::AccountOrSecurityKey && is_last)
            icon = ChromeIcon::SecurityKey;

        // The security key is another way to sign in than the passkeys above it.
        if (icon == ChromeIcon::SecurityKey && !is_for_new_passkey && !is_other_ways) {
            auto* separator = new QFrame(m_content);
            separator->setObjectName("LadybirdPasskeyPromptSeparator");
            separator->setFixedHeight(1);
            rows->addSpacing(4);
            rows->addWidget(separator);
            rows->addSpacing(4);
        }

        auto text = qstring_from_utf16_string(options[index]);
        auto& option = add_button(*rows, "LadybirdPasskeyPromptOption", QStringLiteral(" %1").arg(fontMetrics().elidedText(text, Qt::ElideMiddle, PASSKEY_OPTION_TEXT_WIDTH).replace(QLatin1Char('&'), QStringLiteral("&&"))));
        option.setAccessibleName(text);
        option.setToolTip(text);
        option.setIconSize(QSize(16, 16));
        m_icon_updaters.append([button = &option, icon] {
            button->setIcon(create_chrome_icon(icon, Theme::panel().body));
        });
        QObject::connect(&option, &QPushButton::clicked, this, [this, index] {
            auto on_choice = AK::move(m_on_choice);
            m_on_choice = nullptr;
            wait_for_next_step();
            if (on_choice)
                on_choice(static_cast<u32>(index));
        });
        if (!first_option)
            first_option = &option;
    }

    layout.addSpacing(8);
    auto& cancel = add_button(layout, "LadybirdPasskeyPromptCancel", QStringLiteral("Cancel"));
    QObject::connect(&cancel, &QPushButton::clicked, this, [this] { leave(); });
    // The user is in a form control of the page, and can go on typing there.
    if (kind == WebView::PasskeyChoiceKind::AccountForFormControl)
        present(nullptr, TakeFocus::No);
    else
        present(first_option);
}

void PasskeyPopover::show_security_key_prompt(WebView::SecurityKeyPromptKind kind, QString const& site, Function<void()> on_cancel)
{
    auto& layout = begin_step(Step::SecurityKey, ChromeIcon::Passkey,
        kind == WebView::SecurityKeyPromptKind::SignIn ? QStringLiteral("Sign in to %1").arg(site) : QStringLiteral("Create a passkey for %1").arg(site),
        QStringLiteral("Connect your security key and touch it."));
    m_on_cancel = AK::move(on_cancel);

    layout.addWidget(new TouchIndicator(m_content));
    layout.addSpacing(8);
    auto& cancel = add_button(layout, "LadybirdPasskeyPromptCancel", QStringLiteral("Cancel"));
    QObject::connect(&cancel, &QPushButton::clicked, this, [this] { leave(); });
    present(nullptr);
}

void PasskeyPopover::show_pin_prompt(WebView::SecurityKeyPinPromptReason reason, QString const& site, Function<void(Optional<String>)> on_pin)
{
    auto& layout = begin_step(Step::Pin, ChromeIcon::Passkey, QStringLiteral("Enter the PIN of your security key"),
        QStringLiteral("Your security key needs its PIN for %1.").arg(site));
    m_on_pin = AK::move(on_pin);

    m_pin_field = new QLineEdit(m_content);
    m_pin_field->setEchoMode(QLineEdit::Password);
    m_pin_field->setPlaceholderText(QStringLiteral("PIN"));
    m_pin_field->setAccessibleName(QStringLiteral("PIN of the security key"));
    m_pin_field->setAttribute(Qt::WA_MacShowFocusRect, false);
    layout.addWidget(m_pin_field);

    if (reason == WebView::SecurityKeyPinPromptReason::Wrong) {
        auto* error = new QLabel(QStringLiteral("That PIN was not correct. Try again."), m_content);
        error->setObjectName("LadybirdPasskeyPromptError");
        error->setWordWrap(true);
        layout.addWidget(error);
    }

    layout.addSpacing(8);
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    layout.addLayout(buttons);

    auto* cancel = new QPushButton(QStringLiteral("Cancel"), m_content);
    cancel->setObjectName("LadybirdPasskeyPromptCancel");
    cancel->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    buttons->addWidget(cancel, 1);
    QObject::connect(cancel, &QPushButton::clicked, this, [this] { leave(); });

    m_pin_submit_button = new QPushButton(QStringLiteral("Continue"), m_content);
    m_pin_submit_button->setObjectName("LadybirdPasskeyPromptPrimary");
    m_pin_submit_button->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    m_pin_submit_button->setEnabled(false);
    buttons->addWidget(m_pin_submit_button, 1);
    QObject::connect(m_pin_submit_button, &QPushButton::clicked, this, [this] { submit_pin(); });

    QObject::connect(m_pin_field, &QLineEdit::textChanged, this, [this](QString const& text) {
        m_pin_submit_button->setEnabled(text.size() >= SHORTEST_SECURITY_KEY_PIN);
    });
    QObject::connect(m_pin_field, &QLineEdit::returnPressed, this, [this] { submit_pin(); });
    present(m_pin_field);
}

void PasskeyPopover::submit_pin()
{
    if (!m_on_pin || !m_pin_field || m_pin_field->text().size() < SHORTEST_SECURITY_KEY_PIN)
        return;
    auto on_pin = AK::move(m_on_pin);
    m_on_pin = nullptr;
    auto pin = ak_string_from_qstring(m_pin_field->text());
    // NB: The PIN stays in the field no longer than the user needs it there.
    m_pin_field->clear();
    wait_for_next_step();
    on_pin(AK::move(pin));
}

void PasskeyPopover::show_message(WebView::PasskeyMessage message, QString const& site)
{
    QString title;
    QString body;
    switch (message) {
    case WebView::PasskeyMessage::SecurityKeyHasNoPasskey:
        title = QStringLiteral("No passkey for %1 on this security key").arg(site);
        body = QStringLiteral("Sign in another way, or use the security key that has your passkey for this site.");
        break;
    case WebView::PasskeyMessage::SecurityKeyAlreadyHasPasskey:
        title = QStringLiteral("This security key already has your passkey");
        body = QStringLiteral("It has a passkey for your account on %1, so it needs no other.").arg(site);
        break;
    case WebView::PasskeyMessage::SecurityKeyPinBlocked:
        title = QStringLiteral("This security key takes no more PINs");
        body = QStringLiteral("Remove the key and connect it again to try once more. After too many wrong PINs, a key must be reset before it works again.");
        break;
    case WebView::PasskeyMessage::SecurityKeyNotSupported:
        title = QStringLiteral("This security key cannot do what %1 asks for").arg(site);
        break;
    }

    auto& layout = begin_step(Step::Message, ChromeIcon::Warning, title, body);
    auto& close = add_button(layout, "LadybirdPasskeyPromptClose", QStringLiteral("OK"));
    QObject::connect(&close, &QPushButton::clicked, this, [this] { leave(); });
    present(nullptr);
}

void PasskeyPopover::wait_for_next_step()
{
    if (m_content)
        m_content->setEnabled(false);
}

void PasskeyPopover::give_no_answer()
{
    auto on_choice = AK::move(m_on_choice);
    m_on_choice = nullptr;
    auto on_pin = AK::move(m_on_pin);
    m_on_pin = nullptr;
    auto on_cancel = AK::move(m_on_cancel);
    m_on_cancel = nullptr;

    if (on_choice)
        on_choice({});
    if (on_pin)
        on_pin({});
    if (on_cancel)
        on_cancel();
}

void PasskeyPopover::set_leaves_keyboard_to_page(bool leaves_keyboard_to_page)
{
    if (m_leaves_keyboard_to_page == leaves_keyboard_to_page)
        return;
    m_leaves_keyboard_to_page = leaves_keyboard_to_page;

    // NB: A window hides as its flags change, which is not the user leaving the step.
    auto was_hiding_itself = exchange(m_is_hiding_itself, true);
    setWindowFlags((leaves_keyboard_to_page ? Qt::ToolTip : Qt::Popup) | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint);
    setAttribute(Qt::WA_ShowWithoutActivating, leaves_keyboard_to_page);
    m_is_hiding_itself = was_hiding_itself;

    if (leaves_keyboard_to_page)
        qApp->installEventFilter(this);
    else
        qApp->removeEventFilter(this);
}

bool PasskeyPopover::eventFilter(QObject* watched, QEvent* event)
{
    if (m_leaves_keyboard_to_page && isVisible()) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto position = static_cast<QMouseEvent*>(event)->globalPosition().toPoint();
            if (!QRect(card().mapToGlobal(QPoint(0, 0)), card().size()).contains(position))
                leave();
        } else if (event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
            leave();
        }
    }
    return Popover::eventFilter(watched, event);
}

void PasskeyPopover::leave()
{
    ++m_step_serial;
    m_step = Step::None;
    // NB: A PIN that the user typed does not stay in the hidden popover.
    if (m_pin_field)
        m_pin_field->clear();

    m_is_hiding_itself = true;
    hide();
    m_is_hiding_itself = false;

    give_no_answer();
}

void PasskeyPopover::dismiss()
{
    if (m_step == Step::None || m_step == Step::Message)
        return;

    // The prompt for the touch of a security key answers only when the user cancels it, and here the user did not.
    m_on_cancel = nullptr;
    give_no_answer();
    wait_for_next_step();
    if (m_pin_field)
        m_pin_field->clear();

    // The next step of the ceremony can be on its way, and then takes the place of this one in the open popover.
    QTimer::singleShot(0, this, [this, step_serial = m_step_serial] {
        if (m_step_serial == step_serial)
            leave();
    });
}

void PasskeyPopover::suspend()
{
    m_suspended = true;
    hide();
}

void PasskeyPopover::resume()
{
    m_suspended = false;
    if (m_step == Step::None)
        return;
    show();
    if (on_layout_change)
        on_layout_change();
}

void PasskeyPopover::abandon()
{
    leave();
}

void PasskeyPopover::keyPressEvent(QKeyEvent* event)
{
    switch (event->key()) {
    case Qt::Key_Escape:
        leave();
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (m_step == Step::Pin) {
            submit_pin();
            return;
        }
        if (auto* button = qobject_cast<QPushButton*>(focusWidget())) {
            button->click();
            return;
        }
        if (m_step == Step::Message || m_step == Step::Choice) {
            leave();
            return;
        }
        break;
    case Qt::Key_Down:
        focusNextChild();
        return;
    case Qt::Key_Up:
        focusPreviousChild();
        return;
    default:
        break;
    }
    Popover::keyPressEvent(event);
}

void PasskeyPopover::hideEvent(QHideEvent* event)
{
    Popover::hideEvent(event);

    // A popover hides itself when the user clicks outside it, which leaves the step. Qt also closes a popup as its
    // window goes inactive, before it says so, so the prompt waits a turn to see whether it was suspended instead.
    if (m_is_hiding_itself || m_suspended || m_step == Step::None)
        return;
    QTimer::singleShot(0, this, [this, step_serial = m_step_serial] {
        if (m_step_serial == step_serial && !m_suspended && !isVisible())
            leave();
    });
}

}
