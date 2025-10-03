/*
 * PoC bridge to Pascal CSS tokenizer
 */

#pragma once

#include <AK/Types.h>

extern "C" {

enum LB_TokenType : u8 {
    LB_Invalid,
    LB_EndOfFile,
    LB_Ident,
    LB_Function,
    LB_AtKeyword,
    LB_Hash,
    LB_String,
    LB_BadString,
    LB_Url,
    LB_BadUrl,
    LB_Delim,
    LB_Number,
    LB_Percentage,
    LB_Dimension,
    LB_Whitespace,
    LB_CDO,
    LB_CDC,
    LB_Colon,
    LB_Semicolon,
    LB_Comma,
    LB_OpenSquare,
    LB_CloseSquare,
    LB_OpenParen,
    LB_CloseParen,
    LB_OpenCurly,
    LB_CloseCurly
};

enum LB_HashType : u8 {
    LB_Hash_Id,
    LB_Hash_Unrestricted
};

enum LB_NumberType : u8 {
    LB_Number_Number,
    LB_Number_IntegerWithExplicitSign,
    LB_Number_Integer
};

struct LB_CssTokenLite {
    u8 token_type;
    u8 hash_type;
    u8 number_type;
    u8 _reserved;
    u32 delim;
    u32 start_line;
    u32 start_col;
    u32 end_line;
    u32 end_col;
    double number_value;
};

typedef void (*lb_css_emit_cb)(void* userdata, const LB_CssTokenLite* token, const char* str1, size_t str1_len, const char* str2, size_t str2_len);

int lb_css_tokenize_stream(const char* input_utf8, size_t len, lb_css_emit_cb emit, void* userdata);

}
