/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Format.h>
#include <AK/String.h>
#include <LibIPC/Forward.h>
#include <LibWebCommon/Export.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/webappapis.html#concept-environment-id
class WEBCOMMON_API EnvironmentId {
public:
    static EnvironmentId generate();

    EnvironmentId() = default;

    bool is_empty() const { return m_value.is_empty(); }
    String const& to_string() const { return m_value; }

    bool operator==(EnvironmentId const&) const = default;

private:
    template<typename T>
    friend ErrorOr<T> IPC::decode(IPC::Decoder&);

    explicit EnvironmentId(String value)
        : m_value(move(value))
    {
    }

    String m_value;
};

}

template<>
struct AK::Formatter<Web::HTML::EnvironmentId> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::HTML::EnvironmentId const& id)
    {
        return Formatter<StringView>::format(builder, id.to_string());
    }
};

namespace IPC {

template<>
WEBCOMMON_API ErrorOr<void> encode(Encoder&, Web::HTML::EnvironmentId const&);

template<>
WEBCOMMON_API ErrorOr<Web::HTML::EnvironmentId> decode(Decoder&);

}
