{*
  Minimal CSS Tokenizer in FreePascal (PoC)
  - UTF-8 input expected (already normalized by C++ filter)
  - Exports a C-friendly streaming API to emit tokens
  - Not a complete spec implementation; focuses on common tokens
*}

{$mode objfpc}{$H-}

unit css_tokenizer;

interface

{$PACKRECORDS C}

type
  LB_TokenType = (
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
  );

  LB_HashType = (
    LB_Hash_Id,
    LB_Hash_Unrestricted
  );

  LB_NumberType = (
    LB_Number_Number,
    LB_Number_IntegerWithExplicitSign,
    LB_Number_Integer
  );

  PLB_CssTokenLite = ^LB_CssTokenLite;
  LB_CssTokenLite = packed record
    token_type : Byte;      { LB_TokenType }
    hash_type  : Byte;      { LB_HashType }
    number_type: Byte;      { LB_NumberType }
    _reserved  : Byte;
    delim      : Cardinal;  { Unicode scalar value }
    start_line : Cardinal;
    start_col  : Cardinal;
    end_line   : Cardinal;
    end_col    : Cardinal;
    number_value : Double;  { for Number/Percentage/Dimension }
  end;

  PLB_EmitCb = procedure(userdata: Pointer; const token: PLB_CssTokenLite; str1: PChar; str1_len: SizeUInt; str2: PChar; str2_len: SizeUInt); cdecl;

{$if defined(DARWIN)}
function lb_css_tokenize_stream(const input_utf8: PChar; input_len: SizeUInt; emit_cb: PLB_EmitCb; userdata: Pointer): Integer; cdecl; public name '_lb_css_tokenize_stream';
{$else}
function lb_css_tokenize_stream(const input_utf8: PChar; input_len: SizeUInt; emit_cb: PLB_EmitCb; userdata: Pointer): Integer; cdecl; public name 'lb_css_tokenize_stream';
{$endif}

implementation

var
  PInput    : PChar;
  InputLen  : SizeUInt;
  Index     : SizeUInt;
  LineNo    : Cardinal;
  ColNo     : Cardinal;
  ScratchBuf: array[0..262143] of Char;
const
  SCRATCH_CAP = 262144;

const
  REPLACEMENT_CHARACTER = $FFFD;

{ Provide a local implementation of the only shortstring helper the
  compiler may reference under $H-, so we don't need to link the
  Pascal RTL. }
function fpc_shortstr_compare_equal(const a, b: ShortString): Boolean; cdecl; public name 'fpc_shortstr_compare_equal';
var i: Integer;
begin
  if Length(a) <> Length(b) then begin
    fpc_shortstr_compare_equal := False;
    exit;
  end;
  for i := 1 to Length(a) do begin
    if a[i] <> b[i] then begin
      fpc_shortstr_compare_equal := False;
      exit;
    end;
  end;
  fpc_shortstr_compare_equal := True;
end;

procedure ZeroToken(out tok: LB_CssTokenLite); inline;
begin
  tok.token_type := 0;
  tok.hash_type := 0;
  tok.number_type := 0;
  tok._reserved := 0;
  tok.delim := 0;
  tok.start_line := 0;
  tok.start_col := 0;
  tok.end_line := 0;
  tok.end_col := 0;
  tok.number_value := 0.0;
end;

function AtEOF: Boolean; inline;
begin
  AtEOF := Index >= InputLen;
end;

// (duplicate helper declarations removed; actual implementations are below)

function PeekByte(offset: SizeUInt = 0): Integer; inline;
begin
  if (Index + offset) >= InputLen then
    exit(-1);
  PeekByte := Ord(PInput[Index + offset]);
end;

function IsNewline(b: Integer): Boolean; inline;
begin
  IsNewline := (b = 10);
end;

function IsHexDigit(b: Integer): Boolean; inline;
begin
  IsHexDigit := ((b >= Ord('0')) and (b <= Ord('9'))) or ((b >= Ord('A')) and (b <= Ord('F'))) or ((b >= Ord('a')) and (b <= Ord('f')));
end;

function HexVal(b: Integer): Integer; inline;
begin
  if (b >= Ord('0')) and (b <= Ord('9')) then exit(b - Ord('0'));
  if (b >= Ord('A')) and (b <= Ord('F')) then exit(10 + b - Ord('A'));
  if (b >= Ord('a')) and (b <= Ord('f')) then exit(10 + b - Ord('a'));
  exit(0);
end;

function IsValidEscapeSequence(b1, b2: Integer): Boolean; inline;
begin
  // A valid escape is a backslash not followed by a newline
  IsValidEscapeSequence := (b1 = 92) and (b2 >= 0) and (not IsNewline(b2));
end;

function WouldStartAnIdentSequence(b1, b2, b3: Integer): Boolean; inline;
begin
  if (b1 = Ord('-')) then begin
    if ((b2 >= Ord('A')) and (b2 <= Ord('Z'))) or ((b2 >= Ord('a')) and (b2 <= Ord('z'))) or (b2 = Ord('_')) or (b2 >= 128) then
      exit(True);
    if (b2 = Ord('-')) then exit(True);
    exit(IsValidEscapeSequence(b2, b3));
  end;
  if ((b1 >= Ord('A')) and (b1 <= Ord('Z'))) or ((b1 >= Ord('a')) and (b1 <= Ord('z'))) or (b1 = Ord('_')) or (b1 >= 128) then
    exit(True);
  if (b1 = 92) then
    exit(IsValidEscapeSequence(b1, b2));
  exit(False);
end;

function WouldStartANumber(b1, b2, b3: Integer): Boolean; inline;
begin
  if (b1 = Ord('+')) or (b1 = Ord('-')) then begin
    if (b2 >= Ord('0')) and (b2 <= Ord('9')) then exit(True);
    if (b2 = Ord('.')) and (b3 >= Ord('0')) and (b3 <= Ord('9')) then exit(True);
    exit(False);
  end;
  if (b1 = Ord('.')) and (b2 >= Ord('0')) and (b2 <= Ord('9')) then exit(True);
  if (b1 >= Ord('0')) and (b1 <= Ord('9')) then exit(True);
  exit(False);
end;

// helper implementations are placed later
procedure AdvanceBytes(n: SizeUInt = 1); inline;
var i: SizeUInt; ch: Char;
begin
  for i := 1 to n do begin
    if Index >= InputLen then exit;
    ch := PInput[Index];
    Inc(Index);
    if ch = #10 then begin
      Inc(LineNo);
      ColNo := 0;
    end else begin
      Inc(ColNo);
    end;
  end;
end;

function IsWhitespace(b: Integer): Boolean; inline;
begin
  case b of
    9, 10, 12, 13, 32: IsWhitespace := True; { \t, \n, \f, \r, space }
  else
    IsWhitespace := False;
  end;
end;

function IsNameStart(b: Integer): Boolean; inline;
begin
  if (b < 0) then exit(False);
  if (b >= Ord('A')) and (b <= Ord('Z')) then exit(True);
  if (b >= Ord('a')) and (b <= Ord('z')) then exit(True);
  if (b = Ord('_')) then exit(True);
  if (b >= 128) then exit(True); { treat non-ASCII as name-start }
  IsNameStart := False;
end;

function IsNameChar(b: Integer): Boolean; inline;
begin
  if (b < 0) then exit(False);
  if IsNameStart(b) then exit(True);
  if (b >= Ord('0')) and (b <= Ord('9')) then exit(True);
  if (b = Ord('-')) then exit(True);
  IsNameChar := False;
end;

function IsDigit(b: Integer): Boolean; inline;
begin
  IsDigit := (b >= Ord('0')) and (b <= Ord('9'));
end;

procedure EmitBytes(emit_cb: PLB_EmitCb; user: Pointer; const token: LB_CssTokenLite; p1: PChar; l1: SizeUInt; p2: PChar; l2: SizeUInt);
begin
  emit_cb(user, @token, p1, l1, p2, l2);
end;

procedure ConsumeComment();
begin
  { assumes current is '/' and next '*' }
  AdvanceBytes(2);
  while not AtEOF do begin
    if (PeekByte() = Ord('*')) and (PeekByte(1) = Ord('/')) then begin
      AdvanceBytes(2);
      exit;
    end;
    AdvanceBytes(1);
  end;
end;

procedure ConsumeWhileName(out start_off: SizeUInt; out name_len: SizeUInt);
var b: Integer;
begin
  start_off := Index;
  b := PeekByte();
  while IsNameChar(b) do begin
    AdvanceBytes(1);
    b := PeekByte();
  end;
  name_len := Index - start_off;
end;

procedure AppendUTF8FromCodePoint_Impl(var buf: PChar; var out_len: SizeUInt; code: Cardinal);
begin
  if (code <= $7F) then begin
    buf[out_len] := Char(code);
    Inc(out_len);
  end else if (code <= $7FF) then begin
    buf[out_len] := Char($C0 or ((code shr 6) and $1F)); Inc(out_len);
    buf[out_len] := Char($80 or (code and $3F)); Inc(out_len);
  end else if (code <= $FFFF) then begin
    if (code >= $D800) and (code <= $DFFF) then code := REPLACEMENT_CHARACTER;
    buf[out_len] := Char($E0 or ((code shr 12) and $0F)); Inc(out_len);
    buf[out_len] := Char($80 or ((code shr 6) and $3F)); Inc(out_len);
    buf[out_len] := Char($80 or (code and $3F)); Inc(out_len);
  end else begin
    if (code > $10FFFF) then code := REPLACEMENT_CHARACTER;
    buf[out_len] := Char($F0 or ((code shr 18) and $07)); Inc(out_len);
    buf[out_len] := Char($80 or ((code shr 12) and $3F)); Inc(out_len);
    buf[out_len] := Char($80 or ((code shr 6) and $3F)); Inc(out_len);
    buf[out_len] := Char($80 or (code and $3F)); Inc(out_len);
  end;
end;

function ConsumeEscapedCodePoint_Impl(): Cardinal;
var b: Integer; val: Cardinal; digits: Integer;
begin
  b := PeekByte();
  if IsHexDigit(b) then begin
    val := 0; digits := 0;
    while IsHexDigit(PeekByte()) and (digits < 6) do begin
      val := (val shl 4) or HexVal(PeekByte());
      AdvanceBytes(1);
      Inc(digits);
    end;
    if (PeekByte() = Ord(' ')) then AdvanceBytes(1);
    if (val = 0) or (val > $10FFFF) or ((val >= $D800) and (val <= $DFFF)) then
      exit(REPLACEMENT_CHARACTER)
    else
      exit(val);
  end;
  if (b < 0) then exit(REPLACEMENT_CHARACTER);
  AdvanceBytes(1);
  exit(Cardinal(b));
end;

procedure ConsumeIdentSequence(out out_ptr: PChar; out out_len: SizeUInt);
var start_scan: SizeUInt; had_escape: Boolean; p: SizeUInt; cp: Cardinal; buf: PChar;
begin
  start_scan := Index;
  had_escape := False;
  p := Index;
  while p < InputLen do begin
    if IsNameChar(Ord(PInput[p])) then begin
      Inc(p);
      continue;
    end;
    if (PInput[p] = '\\') and IsValidEscapeSequence(Ord(PInput[p]), Ord(PInput[p+1])) then begin
      had_escape := True;
      break;
    end;
    break;
  end;
  if (not had_escape) then begin
    out_ptr := @PInput[start_scan];
    out_len := p - start_scan;
    Index := p;
    exit;
  end;
  // Use static scratch buffer to avoid linking FPC memory manager
  buf := @ScratchBuf[0];
  out_len := 0;
  while (Index < InputLen) do begin
    if IsNameChar(PeekByte()) then begin
      if out_len < SCRATCH_CAP then begin buf[out_len] := PInput[Index]; Inc(out_len); end;
      AdvanceBytes(1);
      continue;
    end;
    if (PeekByte() = 92) and IsValidEscapeSequence(PeekByte(), PeekByte(1)) then begin
      AdvanceBytes(1);
      cp := ConsumeEscapedCodePoint_Impl();
      if out_len + 4 < SCRATCH_CAP then
        AppendUTF8FromCodePoint_Impl(buf, out_len, cp);
      continue;
    end;
    break;
  end;
  out_ptr := buf;
  { buffer is static, no allocation }
end;

function ConsumeNumber(out num: Double; out is_integer: Boolean; out had_explicit_sign: Boolean): Boolean;
var start: SizeUInt; seen_dot, seen_exp: Boolean; b: Integer; has_digit: Boolean; i: SizeUInt; sign: Integer; int_part: Int64; frac_part: Double; frac_div: Double; exp_sign: Integer; exp_val: Integer;
begin
  start := Index;
  seen_dot := False; seen_exp := False; has_digit := False;
  num := 0.0; is_integer := False;
  had_explicit_sign := False;

  b := PeekByte();
  sign := 1;
  if (b = Ord('+')) or (b = Ord('-')) then begin had_explicit_sign := True; if b = Ord('-') then sign := -1; AdvanceBytes(1); b := PeekByte(); end;

  while IsDigit(b) do begin AdvanceBytes(1); b := PeekByte(); has_digit := True; end;

  if (b = Ord('.')) and IsDigit(PeekByte(1)) then begin
    seen_dot := True;
    AdvanceBytes(1); { '.' }
    b := PeekByte();
    while IsDigit(b) do begin AdvanceBytes(1); b := PeekByte(); has_digit := True; end;
  end;

  if (b = Ord('e')) or (b = Ord('E')) then begin
    { exponent: e[+/-]?digits }
    if IsDigit(PeekByte(1)) or (((PeekByte(1) = Ord('+')) or (PeekByte(1) = Ord('-'))) and IsDigit(PeekByte(2))) then begin
      seen_exp := True;
      AdvanceBytes(1); { e }
      if (PeekByte() = Ord('+')) or (PeekByte() = Ord('-')) then AdvanceBytes(1);
      b := PeekByte();
      while IsDigit(b) do begin AdvanceBytes(1); b := PeekByte(); has_digit := True; end;
    end;
  end;

  if not has_digit then begin
    num := 0.0; is_integer := False; exit(False);
  end;

  { simple parser: parse integer and fractional parts manually }
  i := start;
  if (PInput[i] = '+') or (PInput[i] = '-') then Inc(i);
  int_part := 0;
  while (i < Index) and (PInput[i] >= '0') and (PInput[i] <= '9') do begin
    int_part := int_part * 10 + Ord(PInput[i]) - Ord('0');
    Inc(i);
  end;
  frac_part := 0.0; frac_div := 1.0;
  if (i < Index) and (PInput[i] = '.') then begin
    Inc(i);
    while (i < Index) and (PInput[i] >= '0') and (PInput[i] <= '9') do begin
      frac_part := frac_part * 10 + Ord(PInput[i]) - Ord('0');
      frac_div := frac_div * 10.0;
      Inc(i);
    end;
  end;
  num := sign * (int_part + frac_part / frac_div);
  if (i < Index) and ((PInput[i] = 'e') or (PInput[i] = 'E')) then begin
    Inc(i);
    exp_sign := 1; exp_val := 0;
    if (i < Index) and ((PInput[i] = '+') or (PInput[i] = '-')) then begin if PInput[i] = '-' then exp_sign := -1; Inc(i); end;
    while (i < Index) and (PInput[i] >= '0') and (PInput[i] <= '9') do begin
      exp_val := exp_val * 10 + Ord(PInput[i]) - Ord('0');
      Inc(i);
    end;
    while exp_val > 0 do begin
      if exp_sign > 0 then num := num * 10.0 else num := num / 10.0;
      Dec(exp_val);
    end;
  end;
  is_integer := not seen_dot and not seen_exp;
  ConsumeNumber := True;
end;

procedure TokenizeInternal(emit_cb: PLB_EmitCb; user: Pointer);
var t: LB_CssTokenLite; b, b2, b3: Integer; start_line, start_col: Cardinal; num: Double; is_int: Boolean; name_start, name_len: SizeUInt; str_start, str_len: SizeUInt;
    name_ptr, hash_ptr, ident_ptr: PChar; hash_len, ident_len: SizeUInt; had_sign: Boolean;
begin
  while true do begin
    if AtEOF then begin
      ZeroToken(t);
      t.token_type := Ord(LB_EndOfFile);
      t.start_line := LineNo; t.start_col := ColNo;
      t.end_line := LineNo; t.end_col := ColNo;
      EmitBytes(emit_cb, user, t, nil, 0, nil, 0);
      exit;
    end;

    b := PeekByte();
    b2 := PeekByte(1);
    b3 := PeekByte(2);
    start_line := LineNo; start_col := ColNo;

    { consume comments }
    if (b = Ord('/')) and (PeekByte(1) = Ord('*')) then begin
      ConsumeComment();
      continue;
    end;

    { whitespace }
    if IsWhitespace(b) then begin
      while IsWhitespace(PeekByte()) do AdvanceBytes(1);
      ZeroToken(t);
      t.token_type := Ord(LB_Whitespace);
      t.start_line := start_line; t.start_col := start_col;
      t.end_line := LineNo; t.end_col := ColNo;
      EmitBytes(emit_cb, user, t, nil, 0, nil, 0);
      continue;
    end;

    { CDO <!-- and CDC --> }
    if (b = Ord('<')) and (PeekByte(1) = Ord('!')) and (PeekByte(2) = Ord('-')) and (PeekByte(3) = Ord('-')) then begin
      AdvanceBytes(4);
      ZeroToken(t);
      t.token_type := Ord(LB_CDO);
      t.start_line := start_line; t.start_col := start_col;
      t.end_line := LineNo; t.end_col := ColNo;
      EmitBytes(emit_cb, user, t, nil, 0, nil, 0);
      continue;
    end;
    if (b = Ord('-')) and (PeekByte(1) = Ord('-')) and (PeekByte(2) = Ord('>')) then begin
      AdvanceBytes(3);
      ZeroToken(t);
      t.token_type := Ord(LB_CDC);
      t.start_line := start_line; t.start_col := start_col;
      t.end_line := LineNo; t.end_col := ColNo;
      EmitBytes(emit_cb, user, t, nil, 0, nil, 0);
      continue;
    end;

    { punctuation tokens }
    case b of
      Ord(':'): begin AdvanceBytes(1); ZeroToken(t); t.token_type := Ord(LB_Colon); t.start_line:=start_line; t.start_col:=start_col; t.end_line:=LineNo; t.end_col:=ColNo; EmitBytes(emit_cb,user,t,nil,0,nil,0); continue; end;
      Ord(';'): begin AdvanceBytes(1); ZeroToken(t); t.token_type := Ord(LB_Semicolon); t.start_line:=start_line; t.start_col:=start_col; t.end_line:=LineNo; t.end_col:=ColNo; EmitBytes(emit_cb,user,t,nil,0,nil,0); continue; end;
      Ord(','): begin AdvanceBytes(1); ZeroToken(t); t.token_type := Ord(LB_Comma); t.start_line:=start_line; t.start_col:=start_col; t.end_line:=LineNo; t.end_col:=ColNo; EmitBytes(emit_cb,user,t,nil,0,nil,0); continue; end;
      Ord('['): begin AdvanceBytes(1); ZeroToken(t); t.token_type := Ord(LB_OpenSquare); t.start_line:=start_line; t.start_col:=start_col; t.end_line:=LineNo; t.end_col:=ColNo; EmitBytes(emit_cb,user,t,nil,0,nil,0); continue; end;
      Ord(']'): begin AdvanceBytes(1); ZeroToken(t); t.token_type := Ord(LB_CloseSquare); t.start_line:=start_line; t.start_col:=start_col; t.end_line:=LineNo; t.end_col:=ColNo; EmitBytes(emit_cb,user,t,nil,0,nil,0); continue; end;
      Ord('('): begin AdvanceBytes(1); ZeroToken(t); t.token_type := Ord(LB_OpenParen); t.start_line:=start_line; t.start_col:=start_col; t.end_line:=LineNo; t.end_col:=ColNo; EmitBytes(emit_cb,user,t,nil,0,nil,0); continue; end;
      Ord(')'): begin AdvanceBytes(1); ZeroToken(t); t.token_type := Ord(LB_CloseParen); t.start_line:=start_line; t.start_col:=start_col; t.end_line:=LineNo; t.end_col:=ColNo; EmitBytes(emit_cb,user,t,nil,0,nil,0); continue; end;
      Ord('{'): begin AdvanceBytes(1); ZeroToken(t); t.token_type := Ord(LB_OpenCurly); t.start_line:=start_line; t.start_col:=start_col; t.end_line:=LineNo; t.end_col:=ColNo; EmitBytes(emit_cb,user,t,nil,0,nil,0); continue; end;
      Ord('}'): begin AdvanceBytes(1); ZeroToken(t); t.token_type := Ord(LB_CloseCurly); t.start_line:=start_line; t.start_col:=start_col; t.end_line:=LineNo; t.end_col:=ColNo; EmitBytes(emit_cb,user,t,nil,0,nil,0); continue; end;
    end;

    { strings }
    if (b = 34) or (b = 39) then begin
      b2 := b; AdvanceBytes(1);
      str_start := Index;
      str_len := 0;
      while not AtEOF do begin
        b3 := PeekByte();
        if b3 = b2 then begin str_len := Index - str_start; AdvanceBytes(1); break; end;
        if (b3 = 10) then begin { newline -> BadString }
          ZeroToken(t);
          t.token_type := Ord(LB_BadString);
          t.start_line := start_line; t.start_col := start_col;
          t.end_line := LineNo; t.end_col := ColNo;
          EmitBytes(emit_cb, user, t, nil, 0, nil, 0);
          break;
        end;
        if b3 = 92 then begin
          if not AtEOF then begin AdvanceBytes(1); b3 := PeekByte(); end;
        end;
        AdvanceBytes(1);
      end;
      if str_len > 0 then begin
        ZeroToken(t);
        t.token_type := Ord(LB_String);
        t.start_line := start_line; t.start_col := start_col;
        t.end_line := LineNo; t.end_col := ColNo;
        EmitBytes(emit_cb, user, t, @PInput[str_start], str_len, nil, 0);
      end;
      continue;
    end;

    { at-keyword }
    if (b = Ord('@')) and WouldStartAnIdentSequence(PeekByte(1), PeekByte(2), PeekByte(3)) then begin
      AdvanceBytes(1);
      // read ident sequence
      name_len := 0;
      ConsumeIdentSequence(name_ptr, name_len);
      ZeroToken(t);
      t.token_type := Ord(LB_AtKeyword);
      t.start_line := start_line; t.start_col := start_col;
      t.end_line := LineNo; t.end_col := ColNo;
      EmitBytes(emit_cb, user, t, name_ptr, name_len, nil, 0);
      continue;
    end;

    { hash }
    if (b = Ord('#')) and (IsNameChar(PeekByte(1)) or IsValidEscapeSequence(PeekByte(1), PeekByte(2))) then begin
      AdvanceBytes(1);
      hash_len := 0;
      ConsumeIdentSequence(hash_ptr, hash_len);
      ZeroToken(t);
      t.token_type := Ord(LB_Hash);
      if (hash_len > 0) and (((hash_ptr[0] >= 'A') and (hash_ptr[0] <= 'Z')) or ((hash_ptr[0] >= 'a') and (hash_ptr[0] <= 'z')) or (hash_ptr[0] = '_')) then
        t.hash_type := Ord(LB_Hash_Id)
      else
        t.hash_type := Ord(LB_Hash_Unrestricted);
      t.start_line := start_line; t.start_col := start_col;
      t.end_line := LineNo; t.end_col := ColNo;
      EmitBytes(emit_cb, user, t, hash_ptr, hash_len, nil, 0);
      continue;
    end;

    { number vs ident-like vs delim }
    if WouldStartANumber(b, b2, b3) then begin
      if ConsumeNumber(num, is_int, had_sign) then begin
        if (PeekByte() = Ord('%')) then begin
          AdvanceBytes(1);
          ZeroToken(t);
          t.token_type := Ord(LB_Percentage);
          if is_int and had_sign then t.number_type := Ord(LB_Number_IntegerWithExplicitSign)
          else if is_int then t.number_type := Ord(LB_Number_Integer)
          else t.number_type := Ord(LB_Number_Number);
          t.number_value := num;
          t.start_line := start_line; t.start_col := start_col;
          t.end_line := LineNo; t.end_col := ColNo;
          EmitBytes(emit_cb, user, t, nil, 0, nil, 0);
          continue;
        end else if IsNameStart(PeekByte()) then begin
          ConsumeWhileName(name_start, name_len);
          ZeroToken(t);
          t.token_type := Ord(LB_Dimension);
          if is_int and had_sign then t.number_type := Ord(LB_Number_IntegerWithExplicitSign)
          else if is_int then t.number_type := Ord(LB_Number_Integer)
          else t.number_type := Ord(LB_Number_Number);
          t.number_value := num;
          t.start_line := start_line; t.start_col := start_col;
          t.end_line := LineNo; t.end_col := ColNo;
          EmitBytes(emit_cb, user, t, @PInput[name_start], name_len, nil, 0);
          continue;
        end else begin
          ZeroToken(t);
          t.token_type := Ord(LB_Number);
          if is_int and had_sign then t.number_type := Ord(LB_Number_IntegerWithExplicitSign)
          else if is_int then t.number_type := Ord(LB_Number_Integer)
          else t.number_type := Ord(LB_Number_Number);
          t.number_value := num;
          t.start_line := start_line; t.start_col := start_col;
          t.end_line := LineNo; t.end_col := ColNo;
          EmitBytes(emit_cb, user, t, nil, 0, nil, 0);
          continue;
        end;
      end;
    end else if WouldStartAnIdentSequence(b, b2, b3) then begin
      if (b = Ord('-')) and not IsNameChar(PeekByte(1)) then begin
        { just a delim '-' }
      end else begin
        ident_len := 0;
        ConsumeIdentSequence(ident_ptr, ident_len);
        if (ident_len = 3) and (((ident_ptr[0] = 'u') or (ident_ptr[0] = 'U')) and ((ident_ptr[1] = 'r') or (ident_ptr[1] = 'R')) and ((ident_ptr[2] = 'l') or (ident_ptr[2] = 'L'))) and (PeekByte() = 40) then begin
          AdvanceBytes(1);
          { skip whitespace }
          while IsWhitespace(PeekByte()) do AdvanceBytes(1);
          if (PeekByte() = 34) or (PeekByte() = 39) then begin
            { per spec, treat as function token when quotes present }
            ZeroToken(t); t.token_type := Ord(LB_Function);
            t.start_line := start_line; t.start_col := start_col;
            t.end_line := LineNo; t.end_col := ColNo;
            EmitBytes(emit_cb, user, t, ident_ptr, ident_len, nil, 0);
          end else begin
            str_start := Index; str_len := 0;
            while not AtEOF do begin
              b3 := PeekByte();
              if b3 = 41 then begin str_len := Index - str_start; AdvanceBytes(1); break; end;
              if IsWhitespace(b3) then begin AdvanceBytes(1); continue; end;
              if (b3 = 34) or (b3 = 39) or (b3 = 40) then begin
                ZeroToken(t); t.token_type := Ord(LB_BadUrl);
                t.start_line := start_line; t.start_col := start_col;
                t.end_line := LineNo; t.end_col := ColNo;
                EmitBytes(emit_cb, user, t, nil, 0, nil, 0); str_len := 0; break;
              end;
              AdvanceBytes(1);
            end;
            if str_len > 0 then begin
              ZeroToken(t); t.token_type := Ord(LB_Url);
              t.start_line := start_line; t.start_col := start_col;
              t.end_line := LineNo; t.end_col := ColNo;
              EmitBytes(emit_cb, user, t, @PInput[str_start], str_len, nil, 0);
            end;
          end;
          { no allocation; nothing to free }
          continue;
        end;
        if (PeekByte() = 40) then begin
          AdvanceBytes(1);
          ZeroToken(t); t.token_type := Ord(LB_Function);
          t.start_line := start_line; t.start_col := start_col;
          t.end_line := LineNo; t.end_col := ColNo;
          EmitBytes(emit_cb, user, t, ident_ptr, ident_len, nil, 0);
          { no allocation; nothing to free }
          continue;
        end else begin
          ZeroToken(t); t.token_type := Ord(LB_Ident);
          t.start_line := start_line; t.start_col := start_col;
          t.end_line := LineNo; t.end_col := ColNo;
          EmitBytes(emit_cb, user, t, ident_ptr, ident_len, nil, 0);
          { no allocation; nothing to free }
          continue;
        end;
      end;
    end;

    { default: delim }
    ZeroToken(t);
    t.token_type := Ord(LB_Delim);
    t.delim := Cardinal(b);
    t.start_line := start_line; t.start_col := start_col;
    AdvanceBytes(1);
    t.end_line := LineNo; t.end_col := ColNo;
    EmitBytes(emit_cb, user, t, nil, 0, nil, 0);
  end;
end;

function lb_css_tokenize_stream(const input_utf8: PChar; input_len: SizeUInt; emit_cb: PLB_EmitCb; userdata: Pointer): Integer; cdecl;
begin
  PInput := input_utf8;
  InputLen := input_len;
  Index := 0;
  LineNo := 0;
  ColNo := 0;
  TokenizeInternal(emit_cb, userdata);
  lb_css_tokenize_stream := 0;
end;

end.
