/*
 * Copyright (c) 2025, ayeteadoe <ayeteadoe@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/GenericLexer.h>
#include <AK/String.h>
#include <AK/Variant.h>
#include <LibWGSL/Export.h>

namespace WGSL {

#define DECLARE_EQUALITY_OPERATOR(StructName) \
    bool operator==(const StructName&) const = default;

// https://www.w3.org/TR/WGSL/#literal
struct LiteralToken {
    DECLARE_EQUALITY_OPERATOR(LiteralToken);
    enum class Value : u8 {
        Int, // https://www.w3.org/TR/WGSL/#integer-literal
    };
    Value value;
};

// https://www.w3.org/TR/WGSL/#keyword
struct KeywordToken {
    DECLARE_EQUALITY_OPERATOR(KeywordToken);
    enum class Value : u8 {
        Struct,
        Fn,
        Var,
        Return,
    };
    Value value;
};

// https://www.w3.org/TR/WGSL/#identifiers
struct IdentifierToken {
    DECLARE_EQUALITY_OPERATOR(IdentifierToken);
    String value;
};

// https://www.w3.org/TR/WGSL/#types

struct TypeToken {
    DECLARE_EQUALITY_OPERATOR(TypeToken);
    enum class Value : u8 {
        // https://www.w3.org/TR/WGSL/#vector-types
        vec3f,
        vec4f,
    };
    Value value;
};

// https://www.w3.org/TR/WGSL/#syntactic-token
struct SyntacticToken {
    DECLARE_EQUALITY_OPERATOR(SyntacticToken);
    enum class Value : u8 {
        OpenParen,  // (
        CloseParen, // )
        OpenBrace,  // {
        CloseBrace, // }
        Semicolon,  // ;
        Comma,      // ,
        Colon,      // :
        Dot,        // .
        Arrow,      // ->
        Equals,     // =
        At,         // @
    };
    Value value;
};

// https://www.w3.org/TR/WGSL/#context-dependent-name

// https://www.w3.org/TR/WGSL/#attributes
struct BuiltinAttribute {
    DECLARE_EQUALITY_OPERATOR(BuiltinAttribute);
    enum class Flags : u32 {
        Position = 0,
    };
    Flags value;
};
AK_ENUM_BITWISE_OPERATORS(BuiltinAttribute::Flags)
struct LocationAttribute {
    DECLARE_EQUALITY_OPERATOR(LocationAttribute);
    u32 value;
};
struct VertexAttribute {
    DECLARE_EQUALITY_OPERATOR(VertexAttribute);
};
struct FragmentAttribute {
    DECLARE_EQUALITY_OPERATOR(FragmentAttribute);
};
using AttributeToken = Variant<BuiltinAttribute, LocationAttribute, VertexAttribute, FragmentAttribute>;

struct EndOfFileToken {
    DECLARE_EQUALITY_OPERATOR(EndOfFileToken);
};

struct InvalidToken {
    DECLARE_EQUALITY_OPERATOR(InvalidToken);
    ByteString error_message;
};

using TokenType = Variant<InvalidToken, EndOfFileToken, SyntacticToken, TypeToken, IdentifierToken, KeywordToken, LiteralToken, AttributeToken>;

struct WGSL_API Token {
    DECLARE_EQUALITY_OPERATOR(Token);
    TokenType type;
    size_t position;
    size_t line;
    size_t column;

    String to_string() const;
};

class WGSL_API Lexer {
public:
    explicit Lexer(StringView processed_text);

    Token next_token();

private:
    GenericLexer m_lexer;
    size_t m_current_line = 1;
    size_t m_current_column = 1;

    void skip_blankspace();

    Token tokenize_integer_literal();
    Token tokenize_float_literal();
    Token tokenize_attribute(StringView name, size_t start_pos, size_t start_line, size_t start_column);
    Token tokenize(StringView text, size_t start_pos, size_t start_line, size_t start_column);
};

#undef DECLARE_EQUALITY_OPERATOR

}
