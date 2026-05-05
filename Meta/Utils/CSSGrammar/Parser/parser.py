from Utils.CSSGrammar.Parser.grammar_node import CombinatorGrammarNode
from Utils.CSSGrammar.Parser.grammar_node import CombinatorType
from Utils.CSSGrammar.Parser.grammar_node import ComponentValueGrammarNode
from Utils.CSSGrammar.Parser.grammar_node import GrammarNode
from Utils.CSSGrammar.Parser.grammar_node import GroupGrammarNode
from Utils.CSSGrammar.Parser.grammar_node import OptionalGrammarNode
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

        peeked = self.peek()
        component_value = None

        if peeked.is_token_type(TokenType.OPEN_SQUARE_BRACKET):
            self.consume()
            group = self.parse_alternatives()

            if not self.peek().is_token_type(TokenType.CLOSE_SQUARE_BRACKET):
                raise SyntaxError("CSSGrammar::Parser: Expected ']'")

            # FIXME: Support required groups (e.g. [ <foo>? ]!)
            self.consume()
            component_value = GroupGrammarNode(group)

        if peeked.is_token_type(TokenType.COMPONENT_VALUE):
            component_value = ComponentValueGrammarNode(self.consume().component_value())

        if component_value is None:
            raise SyntaxError("CSSGrammar::Parser: Expected a component value")

        if self.peek().is_token_type(TokenType.QUESTION_MARK):
            self.consume()
            component_value = OptionalGrammarNode(component_value)

        return component_value

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
