from Utils.CSSGrammar.Parser.component_values import Keyword
from Utils.CSSGrammar.Parser.component_values import Type
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
        tokens = []

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
        match self.lexer.peek():
            case "|":
                self.lexer.consume()
                return Token.create(TokenType.SINGLE_BAR)
            case "<":
                return self.consume_a_non_terminal_token()

        if is_identifier_character(self.lexer.peek()):
            return self.consume_a_keyword_token()

        raise SyntaxError("CSSGrammar::Tokenizer: Unexpected character")

    def consume_custom_ident_blacklist(self) -> list[str]:
        # NB: This notation isn't yet included in the spec but we use it internally and the CSSWG has resolved to add it in
        #     https://github.com/w3c/csswg-drafts/issues/11924

        self.discard_whitespace()

        if not self.lexer.consume_specific_string("!["):
            return []

        blacklist = []

        while True:
            self.discard_whitespace()

            ident = self.consume_an_identifier()
            if not ident:
                raise SyntaxError("Expected identifier in custom-ident blacklist")

            blacklist.append(ident)

            self.discard_whitespace()
            if self.lexer.consume_specific_char("]"):
                return blacklist

            if not self.lexer.consume_specific_char(","):
                raise SyntaxError("Expected ',' in custom-ident blacklist")

    def consume_a_non_terminal_token(self) -> Token:
        assert self.lexer.consume_specific_char("<")

        name = self.consume_an_identifier()

        if not name:
            raise SyntaxError("CSSGrammar::Tokenizer: Expected a type name")

        custom_ident_blacklist = None
        if name == "custom-ident":
            custom_ident_blacklist = self.consume_custom_ident_blacklist()

        # FIXME: Support numeric data type bracketed range notations (i.e. <integer [0,10]>)
        if not self.lexer.consume_specific_char(">"):
            raise SyntaxError("CSSGrammar::Tokenizer: Expected '>'")

        return Token.create_component_value(Type(name, custom_ident_blacklist))

    def consume_a_keyword_token(self) -> Token:
        value = self.consume_an_identifier()

        assert value

        return Token.create_component_value(Keyword(value))
