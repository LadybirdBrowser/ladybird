from Utils.CSSGrammar.Parser.grammar_node import CombinatorGrammarNode
from Utils.CSSGrammar.Parser.grammar_node import CombinatorType
from Utils.CSSGrammar.Parser.grammar_node import ComponentValueGrammarNode
from Utils.CSSGrammar.Parser.grammar_node import GrammarNode
from Utils.CSSGrammar.Parser.token import Token
from Utils.CSSGrammar.Parser.token import TokenType
from Utils.CSSGrammar.Parser.tokenizer import Tokenizer


class Parser:
    def __init__(self, tokens: list[Token]) -> None:
        self.tokens = tokens
        self.index = 0

    @classmethod
    def parse_value_definition_grammar(cls, input: str) -> GrammarNode:
        parser = cls(Tokenizer.tokenize(input))

        value = parser.parse_alternatives()

        if not parser.peek().is_token_type(TokenType.END_OF_FILE):
            raise SyntaxError("CSSGrammar::Parser: Unexpected trailing input")

        return value

    def parse_alternatives(self) -> GrammarNode:
        children = [self.parse_component_value()]

        while self.peek().is_token_type(TokenType.SINGLE_BAR):
            self.consume()
            children.append(self.parse_component_value())

        if len(children) == 1:
            return children[0]

        return CombinatorGrammarNode(CombinatorType.ALTERNATIVES, children)

    def parse_component_value(self) -> GrammarNode:
        # https://drafts.csswg.org/css-values-4/#component-multipliers
        # FIXME: Support component multipliers

        if not self.peek().is_token_type(TokenType.COMPONENT_VALUE):
            raise SyntaxError("CSSGrammar::Parser: Expected a component value")

        return ComponentValueGrammarNode(self.consume().component_value())

    def peek(self, offset: int = 0) -> Token:
        index = min(self.index + offset, len(self.tokens) - 1)
        return self.tokens[index]

    def consume(self) -> Token:
        token = self.peek()

        if not token.is_token_type(TokenType.END_OF_FILE):
            self.index += 1

        return token


def parse_value_definition_grammar(input: str) -> GrammarNode:
    return Parser.parse_value_definition_grammar(input)
