/*
 * Copyright (c) 2025, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <UI/Qt/StringUtils.h>

#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>

namespace Ladybird {

enum class SignInResult : u8 {
    Cancel,
    Ok,
};

class LoginDialog final : public QDialog {
    Q_OBJECT

public:
    explicit LoginDialog(QPointer<QWidget> const& parent);

    String username() const { return ak_string_from_qstring(m_username_text->text()); }
    String password() const { return ak_string_from_qstring(m_password_text->text()); }

private:
    QPointer<QLabel> m_username_label { nullptr };
    QPointer<QLabel> m_password_label { nullptr };
    QPointer<QLineEdit> m_username_text { nullptr };
    QPointer<QLineEdit> m_password_text { nullptr };
    QPointer<QGridLayout> m_grid_layout { nullptr };
    QPointer<QDialogButtonBox> m_buttons = { nullptr };
};

}
