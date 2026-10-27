/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Optional.h>
#include <LibTest/TestCase.h>
#include <UI/Qt/ScreenshotSelectionOverlay.h>

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPushButton>

using Ladybird::ScreenshotSelectionOverlay;

static QImage create_frame()
{
    QImage frame(400, 200, QImage::Format_RGB32);
    frame.fill(Qt::white);
    for (int y = 20; y < 60; ++y) {
        for (int x = 40; x < 140; ++x)
            frame.setPixel(x, y, qRgb(255, 0, 0));
    }
    return frame;
}

static void send_mouse(QWidget& widget, QEvent::Type type, QPoint position)
{
    auto buttons = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
    auto button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent event(type, position, widget.mapToGlobal(position), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&widget, &event);
}

static void send_key(QWidget& widget, Qt::Key key)
{
    QKeyEvent event(QEvent::KeyPress, key, Qt::NoModifier);
    QApplication::sendEvent(&widget, &event);
}

struct Result {
    Optional<QImage> image;
    bool closed { false };
};

static void track(ScreenshotSelectionOverlay& overlay, Result& result)
{
    result = {};
    overlay.on_finished = [&result](QImage image) { result.image = image; };
    overlay.on_close = [&result] { result.closed = true; };
}

TEST_CASE(a_selection_crops_the_frame_in_its_own_pixels)
{
    QWidget view;
    view.resize(200, 100);
    ScreenshotSelectionOverlay overlay { create_frame(), &view };
    Result result;
    track(overlay, result);
    view.show();

    overlay.select(QRect(20, 10, 50, 20));
    EXPECT(result.image.has_value());
    EXPECT_EQ(result.image->size(), QSize(100, 40));
    EXPECT_EQ(result.image->pixel(0, 0), qRgb(255, 0, 0));
    EXPECT_EQ(result.image->pixel(99, 39), qRgb(255, 0, 0));
    EXPECT(!overlay.isVisible());
}

TEST_CASE(a_selection_is_bounded_by_the_frame)
{
    QWidget view;
    view.resize(200, 100);
    ScreenshotSelectionOverlay overlay { create_frame(), &view };
    Result result;
    track(overlay, result);
    view.show();

    overlay.select(QRect(150, 50, 100, 100));
    EXPECT(result.image.has_value());
    EXPECT_EQ(result.image->size(), QSize(100, 100));
}

TEST_CASE(a_click_without_a_drag_selects_nothing)
{
    QWidget view;
    view.resize(200, 100);
    ScreenshotSelectionOverlay overlay { create_frame(), &view };
    Result result;
    track(overlay, result);
    view.show();

    send_mouse(overlay, QEvent::MouseButtonPress, QPoint(100, 80));
    send_mouse(overlay, QEvent::MouseButtonRelease, QPoint(100, 80));
    EXPECT(!result.image.has_value());
    EXPECT(overlay.isVisible());

    send_mouse(overlay, QEvent::MouseButtonPress, QPoint(10, 70));
    send_mouse(overlay, QEvent::MouseMove, QPoint(60, 90));
    send_mouse(overlay, QEvent::MouseButtonRelease, QPoint(60, 90));
    EXPECT(result.image.has_value());
    EXPECT_EQ(result.image->size(), QSize(100, 40));
}

TEST_CASE(the_keyboard_takes_the_whole_frame_or_cancels)
{
    QWidget view;
    view.resize(200, 100);
    Result result;

    {
        ScreenshotSelectionOverlay overlay { create_frame(), &view };
        track(overlay, result);
        view.show();
        send_key(overlay, Qt::Key_Return);
        EXPECT(result.image.has_value());
        EXPECT_EQ(result.image->size(), QSize(400, 200));
    }

    {
        ScreenshotSelectionOverlay overlay { create_frame(), &view };
        track(overlay, result);
        send_key(overlay, Qt::Key_Escape);
        EXPECT(!result.image.has_value());
        EXPECT(result.closed);
        EXPECT(!overlay.isVisible());
    }
}

TEST_CASE(the_instructions_keep_their_padding_when_their_buttons_grow)
{
    QWidget view;
    view.resize(1000, 400);
    ScreenshotSelectionOverlay overlay { create_frame(), &view };
    view.show();

    auto* instructions = overlay.findChild<QWidget*>("ScreenshotSelectionInstructions");
    auto buttons = instructions->findChildren<QPushButton*>();
    for (auto* button : buttons) {
        auto font = button->font();
        font.setPointSizeF(font.pointSizeF() * 2);
        button->setFont(font);
    }
    QApplication::processEvents();

    for (auto* button : buttons) {
        EXPECT(button->geometry().top() >= 12);
        EXPECT(instructions->height() - button->geometry().bottom() - 1 >= 12);
    }
}

TEST_CASE(the_keyboard_reaches_every_choice)
{
    QWidget view;
    view.resize(1000, 300);
    Result result;
    auto press = [](Qt::Key key) { send_key(*QApplication::focusWidget(), key); };

    {
        ScreenshotSelectionOverlay overlay { create_frame(), &view };
        track(overlay, result);
        view.show();
        view.activateWindow();
        overlay.setFocus();
        QApplication::processEvents();

        press(Qt::Key_Tab);
        EXPECT_EQ(QApplication::focusWidget(), overlay.findChild<QPushButton*>("ScreenshotSelectionWholeFrameButton"));
        press(Qt::Key_Tab);
        EXPECT_EQ(QApplication::focusWidget(), overlay.findChild<QPushButton*>("ScreenshotSelectionSkipButton"));
        press(Qt::Key_Return);
        EXPECT(result.image.has_value());
        EXPECT(result.image->isNull());
    }

    {
        ScreenshotSelectionOverlay overlay { create_frame(), &view };
        track(overlay, result);
        overlay.show();
        overlay.setFocus();
        QApplication::processEvents();

        // Tab stays within the overlay, going around from the last button back to the selection.
        press(Qt::Key_Backtab);
        EXPECT_EQ(QApplication::focusWidget(), overlay.findChild<QPushButton*>("ScreenshotSelectionCancelButton"));
        press(Qt::Key_Tab);
        EXPECT_EQ(QApplication::focusWidget(), &overlay);
        press(Qt::Key_Return);
        EXPECT_EQ(result.image->size(), QSize(400, 200));
    }
}

TEST_CASE(the_instructions_offer_the_same_choices)
{
    QWidget view;
    view.resize(400, 300);
    Result result;

    ScreenshotSelectionOverlay overlay { create_frame(), &view };
    track(overlay, result);
    view.show();
    overlay.findChild<QPushButton*>("ScreenshotSelectionWholeFrameButton")->click();
    EXPECT_EQ(result.image->size(), QSize(400, 200));

    ScreenshotSelectionOverlay second_overlay { create_frame(), &view };
    track(second_overlay, result);
    second_overlay.findChild<QPushButton*>("ScreenshotSelectionSkipButton")->click();
    EXPECT(result.image->isNull());

    ScreenshotSelectionOverlay third_overlay { create_frame(), &view };
    track(third_overlay, result);
    third_overlay.findChild<QPushButton*>("ScreenshotSelectionCancelButton")->click();
    EXPECT(!result.image.has_value());
    EXPECT(result.closed);
}
