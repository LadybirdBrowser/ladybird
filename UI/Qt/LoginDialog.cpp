/*
 * Copyright (c) 2025, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <UI/Qt/LoginDialog.h>

#include <QPushButton>
#include <QWidget>

namespace Ladybird {

LoginDialog::LoginDialog([[maybe_unused]] QPointer<QWidget> const& parent)
    : QDialog(parent)
{
    setWindowTitle("Sign in");
    setWindowModality(Qt::WindowModal);
    setFixedSize(300, 140);
    m_username_text = new QLineEdit(this);
    m_password_text = new QLineEdit(this);
    m_password_text->setEchoMode(QLineEdit::Password);

    m_username_label = new QLabel(this);
    m_username_label->setText("Username");
    m_username_label->setBuddy(m_username_text);
    m_password_label = new QLabel(this);
    m_password_label->setText("Password");
    m_password_label->setBuddy(m_password_text);

    m_buttons = new QDialogButtonBox(this);
    auto const* ok_button = m_buttons->addButton(QDialogButtonBox::Ok);
    auto const* cancel_button = m_buttons->addButton(QDialogButtonBox::Cancel);
    m_buttons->button(QDialogButtonBox::Ok)->setText("Sign in");
    m_buttons->button(QDialogButtonBox::Cancel)->setText("Cancel");

    connect(ok_button, &QPushButton::clicked, this, [this] { finished(DialogCode::Accepted); });
    connect(cancel_button, &QPushButton::clicked, this, [this] { finished(DialogCode::Rejected); });

    m_grid_layout = new QGridLayout(this);
    m_grid_layout->addWidget(m_username_label, 0, 0);
    m_grid_layout->addWidget(m_username_text, 0, 1);
    m_grid_layout->addWidget(m_password_label, 1, 0);
    m_grid_layout->addWidget(m_password_text, 1, 1);
    m_grid_layout->addWidget(m_buttons, 2, 0, 1, 2);
    setLayout(m_grid_layout);
}

}
