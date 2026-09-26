/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <LibGfx/Color.h>
#include <LibWeb/HTML/ColorPickerUpdateState.h>

#include <QObject>
#include <QPointer>

class QColorDialog;
class QWidget;

namespace Ladybird {

class ColorPicker final : public QObject {
public:
    explicit ColorPicker(QWidget& parent);

    bool is_open() const;

    void open(Color current_color);
    void reset();

    Function<void(Optional<Color>, Web::HTML::ColorPickerUpdateState)> on_update;

private:
    QWidget& m_parent;
    QPointer<QColorDialog> m_dialog;
};

}
