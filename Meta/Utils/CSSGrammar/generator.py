from math import inf
from typing import TextIO

from Utils.CSSGrammar.Parser.component_values import Keyword
from Utils.CSSGrammar.Parser.component_values import Type
from Utils.CSSGrammar.Parser.component_values import is_dimension_percentage_mix_type
from Utils.CSSGrammar.Parser.grammar_node import CombinatorGrammarNode
from Utils.CSSGrammar.Parser.grammar_node import CombinatorType
from Utils.CSSGrammar.Parser.grammar_node import ComponentValueGrammarNode
from Utils.CSSGrammar.Parser.grammar_node import GrammarNode
from Utils.CSSGrammar.Parser.grammar_node import GroupGrammarNode
from Utils.CSSGrammar.Parser.grammar_node import MultiplierGrammarNode
from Utils.CSSGrammar.Parser.grammar_node import OptionalGrammarNode
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
    out.write(f"auto {cpp_name} = parse_specific_keyword_value(tokens, {{ {{ Keyword::{keyword_name} }} }});\n")


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

    # NB: As an optimization we combine all keyword alternatives into a single parse_specific_keyword_value call to
    #     avoid unnecessary backtracking in the common case of multiple keyword alternatives.
    keyword_alternatives: list[str] = []

    for i, alternative in enumerate(alternatives):
        if isinstance(alternative, ComponentValueGrammarNode) and isinstance(alternative.component_value, Keyword):
            keyword_alternatives.append(alternative.component_value.value)
            continue

        alternative_name = f"{cpp_name}_alternative_{i}"
        generate_css_parser_expression_for_grammar_node(out, alternative_name, alternative)
        out.write(f"""if ({alternative_name})
    return {alternative_name};
""")

    if keyword_alternatives:
        keyword_alternatives_names = ", ".join(f"Keyword::{title_casify(keyword)}" for keyword in keyword_alternatives)
        out.write(f"""auto {cpp_name}_keyword_alternative = parse_specific_keyword_value(tokens, {{ {{ {keyword_alternatives_names} }} }});
if ({cpp_name}_keyword_alternative)
    return {cpp_name}_keyword_alternative;
""")

    out.write(f"""return nullptr;
}};

auto {cpp_name} = parse_{cpp_name}_alternatives();
""")


def generate_css_parser_expression_for_juxtaposition(out: TextIO, cpp_name: str, children: list[GrammarNode]) -> None:
    out.write(f"""auto const parse_{cpp_name}_juxtaposition = [&]() -> RefPtr<StyleValue const> {{
auto {cpp_name}_transaction = tokens.begin_transaction();
StyleValueVector {cpp_name}_values;
{cpp_name}_values.ensure_capacity({len(children)});

""")

    for i, component in enumerate(children):
        component_name = f"{cpp_name}_component_{i}"
        generate_css_parser_expression_for_grammar_node(out, component_name, component)
        out.write(f"""if (!{component_name})
    return nullptr;

{cpp_name}_values.append({component_name}.release_nonnull());
""")

    out.write(f"""{cpp_name}_transaction.commit();
return StyleValueList::create(move({cpp_name}_values), StyleValueList::Separator::Space, StyleValueList::Collapsible::No);
}};

auto {cpp_name} = parse_{cpp_name}_juxtaposition();
""")


def generate_css_parser_expression_for_combinator_grammar_node(
    out: TextIO, cpp_name: str, grammar_node: CombinatorGrammarNode
) -> None:
    if grammar_node.combinator_type == CombinatorType.JUXTAPOSITION:
        generate_css_parser_expression_for_juxtaposition(out, cpp_name, grammar_node.children)
        return

    if grammar_node.combinator_type == CombinatorType.ALTERNATIVES:
        generate_css_parser_expression_for_alternatives(out, cpp_name, grammar_node.children)
        return

    raise TypeError(f"Unhandled combinator type: {grammar_node.combinator_type}")


def generate_css_parser_expression_for_group_grammar_node(
    out: TextIO, cpp_name: str, grammar_node: GroupGrammarNode
) -> None:
    generate_css_parser_expression_for_grammar_node(out, cpp_name, grammar_node.child)


def generate_css_parser_expression_for_optional_grammar_node(
    out: TextIO, cpp_name: str, grammar_node: OptionalGrammarNode
) -> None:
    out.write(f"""RefPtr<StyleValue const> {cpp_name} = EmptyOptionalStyleValue::create();
""")

    generate_css_parser_expression_for_grammar_node(out, f"maybe_{cpp_name}", grammar_node.child)

    out.write(f"""if (maybe_{cpp_name})
        {cpp_name} = maybe_{cpp_name};
""")


def generate_css_parser_expression_for_multiplier_grammar_node(
    out: TextIO, cpp_name: str, grammar_node: MultiplierGrammarNode
) -> None:
    # Generate a helper lambda for parsing one repetition of the child component.
    out.write(f"auto const parse_{cpp_name}_repetition = [&]() -> RefPtr<StyleValue const> {{\n")
    generate_css_parser_expression_for_grammar_node(out, f"{cpp_name}_rep", grammar_node.child)
    out.write(f"    return {cpp_name}_rep;\n")
    out.write("};\n\n")

    capacity = grammar_node.maximum if grammar_node.maximum is not None else grammar_node.minimum
    out.write(f"""auto const parse_{cpp_name}_multiplier = [&]() -> RefPtr<StyleValue const> {{
auto {cpp_name}_transaction = tokens.begin_transaction();
StyleValueVector {cpp_name}_values;
{cpp_name}_values.ensure_capacity({capacity});

""")
    # Minimum repetitions (required)
    for i in range(grammar_node.minimum):
        out.write(f"""auto {cpp_name}_min_{i} = parse_{cpp_name}_repetition();
if (!{cpp_name}_min_{i})
    return nullptr;
{cpp_name}_values.append({cpp_name}_min_{i}.release_nonnull());

""")
    # Optional additional repetitions
    if grammar_node.maximum is not None:
        remaining = grammar_node.maximum - grammar_node.minimum
        if remaining > 0:
            for i in range(remaining):
                out.write(f"""do {{
auto {cpp_name}_extra_{i} = parse_{cpp_name}_repetition();
if (!{cpp_name}_extra_{i})
    break;
{cpp_name}_values.append({cpp_name}_extra_{i}.release_nonnull());
}} while (false);

""")
            out.write("\n")
    else:
        # {A,} — unbounded: keep trying until failure
        out.write(f"""while (true) {{
auto {cpp_name}_extra = parse_{cpp_name}_repetition();
if (!{cpp_name}_extra)
    break;
{cpp_name}_values.append({cpp_name}_extra.release_nonnull());
}}

""")

    out.write(f"""{cpp_name}_transaction.commit();
return StyleValueList::create(move({cpp_name}_values), StyleValueList::Separator::Space, StyleValueList::Collapsible::No);
}};
auto {cpp_name} = parse_{cpp_name}_multiplier();
""")


def generate_css_parser_expression_for_grammar_node(out: TextIO, cpp_name: str, grammar_node: GrammarNode) -> None:
    if isinstance(grammar_node, ComponentValueGrammarNode):
        generate_css_parser_expression_for_component_value_grammar_node(out, cpp_name, grammar_node)
        return
    if isinstance(grammar_node, GroupGrammarNode):
        generate_css_parser_expression_for_group_grammar_node(out, cpp_name, grammar_node)
        return
    if isinstance(grammar_node, OptionalGrammarNode):
        generate_css_parser_expression_for_optional_grammar_node(out, cpp_name, grammar_node)
        return
    if isinstance(grammar_node, MultiplierGrammarNode):
        generate_css_parser_expression_for_multiplier_grammar_node(out, cpp_name, grammar_node)
        return
    if isinstance(grammar_node, CombinatorGrammarNode):
        generate_css_parser_expression_for_combinator_grammar_node(out, cpp_name, grammar_node)
        return

    raise TypeError(f"Unhandled grammar node type: {type(grammar_node).__name__}")


def generate_css_parser_expression_for_grammar(out: TextIO, cpp_name: str, grammar: str) -> None:
    generate_css_parser_expression_for_grammar_node(out, cpp_name, parse_value_definition_grammar(grammar))
