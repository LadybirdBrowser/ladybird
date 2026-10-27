/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <AK/TemporaryChange.h>
#include <UI/Qt/ChromeStyle.h>
#include <UI/Qt/Motion.h>
#include <UI/Qt/Popover.h>
#include <UI/Qt/SystemAppearance.h>
#include <UI/Qt/Theme.h>

#include <QCloseEvent>
#include <QFocusEvent>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QGraphicsOpacityEffect>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QShowEvent>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QWindow>

#if defined(AK_OS_MACOS)
#    include <UI/Qt/MacWindow.h>
#endif

namespace Ladybird {

static constexpr int POPOVER_ANCHOR_GAP = 4;
// NB: The popovers' shadow falls downward, so it needs less room above the card.
static constexpr QMargins POPOVER_SHADOW_MARGINS { 60, 36, 60, 90 };

// The window is transparent around a card that casts its own shadow. The window has no shadow of its own, which would
// not follow the card's rounded corners.
Popover::Popover(QWidget* parent)
    : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
{
#if defined(AK_OS_MACOS)
    setAttribute(Qt::WA_NativeWindow);
#endif
    setAttribute(Qt::WA_TranslucentBackground);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(POPOVER_SHADOW_MARGINS);
    layout->setSpacing(0);

    m_card = new QFrame(this);
    m_card->setFrameShape(QFrame::StyledPanel);
    m_card->setFrameShadow(QFrame::Raised);
    layout->addWidget(m_card);
    cast_popover_shadow(*m_card);
}

// An opening popover grows to its full size from a little smaller.
static constexpr qreal OPENING_POPOVER_STARTING_SCALE = 0.97;

#if defined(AK_OS_MACOS)
// A picture of a popover that is opening, which grows out of the popover's anchor as it fades in. The popover itself
// is shown at once, transparent, so it takes the keyboard and the pointer as a popup does, and appears when the picture
// has grown.
class OpeningPopoverSnapshot final : public QWidget {
public:
    AK_ALLOC_WITH_KMALLOC;

    OpeningPopoverSnapshot(QWidget& popover, QPixmap snapshot, QPointF origin, Function<void()> on_finished)
        : QWidget(nullptr, Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint | Qt::WindowTransparentForInput)
        , m_snapshot(AK::move(snapshot))
        , m_origin(origin)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_DeleteOnClose);
        setGeometry(popover.geometry());
        show();

        auto* animation = new QVariantAnimation(this);
        animation->setDuration(Motion::FAST);
        animation->setEasingCurve(Motion::enter_easing());
        animation->setStartValue(0.0);
        animation->setEndValue(1.0);
        QObject::connect(animation, &QVariantAnimation::valueChanged, this, [this](QVariant const& value) {
            m_progress = value.toReal();
            update();
        });
        QObject::connect(animation, &QVariantAnimation::finished, this, [this, on_finished = AK::move(on_finished)] {
            on_finished();
            close();
        });
        animation->start(QAbstractAnimation::DeleteWhenStopped);
    }

private:
    virtual void paintEvent(QPaintEvent*) override
    {
        auto scale = OPENING_POPOVER_STARTING_SCALE + ((1.0 - OPENING_POPOVER_STARTING_SCALE) * m_progress);

        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.setOpacity(m_progress);
        painter.translate(m_origin);
        painter.scale(scale, scale);
        painter.translate(-m_origin);
        painter.drawPixmap(0, 0, m_snapshot);
    }

    QPixmap m_snapshot;
    QPointF m_origin;
    qreal m_progress { 0 };
};
#endif

// A popover fades in quickly as it opens, growing out of its anchor.
void Popover::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);

    Optional<QPointF> origin;
    if (!SystemAppearance::prefers_reduced_motion() && m_anchor_point.has_value())
        origin = QPointF(mapFromGlobal(*m_anchor_point));

#if defined(AK_OS_MACOS)
    // NB: AppKit would fade the window out as it hides, under the picture that fades out in its place.
    disable_appkit_window_animations(*this);

    if (!origin.has_value()) {
        auto* animation = new QVariantAnimation(this);
        animation->setDuration(Motion::FAST);
        animation->setEasingCurve(Motion::enter_easing());
        animation->setStartValue(0.0);
        animation->setEndValue(1.0);
        QObject::connect(animation, &QVariantAnimation::valueChanged, this, [this](QVariant const& value) {
            setWindowOpacity(value.toReal());
        });
        setWindowOpacity(0);
        animation->start(QAbstractAnimation::DeleteWhenStopped);
    } else {
        auto snapshot = grab();
        setWindowOpacity(0);
        m_opening_snapshot = new OpeningPopoverSnapshot(*this, AK::move(snapshot), *origin, [popover = QPointer { this }] {
            if (popover)
                popover->setWindowOpacity(1);
        });
    }
#else
    fade_in(origin);
#endif

    if (on_visibility_change)
        on_visibility_change(true);
}

#if defined(AK_OS_MACOS)
// A picture of a popover that has just closed, which fades out where it was. The popover itself closes at once, so
// that it lets go of the keyboard and the pointer as a popup does.
class ClosingPopoverSnapshot final : public QWidget {
public:
    AK_ALLOC_WITH_KMALLOC;

    ClosingPopoverSnapshot(QPixmap snapshot, QRect geometry)
        : QWidget(nullptr, Qt::ToolTip | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint | Qt::WindowTransparentForInput)
        , m_snapshot(AK::move(snapshot))
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_DeleteOnClose);
        setGeometry(geometry);
        show();

        auto* animation = new QVariantAnimation(this);
        animation->setDuration(Motion::EXIT);
        animation->setEasingCurve(Motion::exit_easing());
        animation->setStartValue(1.0);
        animation->setEndValue(0.0);
        QObject::connect(animation, &QVariantAnimation::valueChanged, this, [this](QVariant const& value) {
            m_opacity = value.toReal();
            update();
        });
        QObject::connect(animation, &QVariantAnimation::finished, this, &QWidget::close);
        animation->start(QAbstractAnimation::DeleteWhenStopped);
    }

private:
    virtual void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setOpacity(m_opacity);
        painter.drawPixmap(0, 0, m_snapshot);
    }

    QPixmap m_snapshot;
    qreal m_opacity { 1 };
};
#endif

#if !defined(AK_OS_MACOS)
void Popover::stop_fading()
{
    if (!m_fading_animation)
        return;
    // NB: The animation deletes itself once stopped.
    m_fading_animation->stop();
    m_fading_animation = nullptr;
    m_is_fading_out = false;
    m_fading_picture = {};
    m_fading_origin = {};
    m_card->show();
    layout()->setEnabled(true);
}

void Popover::paintEvent(QPaintEvent* event)
{
    if (!m_fading_picture.has_value()) {
        QWidget::paintEvent(event);
        return;
    }
    QPainter painter(this);
    painter.setOpacity(m_fading_progress);
    if (m_fading_origin.has_value()) {
        auto scale = OPENING_POPOVER_STARTING_SCALE + ((1.0 - OPENING_POPOVER_STARTING_SCALE) * m_fading_progress);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.translate(*m_fading_origin);
        painter.scale(scale, scale);
        painter.translate(-*m_fading_origin);
    }
    painter.drawPixmap(0, 0, *m_fading_picture);
}
#endif

void Popover::setVisible(bool visible)
{
#if defined(AK_OS_MACOS)
    // The picture a closing popover fades out with is taken while it is still shown, as it looked, with the focus that
    // it loses once hidden.
    if (!visible && isVisible() && !size().isEmpty())
        m_closing_picture = grab();
#else
    if (visible) {
        stop_fading();
    } else if (isVisible() && !m_is_finishing_close) {
        fade_out(AfterFadingOut::Hide);
        return;
    }
#endif
    QWidget::setVisible(visible);
}

#if !defined(AK_OS_MACOS)
// NB: A window of its own first appears a frame or more after it is shown on Wayland, so a picture of the popover in a
//     window of its own would leave a frame with neither. The popover fades out a picture of itself in its own window
//     instead, and hides or closes once it has faded. One the compositor closed itself, such as a popup dismissed by a
//     click outside it, is already gone.
void Popover::closeEvent(QCloseEvent* event)
{
    if (!event->spontaneous() && isVisible() && !m_is_finishing_close) {
        event->ignore();
        fade_out(AfterFadingOut::Close);
        return;
    }
    QWidget::closeEvent(event);
}

// NB: The card is hidden behind its picture, with the layout set aside so the window keeps its size.
void Popover::show_fading_picture()
{
    m_fading_picture = grab();
    layout()->setEnabled(false);
    m_card->hide();
}

// NB: As it fades out, the popover fades in a picture of itself in its own window, for the same reason.
void Popover::fade_in(Optional<QPointF> origin)
{
    stop_fading();
    show_fading_picture();
    m_fading_origin = origin;
    m_fading_progress = 0;
    m_fading_animation = new QVariantAnimation(this);
    m_fading_animation->setDuration(Motion::FAST);
    m_fading_animation->setEasingCurve(Motion::enter_easing());
    m_fading_animation->setStartValue(0.0);
    m_fading_animation->setEndValue(1.0);
    QObject::connect(m_fading_animation, &QVariantAnimation::valueChanged, this, [this](QVariant const& value) {
        m_fading_progress = value.toReal();
        update();
    });
    QObject::connect(m_fading_animation, &QVariantAnimation::finished, this, [this] {
        m_fading_animation = nullptr;
        m_fading_picture = {};
        m_fading_origin = {};
        m_card->show();
        layout()->setEnabled(true);
    });
    m_fading_animation->start(QAbstractAnimation::DeleteWhenStopped);
}

void Popover::fade_out(AfterFadingOut after_fading_out)
{
    if (m_is_fading_out)
        return;
    stop_fading();
    show_fading_picture();
    m_is_fading_out = true;
    m_fading_progress = 1;
    m_fading_animation = new QVariantAnimation(this);
    m_fading_animation->setDuration(Motion::EXIT);
    m_fading_animation->setEasingCurve(Motion::exit_easing());
    m_fading_animation->setStartValue(1.0);
    m_fading_animation->setEndValue(0.0);
    QObject::connect(m_fading_animation, &QVariantAnimation::valueChanged, this, [this](QVariant const& value) {
        m_fading_progress = value.toReal();
        update();
    });
    QObject::connect(m_fading_animation, &QVariantAnimation::finished, this, [this, after_fading_out] {
        TemporaryChange finishing_close { m_is_finishing_close, true };
        if (after_fading_out == AfterFadingOut::Close)
            close();
        else
            hide();
    });
    m_fading_animation->start(QAbstractAnimation::DeleteWhenStopped);
}
#endif

// The window reaches well past the card, to make room for its shadow. A press there is outside the popover as far as
// anyone can see, so it closes the popover like a press anywhere else outside it.
void Popover::mousePressEvent(QMouseEvent* event)
{
#if !defined(AK_OS_MACOS)
    // A popover that is fading out has closed, and takes no more presses.
    if (m_is_fading_out) {
        event->accept();
        return;
    }
#endif
    if (!m_card->geometry().contains(event->position().toPoint())) {
        close();
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}

// A popover fades out quickly as it closes.
void Popover::hideEvent(QHideEvent* event)
{
    QWidget::hideEvent(event);

#if defined(AK_OS_MACOS)
    if (m_opening_snapshot)
        m_opening_snapshot->close();
    setWindowOpacity(1);
    if (auto picture = exchange(m_closing_picture, {}); picture.has_value() && !event->spontaneous())
        new ClosingPopoverSnapshot(picture.release_value(), geometry());
#else
    stop_fading();
#endif

    if (on_visibility_change)
        on_visibility_change(false);
}

// A message that asks whether to act on it. Leaving the popover in any other way than accepting it, such as by clicking
// outside it, dismisses the message.
MessagePopover::MessagePopover(QWidget* parent, int width, QString const& title, QString const& body, QString const& dismiss_text, QString const& accept_text, QIcon const& icon)
    : Popover(parent)
{
    card().setObjectName("LadybirdMessagePopover");
    card().setFixedWidth(width);

    auto* layout = new QVBoxLayout(&card());
    layout->setContentsMargins(16, 16, 16, 16);
    layout->setSpacing(6);

    auto* title_label = new QLabel(title, &card());
    title_label->setObjectName("LadybirdMessagePopoverTitle");
    title_label->setWordWrap(true);
    if (icon.isNull()) {
        layout->addWidget(title_label);
    } else {
        auto* header = new QHBoxLayout;
        header->setSpacing(12);
        auto* icon_tile = new QLabel(&card());
        icon_tile->setObjectName("LadybirdMessagePopoverIcon");
        icon_tile->setFixedSize(32, 32);
        icon_tile->setAlignment(Qt::AlignCenter);
        icon_tile->setPixmap(icon.pixmap(QSize(18, 18), devicePixelRatioF()));
        header->addWidget(icon_tile, 0, Qt::AlignTop);
        header->addWidget(title_label, 1);
        layout->addLayout(header);
    }

    if (!body.isEmpty()) {
        auto* body_label = new QLabel(body, &card());
        body_label->setObjectName("LadybirdMessagePopoverBody");
        body_label->setWordWrap(true);
        layout->addWidget(body_label);
    }

    layout->addSpacing(8);
    auto* button_layout = new QHBoxLayout;
    button_layout->setSpacing(8);
    button_layout->addStretch();
    layout->addLayout(button_layout);

    auto* dismiss_button = new QPushButton(dismiss_text, &card());
    dismiss_button->setObjectName("LadybirdMessagePopoverDismissButton");
    // NB: The platform style lays out a push button by a frame of its own rather than by the button, which would
    //     make it overlap its neighbors.
    dismiss_button->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    dismiss_button->setFocusPolicy(Qt::NoFocus);
    QObject::connect(dismiss_button, &QPushButton::clicked, this, [this] { close(); });
    button_layout->addWidget(dismiss_button);

    auto* accept_button = new QPushButton(accept_text, &card());
    accept_button->setObjectName("LadybirdMessagePopoverAcceptButton");
    accept_button->setAttribute(Qt::WA_LayoutUsesWidgetRect);
    accept_button->setDefault(true);
    QObject::connect(accept_button, &QPushButton::clicked, this, [this] {
        m_is_accepted = true;
        close();
        if (on_accept)
            on_accept();
    });
    button_layout->addWidget(accept_button);
}

void MessagePopover::update_chrome_style(QPalette const& palette)
{
    setPalette(palette);
    setStyleSheet(ChromeStyle::message_popover_style_sheet(palette));
}

void MessagePopover::hideEvent(QHideEvent* event)
{
    Popover::hideEvent(event);
    if (!exchange(m_is_accepted, false) && on_dismiss)
        on_dismiss();
}

// Places the popover's card below the anchor with their left or right edges aligned, kept on the anchor's screen, and
// above the anchor when there is no room below it. Only the shadow around the card may reach past the edge of the screen.
void move_popover_below(Popover& popover, QWidget& anchor, PopoverAlignment alignment, Optional<QRect> anchor_rect)
{
    auto rect = anchor_rect.value_or(anchor.rect());
    auto card_size = popover.size().shrunkBy(POPOVER_SHADOW_MARGINS);
    auto anchor_position = anchor.mapToGlobal(rect.bottomRight());
    auto card_x = alignment == PopoverAlignment::Leading ? anchor.mapToGlobal(rect.topLeft()).x() : anchor_position.x() - card_size.width();
    auto card_position = QPoint(card_x, anchor_position.y() + POPOVER_ANCHOR_GAP);

    if (auto* screen = QGuiApplication::screenAt(anchor_position)) {
        auto available_geometry = screen->availableGeometry();
        if (card_position.x() < available_geometry.left())
            card_position.setX(available_geometry.left());
        if (card_position.x() + card_size.width() > available_geometry.right())
            card_position.setX(available_geometry.right() - card_size.width() + 1);
        if (card_position.y() + card_size.height() > available_geometry.bottom())
            card_position.setY(anchor.mapToGlobal(rect.topRight()).y() - card_size.height() - POPOVER_ANCHOR_GAP);
        if (card_position.y() < available_geometry.top())
            card_position.setY(available_geometry.top());
    }

    popover.move(card_position - QPoint(POPOVER_SHADOW_MARGINS.left(), POPOVER_SHADOW_MARGINS.top()));

    // The popover grows out of the corner of its card nearest the anchor.
    auto anchor_x = alignment == PopoverAlignment::Leading ? card_position.x() : card_position.x() + card_size.width();
    popover.set_anchor_point(QPoint(anchor_x, card_position.y()));
}

// Cards that float over other content cast the same shadow as popovers, whether they are windows of their own or not.
void cast_popover_shadow(QWidget& card)
{
    auto* shadow = new QGraphicsDropShadowEffect(&card);
    shadow->setBlurRadius(ChromeStyle::drop_shadow_blur_radius(36));
    shadow->setOffset(0, 14);
    shadow->setColor(QColor(0, 0, 0, Theme::is_dark() ? 115 : 41));
    card.setGraphicsEffect(shadow);
}

static constexpr int SHEET_SHADOW_MARGIN = 100;

// The shape that casts the shadow. It lies under the surface, so only the shadow around it shows.
class FloatingShadowCaster final : public QWidget {
public:
    AK_ALLOC_WITH_KMALLOC;

    FloatingShadowCaster(QWidget& parent, qreal corner_radius, FloatingShadowTone tone, FloatingShadowKind kind)
        : QWidget(&parent)
        , m_corner_radius(corner_radius)
        , m_tone(tone)
    {
        auto is_dark = tone == FloatingShadowTone::Dark || Theme::is_dark();
        auto* shadow = new QGraphicsDropShadowEffect(this);
        if (kind == FloatingShadowKind::Sheet) {
            shadow->setBlurRadius(ChromeStyle::drop_shadow_blur_radius(40));
            shadow->setOffset(0, 18);
            shadow->setColor(QColor(0, 0, 0, is_dark ? 140 : 56));
        } else {
            shadow->setBlurRadius(ChromeStyle::drop_shadow_blur_radius(36));
            shadow->setOffset(0, 14);
            shadow->setColor(QColor(0, 0, 0, is_dark ? 115 : 41));
        }
        setGraphicsEffect(shadow);
    }

private:
    virtual void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(m_tone == FloatingShadowTone::Dark ? QColor(28, 28, 30) : Theme::panel().surface);
        painter.drawRoundedRect(QRectF(rect()), m_corner_radius, m_corner_radius);
    }

    qreal m_corner_radius { 0 };
    FloatingShadowTone m_tone { FloatingShadowTone::FollowTheme };
};

class FloatingShadow final : public QWidget {
public:
    AK_ALLOC_WITH_KMALLOC;

    // NB: On macOS, like the suggestions under the address field, the shadow is a window of its own embedded in the
    //     browser window, so it stacks above the page's native view, while letting the mouse through to it. Elsewhere
    //     the page and the surface are drawn in the window itself, so the shadow is a widget beside the surface.
    FloatingShadow(QWidget& surface, qreal corner_radius, FloatingShadowTone tone, FloatingShadowKind kind)
#if defined(AK_OS_MACOS)
        : QWidget(surface.window(), Qt::Tool | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint | Qt::WindowTransparentForInput)
#else
        : QWidget(surface.parentWidget())
#endif
        , m_surface(&surface)
        , m_margins(kind == FloatingShadowKind::Sheet
                  ? QMargins(SHEET_SHADOW_MARGIN, 0, SHEET_SHADOW_MARGIN, SHEET_SHADOW_MARGIN)
                  : POPOVER_SHADOW_MARGINS)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_TransparentForMouseEvents);
#if defined(AK_OS_MACOS)
        setAttribute(Qt::WA_NativeWindow);
#endif
        auto* layout = new QVBoxLayout(this);
        layout->setContentsMargins(m_margins);
        layout->addWidget(new FloatingShadowCaster(*this, corner_radius, tone, kind));

        watch_surface_and_its_ancestors();
        connect(&surface, &QObject::destroyed, this, &QObject::deleteLater);
        hide();
        follow_surface();
    }

private:
    virtual bool eventFilter(QObject*, QEvent* event) override
    {
        switch (event->type()) {
        case QEvent::Move:
        case QEvent::Resize:
        case QEvent::Show:
        case QEvent::Hide:
        case QEvent::ZOrderChange:
            follow_surface();
            break;
        case QEvent::ParentChange:
            // NB: The surface can move to another window with its tab, so the shadow watches the widgets it now lies in.
            watch_surface_and_its_ancestors();
            follow_surface();
            break;
        default:
            break;
        }
        return false;
    }

    // The surface moves in the window when it or any widget it lies in moves.
    void watch_surface_and_its_ancestors()
    {
        if (!m_surface)
            return;
        for (QWidget* widget = m_surface; widget; widget = widget->isWindow() ? nullptr : widget->parentWidget())
            widget->installEventFilter(this);
    }

    void follow_surface()
    {
        // NB: The shadow is deleted only after its surface, and may still hear from the surface's window until then.
        if (m_is_following || !m_surface)
            return;
        TemporaryChange following { m_is_following, true };

        if (!m_surface->isVisible()) {
            hide();
            return;
        }

#if defined(AK_OS_MACOS)
        // NB: The shadow belongs to the surface's window, so it is not deleted with a window the surface has left.
        if (parentWidget() != m_surface->window())
            setParent(m_surface->window(), windowFlags());

        // NB: The shadow is stacked right below the surface's native view, among the views of the surface's native
        //     parent, so it is placed in that parent's coordinates.
        auto* host = m_surface->nativeParentWidget();
        if (!host)
            host = m_surface->window();
        (void)winId();
        windowHandle()->setParent(host->windowHandle());
        auto surface_rect = QRect(m_surface->mapTo(host, QPoint()), m_surface->size());
        setGeometry(surface_rect.marginsAdded(m_margins));
        show();
        stack_appkit_view_below(*this, *m_surface);
#else
        if (parentWidget() != m_surface->parentWidget())
            setParent(m_surface->parentWidget());
        setGeometry(m_surface->geometry().marginsAdded(m_margins));
        show();
        stackUnder(m_surface);
#endif
    }

    QPointer<QWidget> m_surface;
    QMargins m_margins;
    bool m_is_following { false };
};

QWidget& cast_floating_shadow(QWidget& surface, qreal corner_radius, FloatingShadowTone tone, FloatingShadowKind kind)
{
    return *new FloatingShadow(surface, corner_radius, tone, kind);
}

void set_floating_window_opacity(QWidget& window, qreal opacity)
{
#if defined(AK_OS_MACOS)
    set_appkit_view_opacity(window, opacity);
#else
    // NB: Wayland has no window opacity, and a widget in a window has none either, so those fade by an effect instead.
    static auto const has_window_opacity = !QGuiApplication::platformName().startsWith(QStringLiteral("wayland"));
    if (window.isWindow() && has_window_opacity) {
        window.setWindowOpacity(opacity);
        return;
    }
    auto* effect = qobject_cast<QGraphicsOpacityEffect*>(window.graphicsEffect());
    if (opacity >= 1.0) {
        if (effect)
            window.setGraphicsEffect(nullptr);
        return;
    }
    if (!effect) {
        // NB: A widget has only one effect, and one it casts its shadow with is left alone.
        if (window.graphicsEffect())
            return;
        effect = new QGraphicsOpacityEffect(&window);
        window.setGraphicsEffect(effect);
    }
    effect->setOpacity(opacity);
#endif
}

PopoverSwitch::PopoverSwitch(QWidget* parent)
    : QCheckBox(parent)
{
    setAttribute(Qt::WA_MacShowFocusRect, false);
    setAttribute(Qt::WA_LayoutUsesWidgetRect);
    setFocusPolicy(Qt::StrongFocus);
    setFixedSize(sizeHint());
}

static constexpr int CHECK_BOX_SIZE = 16;
static constexpr int CHECK_BOX_TEXT_GAP = 8;

PopoverCheckBox::PopoverCheckBox(QString const& text, QWidget* parent)
    : QCheckBox(text, parent)
{
    setAttribute(Qt::WA_MacShowFocusRect, false);
}

// Focus that came from a click isn't ringed.
static bool is_keyboard_focus_reason(Qt::FocusReason reason)
{
    return reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason || reason == Qt::ShortcutFocusReason;
}

void PopoverSwitch::focusInEvent(QFocusEvent* event)
{
    m_has_keyboard_focus = is_keyboard_focus_reason(event->reason());
    QCheckBox::focusInEvent(event);
}

void PopoverCheckBox::focusInEvent(QFocusEvent* event)
{
    m_has_keyboard_focus = is_keyboard_focus_reason(event->reason());
    QCheckBox::focusInEvent(event);
}

QSize PopoverCheckBox::sizeHint() const
{
    auto metrics = fontMetrics();
    return { CHECK_BOX_SIZE + CHECK_BOX_TEXT_GAP + metrics.horizontalAdvance(text()), max(CHECK_BOX_SIZE, metrics.height()) };
}

void PopoverCheckBox::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QRectF box { 0, (height() - CHECK_BOX_SIZE) / 2.0, CHECK_BOX_SIZE, CHECK_BOX_SIZE };
    auto const& panel = Theme::panel();
    if (isChecked()) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(Theme::accent());
        painter.drawRoundedRect(box, 4, 4);
        // The check is drawn on a 16 px grid, scaled into the middle of the box.
        painter.save();
        painter.translate(box.center());
        painter.scale(11.0 / 16, 11.0 / 16);
        painter.translate(-8, -8);
        painter.setPen(QPen(Qt::white, 2.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPointF check[] { { 3.5, 8.5 }, { 6.5, 11.5 }, { 12.5, 4.5 } };
        painter.drawPolyline(check, 3);
        painter.restore();
    } else {
        painter.setPen(QPen(panel.shortcut, 1.5));
        painter.setBrush(panel.page);
        painter.drawRoundedRect(box.adjusted(0.75, 0.75, -0.75, -0.75), 3.25, 3.25);
    }
    if (hasFocus() && m_has_keyboard_focus)
        ChromeStyle::paint_focus_ring(painter, box, 4);

    painter.setPen(palette().color(foregroundRole()));
    painter.drawText(QRectF(CHECK_BOX_SIZE + CHECK_BOX_TEXT_GAP, 0, width() - CHECK_BOX_SIZE - CHECK_BOX_TEXT_GAP, height()), Qt::AlignLeft | Qt::AlignVCenter, text());
}

void PopoverSwitch::paintEvent(QPaintEvent*)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    QRectF track { (width() - 32) / 2.0, (height() - 18) / 2.0, 32, 18 };
    auto track_color = isChecked() ? Theme::accent() : (Theme::is_dark() ? QColor(0x48484e) : QColor(0xbcbcc2));
    // A switch that can't be turned fades, knob and all, so it doesn't look as though it could.
    if (!isEnabled())
        painter.setOpacity(0.4);

    painter.setPen(Qt::NoPen);
    painter.setBrush(track_color);
    painter.drawRoundedRect(track, 9, 9);

    auto knob_x = isChecked() != (layoutDirection() == Qt::RightToLeft) ? track.right() - 16 : track.left() + 2;
    painter.setBrush(QColor(0, 0, 0, 40));
    painter.drawEllipse(QRectF(knob_x, track.top() + 3, 14, 14));
    painter.setBrush(Qt::white);
    painter.drawEllipse(QRectF(knob_x, track.top() + 2, 14, 14));

    if (hasFocus() && m_has_keyboard_focus) {
        painter.setOpacity(1);
        ChromeStyle::paint_focus_ring(painter, track, 9);
    }
}

}
