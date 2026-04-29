from math import inf

from Utils.CSSGrammar.Parser.component_values import Keyword
from Utils.CSSGrammar.Parser.component_values import NumericTypeRangeRestriction
from Utils.CSSGrammar.Parser.component_values import Type
from Utils.CSSGrammar.Parser.component_values import is_dimension_percentage_mix_type
from Utils.CSSGrammar.Parser.component_values import is_dimension_type
from Utils.CSSGrammar.Parser.component_values import is_numeric_type
from Utils.CSSGrammar.Parser.token import Token
from Utils.CSSGrammar.Parser.token import TokenType
from Utils.lexer import Lexer


def is_identifier_character(ch: str) -> bool:
    return ch.isascii() and (ch.isalnum() or ch == "-")


class Tokenizer:
    def __init__(self, input: str) -> None:
        self.lexer = Lexer(input)

    @classmethod
    def tokenize(cls, input: str) -> list[Token]:
        return cls(input).tokenize_impl()

    def tokenize_impl(self) -> list[Token]:
        tokens: list[Token] = []

        while True:
            self.discard_whitespace()
            if self.lexer.is_eof():
                tokens.append(Token.create(TokenType.END_OF_FILE))
                return tokens

            tokens.append(self.consume_a_token())

    def discard_whitespace(self) -> None:
        self.lexer.ignore_while(lambda ch: ch.isspace() and ch.isascii())

    def consume_an_identifier(self) -> str:
        return self.lexer.consume_while(is_identifier_character)

    def consume_a_token(self) -> Token:
        peeked = self.lexer.peek()
        if peeked == "|":
            self.lexer.consume()
            return Token.create(TokenType.SINGLE_BAR)
        if peeked == "<":
            return self.consume_a_non_terminal_token()

        if is_identifier_character(peeked):
            return self.consume_a_keyword_token()

        raise SyntaxError("CSSGrammar::Tokenizer: Unexpected character")

    def consume_custom_ident_blacklist(self) -> list[str]:
        # NB: This notation isn't yet included in the spec but we use it internally and the CSSWG has resolved to add it in
        #     https://github.com/w3c/csswg-drafts/issues/11924

        self.discard_whitespace()

        if not self.lexer.consume_specific("!["):
            return []

        blacklist: list[str] = []

        while True:
            self.discard_whitespace()

            ident = self.consume_an_identifier()
            if not ident:
                raise SyntaxError("Expected identifier in custom-ident blacklist")

            blacklist.append(ident)

            self.discard_whitespace()
            if self.lexer.consume_specific("]"):
                return blacklist

            if not self.lexer.consume_specific(","):
                raise SyntaxError("Expected ',' in custom-ident blacklist")

    # https://drafts.csswg.org/css-values-4/#css-bracketed-range-notation
    def consume_bracketed_range_notation(self, type_name: str) -> NumericTypeRangeRestriction:
        self.discard_whitespace()

        # If no range is indicated, either by using the bracketed range notation or in the property description, then
        # [-∞,∞] is assumed.
        if not self.lexer.consume_specific("["):
            return NumericTypeRangeRestriction(-inf, inf)

        self.discard_whitespace()
        minimum = self.consume_bracketed_range_bound(type_name)

        self.discard_whitespace()
        if not self.lexer.consume_specific(","):
            raise SyntaxError("Expected ',' in bracketed range notation")

        self.discard_whitespace()
        maximum = self.consume_bracketed_range_bound(type_name)

        self.discard_whitespace()
        if not self.lexer.consume_specific("]"):
            raise SyntaxError("Expected ']' to close bracketed range notation")

        return NumericTypeRangeRestriction(minimum, maximum)

    def consume_bracketed_range_bound(self, type_name: str) -> float:
        # Values of -∞ or ∞ must be written without units, even if the value type uses units.
        if self.lexer.consume_specific("-∞"):
            return -inf

        if self.lexer.consume_specific("∞"):
            return inf

        # FIXME: Do we need to allow non-integer values?
        bound_value = self.consume_decimal_integer()

        if bound_value != 0 and (is_dimension_percentage_mix_type(type_name) or type_name == "length"):
            raise SyntaxError("Types with units not resolvable at parse time only support zero and infinite bounds")

        # FIXME: Validate and store the unit, for now we drop it and assume it was the relevant canonical unit.
        unit = self.lexer.consume_while(is_identifier_character)

        if unit and not is_dimension_type(type_name) and not is_dimension_percentage_mix_type(type_name):
            raise SyntaxError("Unexpected unit for unitless bound value")

        if not unit and bound_value != 0 and is_dimension_type(type_name):
            raise SyntaxError("Expected unit for non-zero, non-infinite bound value")

        return float(bound_value)

    def consume_decimal_integer(self) -> int:
        sign = 1
        if self.lexer.consume_specific("-"):
            sign = -1

        digits = self.lexer.consume_while(lambda ch: ch.isdigit())
        if not digits:
            raise SyntaxError("Expected decimal integer")

        return sign * int(digits)

    def consume_a_non_terminal_token(self) -> Token:
        assert self.lexer.consume_specific("<")

        name = self.consume_an_identifier()

        if not name:
            raise SyntaxError("CSSGrammar::Tokenizer: Expected a type name")

        custom_ident_blacklist = []
        if name == "custom-ident":
            custom_ident_blacklist = self.consume_custom_ident_blacklist()

        numeric_type_accepted_range = None
        if is_numeric_type(name) or is_dimension_percentage_mix_type(name):
            numeric_type_accepted_range = self.consume_bracketed_range_notation(name)

        if not self.lexer.consume_specific(">"):
            raise SyntaxError("CSSGrammar::Tokenizer: Expected '>'")

        return Token.create_component_value(Type(name, custom_ident_blacklist, numeric_type_accepted_range))

    def consume_a_keyword_token(self) -> Token:
        value = self.consume_an_identifier()

        assert value

        return Token.create_component_value(Keyword(value))
