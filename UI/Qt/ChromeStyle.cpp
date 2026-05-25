/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/ChromeStyle.h>

namespace Ladybird::ChromeStyle {

static bool is_dark(QPalette const& palette)
{
    return palette.color(QPalette::Window).lightness() < 128;
}

static QColor mix(QColor const& from, QColor const& to, double amount)
{
    auto channel = [&](int from_channel, int to_channel) {
        return static_cast<int>(from_channel + (to_channel - from_channel) * amount);
    };

    return QColor {
        channel(from.red(), to.red()),
        channel(from.green(), to.green()),
        channel(from.blue(), to.blue()),
    };
}

QColor chrome_background(QPalette const& palette)
{
    auto window = palette.color(QPalette::Window);
    return is_dark(palette)
        ? mix(window, QColor(18, 22, 28), 0.42)
        : mix(window, QColor(240, 244, 248), 0.58);
}

QColor chrome_surface(QPalette const& palette)
{
    auto base = palette.color(QPalette::Base);
    return is_dark(palette)
        ? mix(base, QColor(40, 45, 54), 0.46)
        : mix(base, QColor(255, 255, 255), 0.72);
}

QColor chrome_surface_hover(QPalette const& palette)
{
    auto accent = palette.color(QPalette::Highlight);
    return is_dark(palette)
        ? mix(chrome_surface(palette), accent.lighter(135), 0.18)
        : mix(chrome_surface(palette), accent.lighter(165), 0.22);
}

QColor chrome_surface_pressed(QPalette const& palette)
{
    auto accent = palette.color(QPalette::Highlight);
    return is_dark(palette)
        ? mix(chrome_surface(palette), accent.lighter(125), 0.28)
        : mix(chrome_surface(palette), accent.lighter(145), 0.32);
}

QColor chrome_border(QPalette const& palette)
{
    return is_dark(palette)
        ? mix(chrome_surface(palette), QColor(255, 255, 255), 0.16)
        : mix(chrome_background(palette), QColor(92, 105, 120), 0.22);
}

QColor chrome_accent(QPalette const& palette)
{
    return palette.color(QPalette::Highlight);
}

QColor chrome_muted_text(QPalette const& palette)
{
    return palette.color(QPalette::PlaceholderText);
}

QString style_sheet_color(QColor const& color)
{
    return QStringLiteral("rgb(%1, %2, %3)").arg(color.red()).arg(color.green()).arg(color.blue());
}

QString navigation_toolbar_style_sheet(QPalette const& palette)
{
    auto background = style_sheet_color(chrome_background(palette));
    auto surface_hover = style_sheet_color(chrome_surface_hover(palette));
    auto surface_pressed = style_sheet_color(chrome_surface_pressed(palette));
    auto border = style_sheet_color(chrome_border(palette));
    auto text = style_sheet_color(palette.color(QPalette::ButtonText));
    auto disabled_text = style_sheet_color(chrome_muted_text(palette));

    return QStringLiteral(R"(
QToolBar#LadybirdNavigationToolbar {
    background: %1;
    border: 0;
    border-bottom: 1px solid %4;
    padding: 5px 8px 6px 8px;
    spacing: 6px;
}

QToolBar#LadybirdNavigationToolbar QToolButton {
    color: %5;
    background: transparent;
    border: 1px solid transparent;
    border-radius: 6px;
    min-width: 30px;
    min-height: 30px;
    padding: 0 5px;
}

QToolBar#LadybirdNavigationToolbar QToolButton:hover {
    background: %2;
    border-color: %4;
}

QToolBar#LadybirdNavigationToolbar QToolButton:pressed,
QToolBar#LadybirdNavigationToolbar QToolButton:checked {
    background: %3;
    border-color: %4;
}

QToolBar#LadybirdNavigationToolbar QToolButton:disabled {
    color: %6;
}

QToolBar#LadybirdNavigationToolbar QToolButton::menu-indicator {
    image: none;
}
)")
        .arg(background, surface_hover, surface_pressed, border, text, disabled_text);
}

QString location_edit_style_sheet(QPalette const& palette)
{
    auto surface = style_sheet_color(chrome_surface(palette));
    auto hover = style_sheet_color(chrome_surface_hover(palette));
    auto border = style_sheet_color(chrome_border(palette));
    auto accent = style_sheet_color(chrome_accent(palette));
    auto text = style_sheet_color(palette.color(QPalette::Text));
    auto placeholder = style_sheet_color(chrome_muted_text(palette));
    auto selection = style_sheet_color(chrome_accent(palette));
    auto selection_text = style_sheet_color(palette.color(QPalette::HighlightedText));

    return QStringLiteral(R"(
QLineEdit#LadybirdLocationEdit {
    color: %5;
    background: %1;
    border: 1px solid %3;
    border-radius: 8px;
    min-height: 30px;
    padding: 3px 12px;
    selection-background-color: %7;
    selection-color: %8;
}

QLineEdit#LadybirdLocationEdit:hover {
    background: %2;
}

QLineEdit#LadybirdLocationEdit:focus {
    background: %1;
    border-color: %4;
}

QLineEdit#LadybirdLocationEdit:disabled {
    color: %6;
}
)")
        .arg(surface, hover, border, accent, text, placeholder, selection, selection_text);
}

QString bookmarks_bar_style_sheet(QPalette const& palette)
{
    auto background = style_sheet_color(chrome_background(palette));
    auto hover = style_sheet_color(chrome_surface_hover(palette));
    auto pressed = style_sheet_color(chrome_surface_pressed(palette));
    auto border = style_sheet_color(chrome_border(palette));
    auto text = style_sheet_color(palette.color(QPalette::ButtonText));

    return QStringLiteral(R"(
QToolBar#LadybirdBookmarksBar {
    color: %5;
    background: %1;
    border: 0;
    border-bottom: 1px solid %4;
    padding: 4px 8px;
    spacing: 3px;
}

QToolBar#LadybirdBookmarksBar QToolButton {
    color: %5;
    background: transparent;
    border: 1px solid transparent;
    border-radius: 7px;
    padding: 4px 7px;
}

QToolBar#LadybirdBookmarksBar QToolButton:hover {
    background: %2;
    border-color: %4;
}

QToolBar#LadybirdBookmarksBar QToolButton:pressed,
QToolBar#LadybirdBookmarksBar QToolButton:checked {
    background: %3;
    border-color: %4;
}
)")
        .arg(background, hover, pressed, border, text);
}

QString find_in_page_style_sheet(QPalette const& palette)
{
    auto background = style_sheet_color(chrome_background(palette));
    auto surface = style_sheet_color(chrome_surface(palette));
    auto hover = style_sheet_color(chrome_surface_hover(palette));
    auto pressed = style_sheet_color(chrome_surface_pressed(palette));
    auto border = style_sheet_color(chrome_border(palette));
    auto accent = style_sheet_color(chrome_accent(palette));
    auto text = style_sheet_color(palette.color(QPalette::Text));
    auto muted = style_sheet_color(chrome_muted_text(palette));

    return QStringLiteral(R"(
QWidget#LadybirdFindInPageBar {
    background: %1;
    border-top: 1px solid %5;
}

QWidget#LadybirdFindInPageBar QLineEdit {
    color: %7;
    background: %2;
    border: 1px solid %5;
    border-radius: 8px;
    min-height: 26px;
    padding: 2px 9px;
    selection-background-color: %6;
}

QWidget#LadybirdFindInPageBar QLineEdit:focus {
    border-color: %6;
}

QWidget#LadybirdFindInPageBar QPushButton {
    color: %7;
    background: transparent;
    border: 1px solid transparent;
    border-radius: 7px;
    min-height: 26px;
}

QWidget#LadybirdFindInPageBar QPushButton:hover {
    background: %3;
    border-color: %5;
}

QWidget#LadybirdFindInPageBar QPushButton:pressed {
    background: %4;
    border-color: %5;
}

QWidget#LadybirdFindInPageBar QCheckBox,
QWidget#LadybirdFindInPageBar QLabel {
    color: %8;
}
)")
        .arg(background, surface, hover, pressed, border, accent, text, muted);
}

QString tab_widget_style_sheet(QPalette const& palette)
{
    auto background = style_sheet_color(chrome_background(palette));
    auto hover = style_sheet_color(chrome_surface_hover(palette));
    auto pressed = style_sheet_color(chrome_surface_pressed(palette));
    auto border = style_sheet_color(chrome_border(palette));
    auto text = style_sheet_color(palette.color(QPalette::ButtonText));
    auto close_hover = style_sheet_color(QColor(196, 43, 28));
    auto close_text = style_sheet_color(QColor(255, 255, 255));

    return QStringLiteral(R"(
QWidget#LadybirdTabStrip {
    color: %5;
    background: %1;
    border: 0;
    border-bottom: 1px solid %4;
}

QToolButton#LadybirdNewTabButton,
QPushButton#LadybirdTabButton {
    color: %5;
    background: transparent;
    border: 1px solid transparent;
    border-radius: 6px;
    min-width: 24px;
    min-height: 24px;
    padding: 0;
}

QToolButton#LadybirdNewTabButton:hover,
QPushButton#LadybirdTabButton:hover {
    background: %2;
    border-color: %4;
}

QToolButton#LadybirdNewTabButton:pressed,
QPushButton#LadybirdTabButton:pressed,
QPushButton#LadybirdTabButton:checked {
    background: %3;
    border-color: %4;
}

QToolButton#LadybirdWindowButton,
QToolButton#LadybirdCloseWindowButton {
    color: %5;
    background: transparent;
    border: 0;
    border-radius: 0;
    min-width: 40px;
    min-height: 32px;
    padding: 0;
}

QToolButton#LadybirdWindowButton:hover {
    background: %2;
}

QToolButton#LadybirdWindowButton:pressed {
    background: %3;
}

QToolButton#LadybirdCloseWindowButton:hover {
    color: %7;
    background: %6;
}

QToolButton#LadybirdCloseWindowButton:pressed {
    color: %7;
    background: %6;
}
)")
        .arg(background, hover, pressed, border, text, close_hover, close_text);
}

QString autocomplete_popup_style_sheet(QPalette const& palette)
{
    auto surface = style_sheet_color(chrome_surface(palette));
    auto border = style_sheet_color(chrome_border(palette));
    auto text = style_sheet_color(palette.color(QPalette::Text));

    return QStringLiteral(R"(
QFrame#LadybirdAutocompletePopup {
    color: %3;
    background: %1;
    border: 1px solid %2;
    border-radius: 10px;
}

QListView#LadybirdAutocompleteList {
    color: %3;
    background: transparent;
    border: 0;
    outline: 0;
}
)")
        .arg(surface, border, text);
}

}
