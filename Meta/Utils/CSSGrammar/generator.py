from typing import TextIO

from Utils.CSSGrammar.Parser.component_values import Type
from Utils.CSSGrammar.Parser.grammar_node import CombinatorGrammarNode
from Utils.CSSGrammar.Parser.grammar_node import CombinatorType
from Utils.CSSGrammar.Parser.grammar_node import ComponentValueGrammarNode
from Utils.CSSGrammar.Parser.grammar_node import GrammarNode
from Utils.CSSGrammar.Parser.parser import parse_value_definition_grammar
from Utils.utils import snake_casify


def generate_css_parser_expression_for_type_component_value(out: TextIO, cpp_name: str, type: Type) -> None:
    type_name = snake_casify(type.name)

    additional_arguments = ""
    if type.custom_ident_blacklist is not None:
        additional_arguments = ", ReadonlySpan<StringView> { "

        if len(type.custom_ident_blacklist) > 0:
            disallowed_idents = "".join(f'"{disallowed_ident}"sv, ' for disallowed_ident in type.custom_ident_blacklist)
            additional_arguments += f"Array<StringView, {len(type.custom_ident_blacklist)}> {{{disallowed_idents}}}"

        additional_arguments += "}"

    out.write(f"auto {cpp_name} = parse_{type_name}_value(tokens{additional_arguments});\n")


def generate_css_parser_expression_for_component_value_grammar_node(
    out: TextIO, cpp_name: str, grammar_node: ComponentValueGrammarNode
) -> None:
    match grammar_node.component_value:
        case Type() as type_component_value:
            generate_css_parser_expression_for_type_component_value(out, cpp_name, type_component_value)
            return

    raise TypeError(f"Unhandled component value type: {type(grammar_node.component_value).__name__}")


def generate_css_parser_expression_for_alternatives(
    out: TextIO, cpp_name: str, alternatives: list[GrammarNode]
) -> None:
    out.write(f"auto const parse_{cpp_name}_alternatives = [&]() -> RefPtr<StyleValue const> {{\n")

    for i, alternative in enumerate(alternatives):
        alternative_name = f"{cpp_name}_alternative_{i}"
        generate_css_parser_expression_for_grammar_node(out, alternative_name, alternative)
        out.write(f"""if ({alternative_name})
    return {alternative_name};

""")

    out.write(f"""return nullptr;
}};

auto {cpp_name} = parse_{cpp_name}_alternatives();
""")


def generate_css_parser_expression_for_combinator_grammar_node(
    out: TextIO, cpp_name: str, grammar_node: CombinatorGrammarNode
) -> None:
    match grammar_node.combinator_type:
        case CombinatorType.ALTERNATIVES:
            generate_css_parser_expression_for_alternatives(out, cpp_name, grammar_node.children)
            return

    raise TypeError(f"Unhandled combinator type: {grammar_node.combinator_type}")


def generate_css_parser_expression_for_grammar_node(out: TextIO, cpp_name: str, grammar_node: GrammarNode) -> None:
    match grammar_node:
        case ComponentValueGrammarNode():
            generate_css_parser_expression_for_component_value_grammar_node(out, cpp_name, grammar_node)
            return
        case CombinatorGrammarNode():
            generate_css_parser_expression_for_combinator_grammar_node(out, cpp_name, grammar_node)
            return

    raise TypeError(f"Unhandled grammar node type: {type(grammar_node).__name__}")


def generate_css_parser_expression_for_grammar(out: TextIO, cpp_name: str, grammar: str) -> None:
    generate_css_parser_expression_for_grammar_node(out, cpp_name, parse_value_definition_grammar(grammar))
