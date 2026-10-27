/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/Platform.h>
#include <AK/Types.h>
#include <AK/kmalloc.h>

#include <QCheckBox>
#include <QIcon>
#include <QPointer>
#include <QWidget>

class QFrame;
class QCloseEvent;
class QHideEvent;
class QMouseEvent;
class QShowEvent;
class QVariantAnimation;

namespace Ladybird {

class Popover : public QWidget {
public:
    AK_ALLOC_WITH_KMALLOC;

    QFrame& card() { return *m_card; }

    Function<void(bool)> on_visibility_change;

    // The point the popover hangs from, which it grows out of as it opens.
    void set_anchor_point(Optional<QPoint> point) { m_anchor_point = point; }

    virtual void setVisible(bool) override;

protected:
    explicit Popover(QWidget* parent);

    virtual void showEvent(QShowEvent*) override;
    virtual void hideEvent(QHideEvent*) override;
    virtual void mousePressEvent(QMouseEvent*) override;
#if !defined(AK_OS_MACOS)
    virtual void closeEvent(QCloseEvent*) override;
    virtual void paintEvent(QPaintEvent*) override;
#endif

private:
    QFrame* m_card { nullptr };
    Optional<QPoint> m_anchor_point;
#if defined(AK_OS_MACOS)
    QPointer<QWidget> m_opening_snapshot;
    Optional<QPixmap> m_closing_picture;
#else
    enum class AfterFadingOut {
        Hide,
        Close,
    };
    void fade_in(Optional<QPointF> origin);
    void fade_out(AfterFadingOut);
    void show_fading_picture();
    void stop_fading();
    QPointer<QVariantAnimation> m_fading_animation;
    bool m_is_fading_out { false };
    Optional<QPixmap> m_fading_picture;
    Optional<QPointF> m_fading_origin;
    qreal m_fading_progress { 1 };
    bool m_is_finishing_close { false };
#endif
};

class MessagePopover final : public Popover {
public:
    AK_ALLOC_WITH_KMALLOC;

    // With an icon, the icon stands in a tile before the title.
    MessagePopover(QWidget* parent, int width, QString const& title, QString const& body, QString const& dismiss_text, QString const& accept_text, QIcon const& icon = {});

    void update_chrome_style(QPalette const&);

    Function<void()> on_accept;
    Function<void()> on_dismiss;

private:
    virtual void hideEvent(QHideEvent*) override;

    bool m_is_accepted { false };
};

// A small switch in the style of the system's, for settings in popovers.
class PopoverSwitch final : public QCheckBox {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit PopoverSwitch(QWidget* parent);

    virtual QSize sizeHint() const override { return { 36, 22 }; }

private:
    virtual bool hitButton(QPoint const& position) const override { return rect().contains(position); }
    virtual void paintEvent(QPaintEvent*) override;
    virtual void focusInEvent(QFocusEvent*) override;

    bool m_has_keyboard_focus { false };
};

// A checkbox: a rounded box, ringed when unchecked, filled with a white check when checked.
class PopoverCheckBox final : public QCheckBox {
public:
    AK_ALLOC_WITH_KMALLOC;

    PopoverCheckBox(QString const& text, QWidget* parent);

    virtual QSize sizeHint() const override;

private:
    virtual bool hitButton(QPoint const& position) const override { return rect().contains(position); }
    virtual void paintEvent(QPaintEvent*) override;
    virtual void focusInEvent(QFocusEvent*) override;

    bool m_has_keyboard_focus { false };
};

// Which edges of the popover's card and its anchor line up.
enum class PopoverAlignment : u8 {
    Leading,
    Trailing,
};
// The popover hangs from the anchor's rect, or from the given part of it where the anchor draws less than all of it.
void move_popover_below(Popover&, QWidget& anchor, PopoverAlignment = PopoverAlignment::Trailing, Optional<QRect> anchor_rect = {});
void cast_popover_shadow(QWidget&);

// A surface that is a native view of its own, floating over the page, cannot draw a shadow outside itself. This gives it
// the popovers' shadow, cast by a native view behind it that follows it around and lets the mouse through.
enum class FloatingShadowTone : u8 {
    FollowTheme,
    // For surfaces that are dark in both themes.
    Dark,
};
enum class FloatingShadowKind : u8 {
    Popover,
    // A sheet hangs from the top of the page, and casts the deeper shadow of a dialog below it only.
    Sheet,
};
QWidget& cast_floating_shadow(QWidget& surface, qreal corner_radius, FloatingShadowTone = FloatingShadowTone::FollowTheme, FloatingShadowKind = FloatingShadowKind::Popover);

// Floating surfaces and their shadows are native views embedded in the browser window on macOS, which have no window
// opacity of their own.
void set_floating_window_opacity(QWidget&, qreal opacity);

}
