from math import inf
from typing import TextIO

from Utils.CSSGrammar.Parser.component_values import Keyword
from Utils.CSSGrammar.Parser.component_values import Type
from Utils.CSSGrammar.Parser.component_values import is_dimension_percentage_mix_type
from Utils.CSSGrammar.Parser.grammar_node import CombinatorGrammarNode
from Utils.CSSGrammar.Parser.grammar_node import CombinatorType
from Utils.CSSGrammar.Parser.grammar_node import ComponentValueGrammarNode
from Utils.CSSGrammar.Parser.grammar_node import GrammarNode
from Utils.CSSGrammar.Parser.parser import parse_value_definition_grammar
from Utils.utils import snake_casify
from Utils.utils import title_casify


def bound_value_to_code(value: float, type_name: str) -> str:
    if value == -inf:
        return "AK::NumericLimits<i32>::min()" if type_name == "integer" else "AK::NumericLimits<float>::lowest()"
    if value == inf:
        return "AK::NumericLimits<i32>::max()" if type_name == "integer" else "AK::NumericLimits<float>::max()"
    return str(value)


def generate_css_parser_expression_for_type_component_value(out: TextIO, cpp_name: str, type: Type) -> None:
    type_name = snake_casify(type.name)

    additional_arguments = ""
    if type.custom_ident_blacklist:
        additional_arguments = ", ReadonlySpan<StringView> { "

        if len(type.custom_ident_blacklist) > 0:
            disallowed_idents = "".join(f'"{disallowed_ident}"sv, ' for disallowed_ident in type.custom_ident_blacklist)
            additional_arguments += f"Array<StringView, {len(type.custom_ident_blacklist)}> {{{disallowed_idents}}}"

        additional_arguments += "}"

    if type.numeric_type_accepted_range is not None:
        minimum = bound_value_to_code(type.numeric_type_accepted_range.minimum, type_name)
        maximum = bound_value_to_code(type.numeric_type_accepted_range.maximum, type_name)
        accepted_range = f", {{ {minimum}, {maximum} }}"

        additional_arguments += accepted_range

        # NB: Pass the accepted range twice for dimension-percentage mixes, once for the dimension and once for the percentage.
        if is_dimension_percentage_mix_type(type.name):
            additional_arguments += accepted_range

    out.write(f"auto {cpp_name} = parse_{type_name}_value(tokens{additional_arguments});\n")


def generate_css_parser_expression_for_keyword_component_value(out: TextIO, cpp_name: str, keyword: Keyword) -> None:
    keyword_name = title_casify(keyword.value)
    out.write(f"auto {cpp_name} = parse_specific_keyword_value(tokens, Keyword::{keyword_name});\n")


def generate_css_parser_expression_for_component_value_grammar_node(
    out: TextIO, cpp_name: str, grammar_node: ComponentValueGrammarNode
) -> None:
    component_value = grammar_node.component_value
    if isinstance(component_value, Type):
        generate_css_parser_expression_for_type_component_value(out, cpp_name, component_value)
        return
    assert isinstance(component_value, Keyword)
    generate_css_parser_expression_for_keyword_component_value(out, cpp_name, component_value)


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
    if grammar_node.combinator_type == CombinatorType.ALTERNATIVES:
        generate_css_parser_expression_for_alternatives(out, cpp_name, grammar_node.children)
        return

    raise TypeError(f"Unhandled combinator type: {grammar_node.combinator_type}")


def generate_css_parser_expression_for_grammar_node(out: TextIO, cpp_name: str, grammar_node: GrammarNode) -> None:
    if isinstance(grammar_node, ComponentValueGrammarNode):
        generate_css_parser_expression_for_component_value_grammar_node(out, cpp_name, grammar_node)
        return
    if isinstance(grammar_node, CombinatorGrammarNode):
        generate_css_parser_expression_for_combinator_grammar_node(out, cpp_name, grammar_node)
        return

    raise TypeError(f"Unhandled grammar node type: {type(grammar_node).__name__}")


def generate_css_parser_expression_for_grammar(out: TextIO, cpp_name: str, grammar: str) -> None:
    generate_css_parser_expression_for_grammar_node(out, cpp_name, parse_value_definition_grammar(grammar))
