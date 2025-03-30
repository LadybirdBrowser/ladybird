/*
 * Copyright (c) 2022, Filiph Sandström <filiph.sandstrom@filfatstudios.com>
 * Copyright (c) 2023, Cameron Youell <cameronyouell@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibWebView/Application.h>
#include <UI/Qt/Settings.h>
#include <UI/Qt/SettingsDialog.h>
#include <UI/Qt/StringUtils.h>

#include <QLabel>
#include <QMenu>

namespace Ladybird {

SettingsDialog::SettingsDialog(QMainWindow* window)
    : QDialog(window)
    , m_window(window)
{
    m_layout = new QFormLayout(this);

    m_preferred_languages = new QLineEdit(this);
    m_preferred_languages->setText(Settings::the()->preferred_languages().join(","));
    QObject::connect(m_preferred_languages, &QLineEdit::editingFinished, this, [this] {
        Settings::the()->set_preferred_languages(m_preferred_languages->text().split(","));
    });
    QObject::connect(m_preferred_languages, &QLineEdit::returnPressed, this, [this] {
        close();
    });

    m_enable_do_not_track = new QCheckBox(this);
    m_enable_do_not_track->setChecked(Settings::the()->enable_do_not_track());
#if (QT_VERSION > QT_VERSION_CHECK(6, 7, 0))
    QObject::connect(m_enable_do_not_track, &QCheckBox::checkStateChanged, this, [&](int state) {
#else
    QObject::connect(m_enable_do_not_track, &QCheckBox::stateChanged, this, [&](int state) {
#endif
        Settings::the()->set_enable_do_not_track(state == Qt::Checked);
    });

    m_layout->addRow(new QLabel("Preferred Language(s)", this), m_preferred_languages);
    m_layout->addRow(new QLabel("Send web sites a \"Do Not Track\" request", this), m_enable_do_not_track);

    setWindowTitle("Settings");
    setLayout(m_layout);
    resize(600, 250);
}

}
