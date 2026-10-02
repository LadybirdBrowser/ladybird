/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/ColorPicker.h>

#include <QColorDialog>

namespace Ladybird {

ColorPicker::ColorPicker(QWidget& parent)
    : QObject(&parent)
    , m_parent(parent)
{
}

bool ColorPicker::is_open() const
{
    return m_dialog;
}

void ColorPicker::open(Color current_color)
{
    reset();

    auto dialog = QPointer<QColorDialog> { new QColorDialog(QColor(current_color.red(), current_color.green(), current_color.blue()), &m_parent) };
    m_dialog = dialog;

    dialog->setWindowTitle("Ladybird");
    dialog->setOption(QColorDialog::ShowAlphaChannel, false);
    QObject::connect(dialog, &QColorDialog::currentColorChanged, this, [this, dialog](QColor const& color) {
        if (m_dialog != dialog)
            return;
        if (on_update)
            on_update(Color(color.red(), color.green(), color.blue()), Web::HTML::ColorPickerUpdateState::Update);
    });

    QObject::connect(dialog, &QDialog::finished, this, [this, dialog](auto result) {
        if (!dialog || m_dialog != dialog)
            return;

        Optional<Color> color;
        if (result == QDialog::Accepted)
            color = Color(dialog->selectedColor().red(), dialog->selectedColor().green(), dialog->selectedColor().blue());

        m_dialog = nullptr;
        dialog->deleteLater();
        if (on_update)
            on_update(color, Web::HTML::ColorPickerUpdateState::Closed);
    });

    dialog->open();
}

void ColorPicker::reset()
{
    if (!m_dialog)
        return;

    auto dialog = m_dialog;
    m_dialog = nullptr;
    QObject::disconnect(dialog, nullptr, this, nullptr);
    dialog->close();
    dialog->deleteLater();
}

}
