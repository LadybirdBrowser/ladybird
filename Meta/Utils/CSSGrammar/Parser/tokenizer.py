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

        raise SyntaxError("CSSGrammar::Tokenizer: Unexpected character")

    def consume_a_non_terminal_token(self) -> Token:
        assert self.lexer.consume_specific_char("<")

        name = self.consume_an_identifier()

        if not name:
            raise SyntaxError("CSSGrammar::Tokenizer: Expected a type name")

        # FIXME: Support custom-ident blacklist notation (i.e. <custom-ident ![foo, bar]>)
        # FIXME: Support numeric data type bracketed range notations (i.e. <integer [0,10]>)
        if not self.lexer.consume_specific_char(">"):
            raise SyntaxError("CSSGrammar::Tokenizer: Expected '>'")

        return Token.create_component_value(Type(name))
