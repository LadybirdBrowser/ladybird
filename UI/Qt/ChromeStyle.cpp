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
        ? mix(window, QColor(10, 16, 24), 0.72)
        : mix(window, QColor(241, 245, 249), 0.62);
}

QColor chrome_surface(QPalette const& palette)
{
    auto base = palette.color(QPalette::Base);
    return is_dark(palette)
        ? mix(base, QColor(31, 39, 52), 0.64)
        : mix(base, QColor(255, 255, 255), 0.72);
}

QColor chrome_surface_hover(QPalette const& palette)
{
    auto accent = palette.color(QPalette::Highlight);
    return is_dark(palette)
        ? mix(chrome_surface(palette), QColor(61, 77, 100), 0.34)
        : mix(chrome_surface(palette), accent.lighter(165), 0.22);
}

QColor chrome_surface_pressed(QPalette const& palette)
{
    auto accent = palette.color(QPalette::Highlight);
    return is_dark(palette)
        ? mix(chrome_surface(palette), accent.lighter(120), 0.32)
        : mix(chrome_surface(palette), accent.lighter(145), 0.32);
}

QColor chrome_border(QPalette const& palette)
{
    return is_dark(palette)
        ? mix(chrome_surface(palette), QColor(151, 169, 190), 0.22)
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
    auto background_bottom = style_sheet_color(mix(chrome_background(palette), QColor(3, 8, 14), is_dark(palette) ? 0.34 : 0.0));
    auto surface_hover = style_sheet_color(chrome_surface_hover(palette));
    auto surface_pressed = style_sheet_color(chrome_surface_pressed(palette));
    auto border = style_sheet_color(chrome_border(palette));
    auto separator = style_sheet_color(mix(chrome_background(palette), chrome_border(palette), is_dark(palette) ? 0.28 : 0.56));
    auto text = style_sheet_color(palette.color(QPalette::ButtonText));
    auto disabled_text = style_sheet_color(chrome_muted_text(palette));

    return QStringLiteral(R"(
QWidget#LadybirdNavigationToolbar {
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 %1, stop:1 %2);
    border: 0;
    border-bottom: 1px solid %8;
}

QWidget#LadybirdNavigationToolbar QToolButton {
    color: %6;
    background: transparent;
    border: 1px solid transparent;
    border-radius: 18px;
    min-width: 36px;
    min-height: 36px;
    padding: 0;
}

QWidget#LadybirdNavigationToolbar QToolButton:hover {
    background: %3;
    border-color: %5;
}

QWidget#LadybirdNavigationToolbar QToolButton:pressed,
QWidget#LadybirdNavigationToolbar QToolButton:checked {
    background: %4;
    border-color: %5;
}

QWidget#LadybirdNavigationToolbar QToolButton:disabled {
    color: %7;
}

QWidget#LadybirdNavigationToolbar QToolButton::menu-indicator {
    image: none;
}
)")
        .arg(background, background_bottom, surface_hover, surface_pressed, border, text, disabled_text, separator);
}

QString location_edit_style_sheet(QPalette const& palette)
{
    auto surface_color = chrome_surface(palette);
    if (is_dark(palette))
        surface_color = mix(surface_color, QColor(44, 54, 70), 0.10);
    auto hover_color = chrome_surface_hover(palette);
    if (is_dark(palette))
        hover_color = mix(hover_color, QColor(72, 88, 112), 0.08);

    auto surface = style_sheet_color(surface_color);
    auto hover = style_sheet_color(hover_color);
    auto border = style_sheet_color(chrome_border(palette));
    auto focus_border = style_sheet_color(mix(chrome_border(palette), chrome_accent(palette), is_dark(palette) ? 0.46 : 0.58));
    auto text = style_sheet_color(palette.color(QPalette::Text));
    auto placeholder = style_sheet_color(chrome_muted_text(palette));
    auto selection = style_sheet_color(chrome_accent(palette));
    auto selection_text = style_sheet_color(palette.color(QPalette::HighlightedText));

    return QStringLiteral(R"(
QLineEdit#LadybirdLocationEdit {
    color: %5;
    background: %1;
    border: 1px solid %3;
    border-radius: 20px;
    min-height: 40px;
    padding: 0 16px;
    selection-background-color: %7;
    selection-color: %8;
}

QLineEdit#LadybirdLocationEdit:hover {
    background: %2;
}

QLineEdit#LadybirdLocationEdit:focus {
    background: %2;
    border-color: %4;
}

QLineEdit#LadybirdLocationEdit:disabled {
    color: %6;
}

QToolButton#LadybirdLocationIcon {
    background: transparent;
    border: 0;
    padding: 0;
}

QToolButton#LadybirdLocationAction {
    background: transparent;
    border: 0;
    border-radius: 12px;
    padding: 0;
}

QToolButton#LadybirdLocationAction:hover {
    background: %2;
}
)")
        .arg(surface, hover, border, focus_border, text, placeholder, selection, selection_text);
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
    auto background_bottom = style_sheet_color(mix(chrome_background(palette), QColor(3, 8, 14), is_dark(palette) ? 0.26 : 0.0));
    auto hover = style_sheet_color(chrome_surface_hover(palette));
    auto pressed = style_sheet_color(chrome_surface_pressed(palette));
    auto border = style_sheet_color(chrome_border(palette));
    auto text = style_sheet_color(palette.color(QPalette::ButtonText));
    auto close_hover = style_sheet_color(QColor(196, 43, 28));
    auto close_text = style_sheet_color(QColor(255, 255, 255));

    return QStringLiteral(R"(
QWidget#LadybirdTabStrip {
    color: %5;
    background: qlineargradient(x1:0, y1:0, x2:0, y2:1, stop:0 %1, stop:1 %2);
    border: 0;
}

QToolButton#LadybirdNewTabButton,
QPushButton#LadybirdTabButton {
    color: %7;
    background: transparent;
    border: 1px solid transparent;
    border-radius: 11px;
    padding: 0;
}

QToolButton#LadybirdNewTabButton {
    min-width: 32px;
    min-height: 32px;
    border-radius: 16px;
}

QPushButton#LadybirdTabButton {
    min-width: 20px;
    min-height: 20px;
    max-width: 20px;
    max-height: 20px;
}

QToolButton#LadybirdNewTabButton:hover,
QPushButton#LadybirdTabButton:hover {
    background: %3;
    border-color: %6;
}

QToolButton#LadybirdNewTabButton:pressed,
QPushButton#LadybirdTabButton:pressed,
QPushButton#LadybirdTabButton:checked {
    background: %4;
    border-color: %6;
}

QToolButton#LadybirdWindowButton,
QToolButton#LadybirdCloseWindowButton {
    color: %7;
    background: transparent;
    border: 0;
    border-radius: 0;
    min-width: 40px;
    min-height: 40px;
    padding: 0;
}

QToolButton#LadybirdWindowButton:hover {
    background: %3;
}

QToolButton#LadybirdWindowButton:pressed {
    background: %4;
}

QToolButton#LadybirdCloseWindowButton:hover {
    color: %9;
    background: %8;
}

QToolButton#LadybirdCloseWindowButton:pressed {
    color: %9;
    background: %8;
}
)")
        .arg(background, background_bottom, hover, pressed, text, border, text, close_hover, close_text);
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
    border-radius: 8px;
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
