/*
 * Copyright (c) 2024, Ben Jilks <benjyjilks@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Function.h>

namespace TextCodec {

class Encoder {
public:
    virtual ErrorOr<void> process(Utf8View, Function<ErrorOr<void>(u8)> on_byte, Function<ErrorOr<void>(u32)> on_error) = 0;

protected:
    virtual ~Encoder() = default;
};

class UTF8Encoder final : public Encoder {
public:
    virtual ErrorOr<void> process(Utf8View, Function<ErrorOr<void>(u8)> on_byte, Function<ErrorOr<void>(u32)> on_error) override;
};

class EUCJPEncoder final : public Encoder {
public:
    virtual ErrorOr<void> process(Utf8View, Function<ErrorOr<void>(u8)> on_byte, Function<ErrorOr<void>(u32)> on_error) override;
};

class ISO2022JPEncoder final : public Encoder {
public:
    virtual ErrorOr<void> process(Utf8View, Function<ErrorOr<void>(u8)> on_byte, Function<ErrorOr<void>(u32)> on_error) override;

private:
    enum class State {
        ASCII,
        Roman,
        jis0208,
    };

    ErrorOr<State> process_item(u32 item, State, Function<ErrorOr<void>(u8)>& on_byte, Function<ErrorOr<void>(u32)>& on_error);
};

class ShiftJISEncoder final : public Encoder {
public:
    virtual ErrorOr<void> process(Utf8View, Function<ErrorOr<void>(u8)> on_byte, Function<ErrorOr<void>(u32)> on_error) override;
};

class EUCKREncoder final : public Encoder {
public:
    virtual ErrorOr<void> process(Utf8View, Function<ErrorOr<void>(u8)> on_byte, Function<ErrorOr<void>(u32)> on_error) override;
};

class Big5Encoder final : public Encoder {
public:
    virtual ErrorOr<void> process(Utf8View, Function<ErrorOr<void>(u8)> on_byte, Function<ErrorOr<void>(u32)> on_error) override;
};

class GB18030Encoder final : public Encoder {
public:
    enum class IsGBK {
        Yes,
        No,
    };

    GB18030Encoder(IsGBK is_gbk = IsGBK::No);

    virtual ErrorOr<void> process(Utf8View, Function<ErrorOr<void>(u8)> on_byte, Function<ErrorOr<void>(u32)> on_error) override;

private:
    IsGBK m_is_gbk { IsGBK::No };
};
template<Integral ArrayType = u32>
class SingleByteEncoder final : public Encoder {
public:
    SingleByteEncoder(Array<ArrayType, 128> translation_table)
        : m_translation_table(translation_table)
    {
    }

    virtual ErrorOr<void> process(Utf8View, Function<ErrorOr<void>(u8)> on_byte, Function<ErrorOr<void>(u32)> on_error) override;

private:
    Array<ArrayType, 128> m_translation_table;
};

Optional<Encoder&> encoder_for_exact_name(StringView encoding);
Optional<Encoder&> encoder_for(StringView label);

}
