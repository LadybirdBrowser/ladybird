/*
 * Copyright (c) 2022, Filiph Sandström <filiph.sandstrom@filfatstudios.com>
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <QCheckBox>
#include <QDialog>
#include <QFormLayout>
#include <QLineEdit>
#include <QMainWindow>
#include <QPushButton>

namespace Ladybird {

class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(QMainWindow* window);

private:
    QFormLayout* m_layout;
    QMainWindow* m_window { nullptr };
    QLineEdit* m_preferred_languages { nullptr };
};

}
