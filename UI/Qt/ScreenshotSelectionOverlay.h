/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>

#include <QDeadlineTimer>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QWidget>

class QPushButton;
class QTimer;

namespace Ladybird {

class ScreenshotSelectionOverlay final : public QWidget {
public:
    AK_ALLOC_WITH_KMALLOC;

    static constexpr int minimum_selection_size = 8;

    enum class Purpose {
        ReportIssue,
        SaveScreenshot,
    };

    ScreenshotSelectionOverlay(QImage frame, QWidget* parent, Purpose = Purpose::ReportIssue);

    void select(QRect);
    void use_whole_frame();
    void use_full_page();
    void skip();
    void dismiss();

    bool reveals_page() const { return m_reveals_page; }
    void page_did_scroll();
    void page_did_paint();

    Function<void()> on_close;
    Function<void(QImage)> on_finished;
    Function<void()> on_full_page_requested;
    // The frame can arrive later, when the process that holds it sends it.
    Function<void(Function<void(QImage)>)> on_request_page_frame;
    Function<void()> on_page_visibility_change;

protected:
    virtual void paintEvent(QPaintEvent*) override;
    virtual void mousePressEvent(QMouseEvent*) override;
    virtual void mouseMoveEvent(QMouseEvent*) override;
    virtual void mouseReleaseEvent(QMouseEvent*) override;
    virtual void resizeEvent(QResizeEvent*) override;
    virtual void keyPressEvent(QKeyEvent*) override;
    virtual bool focusNextPrevChild(bool next) override;
    virtual void changeEvent(QEvent*) override;
    virtual bool eventFilter(QObject*, QEvent*) override;

private:
    void place_instructions();
    void cover_page();
    void once_page_is_covered(Function<void()>);
    QRect selection() const;
    void finish(QImage);
    void update_chrome_style();

    QImage m_frame;
    bool m_reveals_page { false };
    // Moves on whenever the page scrolls, so a frame that was asked for before then is not taken.
    u64 m_reveal_generation { 0 };
    // What the user asked for while the page was revealed, which takes the frame that covers it.
    Function<void()> m_action_once_page_is_covered;
    QTimer* m_scroll_settle_timer { nullptr };
    QDeadlineTimer m_follow_frames_until;
    Optional<QPoint> m_drag_start;
    QPoint m_drag_end;
    QWidget* m_instructions { nullptr };
    Optional<QPoint> m_instructions_position;
    Optional<QPoint> m_instructions_grab_offset;
    Vector<QPushButton*> m_buttons;
    QPushButton* m_close_button { nullptr };
};

}
