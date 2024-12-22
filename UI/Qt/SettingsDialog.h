/*
 * Copyright (c) 2022, Filiph Sandström <filiph.sandstrom@filfatstudios.com>
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <QCheckBox>
#include <QDialog>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPushButton>
#include <qslider.h>

#pragma once

namespace Ladybird {

class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(QMainWindow* window);

private:
    void setup_search_engines();

    QFormLayout* m_layout;
    QMainWindow* m_window { nullptr };
    QLineEdit* m_new_tab_page { nullptr };
    QCheckBox* m_enable_search { nullptr };
    QPushButton* m_search_engine_dropdown { nullptr };
    QLineEdit* m_preferred_languages { nullptr };
    QCheckBox* m_enable_autocomplete { nullptr };
    QPushButton* m_autocomplete_engine_dropdown { nullptr };
    QCheckBox* m_enable_do_not_track { nullptr };
    QCheckBox* m_enable_autoplay { nullptr };
    QPushButton* m_reset_scrolling_speed { nullptr };
    QLabel* m_scrolling_speed_label { nullptr };
    QSlider* m_scrolling_speed { nullptr };
    QCheckBox* m_invert_vertical_scrolling { nullptr };
    QCheckBox* m_invert_horizontal_scrolling { nullptr };
};

}
