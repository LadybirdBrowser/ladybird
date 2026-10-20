/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <UI/Qt/ScreenshotViewer.h>

#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>

using Ladybird::ScreenshotViewer;

static QImage create_screenshot(QSize size)
{
    QImage image(size, QImage::Format_RGB32);
    image.fill(Qt::red);
    return image;
}

static void send_mouse(QWidget& widget, QEvent::Type type, QPointF position)
{
    auto buttons = type == QEvent::MouseButtonRelease ? Qt::NoButton : Qt::LeftButton;
    auto button = type == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent event(type, position, widget.mapToGlobal(position), button, buttons, Qt::NoModifier);
    QApplication::sendEvent(&widget, &event);
}

static void click(QWidget& widget, QPointF position)
{
    send_mouse(widget, QEvent::MouseButtonPress, position);
    send_mouse(widget, QEvent::MouseButtonRelease, position);
}

TEST_CASE(a_screenshot_that_fits_is_shown_at_its_actual_size)
{
    QWidget tab;
    tab.resize(800, 600);
    ScreenshotViewer viewer { create_screenshot({ 200, 100 }), &tab };
    tab.show();

    EXPECT(viewer.fits_at_actual_size());
    EXPECT_EQ(viewer.image_rect().size(), QSizeF(200, 100));

    click(viewer, viewer.image_rect().center());
    EXPECT(!viewer.is_at_actual_size());
    EXPECT_EQ(viewer.image_rect().size(), QSizeF(200, 100));
}

TEST_CASE(a_larger_screenshot_is_fitted_and_shown_at_its_actual_size_on_a_click)
{
    QWidget tab;
    tab.resize(800, 600);
    ScreenshotViewer viewer { create_screenshot({ 2000, 1200 }), &tab };
    tab.show();

    EXPECT(!viewer.fits_at_actual_size());
    auto fitted = viewer.image_rect();
    EXPECT(fitted.width() < 800 && fitted.height() < 600);

    auto point = fitted.topLeft() + QPointF(fitted.width() / 4, fitted.height() / 4);
    click(viewer, point);
    EXPECT(viewer.is_at_actual_size());
    auto actual = viewer.image_rect();
    EXPECT_EQ(actual.size(), QSizeF(2000, 1200));
    EXPECT(actual.contains(point));
    EXPECT(actual.left() <= 0 && actual.right() >= 800);

    send_mouse(viewer, QEvent::MouseButtonPress, QPointF(400, 300));
    send_mouse(viewer, QEvent::MouseMove, QPointF(350, 250));
    send_mouse(viewer, QEvent::MouseButtonRelease, QPointF(350, 250));
    EXPECT(viewer.is_at_actual_size());
    EXPECT_EQ(viewer.image_rect().topLeft(), actual.topLeft() - QPointF(50, 50));

    click(viewer, QPointF(400, 300));
    EXPECT(!viewer.is_at_actual_size());
    EXPECT_EQ(viewer.image_rect(), fitted);
}

TEST_CASE(escape_or_a_click_beside_the_screenshot_closes_the_viewer)
{
    QWidget tab;
    tab.resize(800, 600);
    auto closes = 0;

    ScreenshotViewer viewer { create_screenshot({ 200, 100 }), &tab };
    viewer.on_closed = [&] { ++closes; };
    tab.show();
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QApplication::sendEvent(&viewer, &escape);
    EXPECT_EQ(closes, 1);
    EXPECT(!viewer.isVisible());

    ScreenshotViewer second_viewer { create_screenshot({ 200, 100 }), &tab };
    second_viewer.on_closed = [&] { ++closes; };
    second_viewer.show();
    click(second_viewer, QPointF(10, 300));
    EXPECT_EQ(closes, 2);
}
