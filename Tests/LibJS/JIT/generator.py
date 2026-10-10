#
# Copyright (c) 2026-present, the Ladybird developers.
#
# SPDX-License-Identifier: BSD-2-Clause
#

"""A generator of random JavaScript programs for differential testing of the JIT.

The programs are made to give the JIT a hard time where it speculates: their functions warm up with arguments of stable
types and shapes, get compiled (by the tier-up thresholds, and at random points by jit.compile()), and then see other
types, other shapes, prototype changes, accessors, proxies, frozen objects, holes, callee changes and exceptions. They
call each other, so that the JIT inlines them, and loop, so that it enters them on the stack.

A program prints a running hash of everything its calls returned or threw, after each phase, and nothing else that
could differ between runs. The same program must print the same in the interpreter and in every JIT mode.

Every statement is on its own line and blocks close on lines of their own, so that the reducer can drop lines.
"""

import random

from typing import List

PRELUDE = """\
"use strict";
let __seed = SEED;
Math.random = () => {
    __seed = (__seed * 1103515245 + 12345) % 2147483648;
    return __seed / 2147483648;
};
let __hash = 0;
function __describe(value, depth) {
    if (typeof value === "number") return Object.is(value, -0) ? "-0" : String(value);
    if (typeof value === "bigint") return value + "n";
    if (typeof value === "string") return JSON.stringify(value);
    if (typeof value === "symbol") return value.toString();
    if (typeof value === "function") return "function";
    if (value === null || value === undefined) return String(value);
    if (typeof value !== "object") return String(value);
    if (depth > 2) return "...";
    if (Array.isArray(value)) {
        let text = "[";
        for (let i = 0; i < value.length && i < 8; ++i) text += (i in value ? __describe(value[i], depth + 1) : "_") + ",";
        return text + "]";
    }
    let text = "{";
    let count = 0;
    for (const key in value) {
        if (++count > 8) break;
        text += key + ":" + __describe(value[key], depth + 1) + ",";
    }
    return text + "}";
}
function __record(value) {
    const text = __describe(value, 0);
    __hash = (Math.imul(__hash, 31) + text.length) | 0;
    for (let i = 0; i < text.length && i < 256; ++i) __hash = (Math.imul(__hash, 31) + text.charCodeAt(i)) | 0;
}
function __call(f) {
    try {
        __record(f());
    } catch (e) {
        __record("threw " + (e instanceof Error ? e.constructor.name + ": " + e.message : __describe(e, 0)));
    }
}
function __phase(name) {
    print(name + " " + __hash);
}
const __jit = typeof jit === "object" ? jit : { compile() {}, prepare() {}, discard() {} };
"""


class Context:
    """What the statements generated in one function can refer to."""

    def __init__(self, generator: "Generator", parameters: List[str], depth: int):
        self.generator = generator
        self.parameters = parameters
        self.locals: List[str] = []
        # NB: Loop counters are read but never assigned, so that every loop ends.
        self.counters: List[str] = []
        self.depth = depth
        self.loop_depth = 0

    def variables(self) -> List[str]:
        return self.parameters + self.locals + self.counters

    def assignable(self) -> List[str]:
        return self.parameters + self.locals


class Generator:
    def __init__(self, seed: int):
        self.random = random.Random(seed)
        self.seed = seed
        self.lines: List[str] = []
        self.functions: List[tuple] = []
        self.next_local = 0

    # Values -----------------------------------------------------------------------------------------------------

    def int_literal(self) -> str:
        r = self.random
        return str(r.choice([0, 1, 2, 3, 5, 7, 10, 31, 100, 255, 1000, -1, -7, 2147483647, -2147483648, 1073741824]))

    def double_literal(self) -> str:
        return self.random.choice(["0.5", "1.5", "-2.25", "0.1", "1e300", "-0", "NaN", "Infinity", "-Infinity", "3.0"])

    def string_literal(self) -> str:
        return self.random.choice(['""', '"a"', '"abc"', '"12"', '" 7 "', '"x"', '"length"', '"0"', '"\\u00e9"'])

    def literal(self) -> str:
        r = self.random.random()
        if r < 0.45:
            return f"({self.int_literal()})"
        if r < 0.6:
            return f"({self.double_literal()})"
        if r < 0.75:
            return self.string_literal()
        return self.random.choice(["true", "false", "undefined", "null", "1n", "[]", "{}", "[1, 2]"])

    def shape_literal(self, context: Context) -> str:
        r = self.random
        properties = r.sample(["x", "y", "z", "w", "length", "0"], r.randint(1, 3))
        # NB: Values that are often plain operands give the JIT literals it can initialize from constants.
        parts = [f"{json_key(p)}: {self.property_value(context)}" for p in properties]
        extra = r.random()
        if extra < 0.1:
            parts.append(f"{json_key(properties[0])}: {self.property_value(context)}")
        elif extra < 0.2:
            parts.insert(0, f"...{self.receiver(context)}")
        elif extra < 0.25:
            parts.append(f"get g() {{ return {self.operand(context)}; }}")
        elif extra < 0.3:
            parts.append(f"[{self.string_literal()}]: {self.operand(context)}")
        elif extra < 0.33:
            parts.append("__proto__: shared")
        return "{ " + ", ".join(parts) + " }"

    def property_value(self, context: Context) -> str:
        return self.operand(context) if self.random.random() < 0.6 else self.expression(context, 1)

    # Expressions ------------------------------------------------------------------------------------------------

    def operand(self, context: Context) -> str:
        variables = context.variables()
        if variables and self.random.random() < 0.75:
            return self.random.choice(variables)
        return self.literal()

    def expression(self, context: Context, depth: int = 0) -> str:
        r = self.random
        if depth > 2:
            return self.operand(context)
        choice = r.random()
        a = self.expression(context, depth + 1)
        if choice < 0.30:
            op = r.choice(["+", "-", "*", "/", "%", "&", "|", "^", "<<", ">>", ">>>", "**"])
            return f"({a} {op} {self.expression(context, depth + 1)})"
        if choice < 0.42:
            op = r.choice(["<", "<=", ">", ">=", "==", "!=", "===", "!=="])
            return f"({a} {op} {self.expression(context, depth + 1)})"
        if choice < 0.47:
            return f"({r.choice(['-', '+', '~', '!', 'typeof '])}{a})"
        if choice < 0.52:
            return f"({a} | 0)"
        if choice < 0.60:
            return f"{self.receiver(context)}.{r.choice(['x', 'y', 'z', 'length', 'w'])}"
        if choice < 0.66:
            return f"{self.receiver(context)}[{self.index(context)}]"
        if choice < 0.74 and self.functions and context.loop_depth == 0:
            return self.call(context, depth)
        if choice < 0.77:
            return f"({a} ? {self.expression(context, depth + 1)} : {self.expression(context, depth + 1)})"
        if choice < 0.80:
            return f"({a} ?? {self.expression(context, depth + 1)})"
        if choice < 0.83:
            return f"`${{{a}}}-${{{self.expression(context, depth + 1)}}}`"
        if choice < 0.86:
            kind = r.choice(["number", "string", "object", "undefined"])
            return f'(typeof {a} === "{kind}")'
        if choice < 0.88:
            return f"({self.string_literal()} in Object({a}))"
        if choice < 0.90:
            return f"({a} instanceof {r.choice(['Object', 'Array', 'Shape0', 'Number'])})"
        if choice < 0.92:
            return f"String({a}).length"
        if choice < 0.94:
            return (
                f"Math.{r.choice(['max', 'min', 'imul', 'floor', 'abs'])}({a}, {self.expression(context, depth + 1)})"
            )
        if choice < 0.96:
            return f"[{a}, {self.expression(context, depth + 1)}]"
        return r.choice(
            [
                f"String({a}).charCodeAt({self.index(context)})",
                f"String({a}).slice(1)",
                f"[1, , {a}][{self.index(context)}]",
                f"{self.receiver(context)}?.x",
                f"Object.keys(Object({a})).length",
                f"(({a}) instanceof Shape1 ? 1 : 2)",
                f'(typeof {a} === "object" && {a} !== null ? {a}.method?.() : {a})',
                f"Number({a})",
                f"({a}, {self.expression(context, depth + 1)})",
                f"{a}?.length",
                a,
            ]
        )

    def receiver(self, context: Context) -> str:
        r = self.random
        variables = context.variables()
        if variables and r.random() < 0.8:
            return r.choice(variables)
        return r.choice(["globalObject", "globalArray", "shared"])

    def index(self, context: Context) -> str:
        r = self.random
        choice = r.random()
        if choice < 0.5:
            return str(r.choice([0, 1, 2, 3, 0, 1]))
        if choice < 0.7 and context.variables():
            return f"({r.choice(context.variables())} & 3)"
        if choice < 0.8:
            return self.random.choice(["-1", "100", "1.5", '"1"', '"x"', "i"]) if context.loop_depth else "-1"
        return self.operand(context)

    def call(self, context: Context, depth: int) -> str:
        r = self.random
        name, arity = r.choice(self.functions)
        arguments = [
            self.expression(context, depth + 1) for _ in range(r.choice([arity, arity, arity, arity - 1, arity + 1]))
        ]
        style = r.random()
        if style < 0.7:
            return f"{name}({', '.join(arguments)})"
        if style < 0.8:
            return f"{name}.call({self.operand(context)}, {', '.join(arguments)})"
        if style < 0.9:
            return f"{name}.apply(undefined, [{', '.join(arguments)}])"
        return f"{name}(...[{', '.join(arguments)}])"

    # Statements -------------------------------------------------------------------------------------------------

    def new_local(self, context: Context) -> str:
        name = f"v{self.next_local}"
        self.next_local += 1
        context.locals.append(name)
        return name

    def statements(self, context: Context, count: int, indent: str) -> None:
        for _ in range(count):
            self.statement(context, indent)

    def statement(self, context: Context, indent: str) -> None:
        r = self.random
        emit = self.lines.append
        variables = context.variables()
        choice = r.random()
        if choice < 0.18 or not variables:
            value = self.expression(context)
            emit(f"{indent}let {self.new_local(context)} = {value};")
        elif choice < 0.33 and context.assignable():
            emit(f"{indent}{r.choice(context.assignable())} = {self.expression(context)};")
        elif choice < 0.40 and context.assignable():
            op = r.choice(["+=", "-=", "*=", "|=", "&=", "<<=", "++", "--"])
            target = r.choice(context.assignable())
            emit(
                f"{indent}{target}{op};" if op in ("++", "--") else f"{indent}{target} {op} {self.expression(context)};"
            )
        elif choice < 0.48:
            receiver = self.receiver(context)
            emit(
                f'{indent}if ({receiver} !== null && typeof {receiver} === "object") {receiver}.{r.choice(["x", "y", "z", "w"])} = {self.expression(context)};'
            )
        elif choice < 0.53:
            receiver = self.receiver(context)
            emit(
                f"{indent}if (Array.isArray({receiver})) {receiver}[{self.index(context)}] = {self.expression(context)};"
            )
        elif choice < 0.58:
            value = self.shape_literal(context)
            emit(f"{indent}let {self.new_local(context)} = {value};")
        elif choice < 0.61:
            value = f"new Shape{r.randint(0, 1)}({self.expression(context)}, {self.expression(context)})"
            emit(f"{indent}let {self.new_local(context)} = {value};")
        elif choice < 0.64:
            value = f"[{', '.join(self.expression(context, 1) for _ in range(r.randint(0, 4)))}]"
            emit(f"{indent}let {self.new_local(context)} = {value};")
        elif choice < 0.72 and context.depth < 3:
            emit(f"{indent}if ({self.expression(context)}) {{")
            context.depth += 1
            saved = list(context.locals)
            self.statements(context, r.randint(1, 3), indent + "    ")
            context.locals = list(saved)
            if r.random() < 0.5:
                emit(f"{indent}}} else {{")
                self.statements(context, r.randint(1, 3), indent + "    ")
                context.locals = saved
            context.depth -= 1
            emit(f"{indent}}}")
        elif choice < 0.79 and context.depth < 3 and context.loop_depth < 2:
            counter = "i" if context.loop_depth == 0 else "j"
            emit(f"{indent}for (let {counter} = 0; {counter} < {r.choice([2, 3, 5, 8])}; ++{counter}) {{")
            context.loop_depth += 1
            context.depth += 1
            saved = list(context.locals)
            context.counters.append(counter)
            self.statements(context, r.randint(1, 4), indent + "    ")
            context.counters.pop()
            context.locals = saved
            context.depth -= 1
            context.loop_depth -= 1
            emit(f"{indent}}}")
        elif choice < 0.84 and context.depth < 3:
            emit(f"{indent}try {{")
            context.depth += 1
            saved = list(context.locals)
            self.statements(context, r.randint(1, 3), indent + "    ")
            if r.random() < 0.4:
                emit(
                    f'{indent}    if ({self.expression(context)}) throw new RangeError("thrown " + {self.operand(context)});'
                )
            context.locals = saved
            emit(f"{indent}}} catch (e) {{")
            emit(f"{indent}    __record(e instanceof Error ? e.message : e);")
            if r.random() < 0.3:
                emit(f"{indent}}} finally {{")
                emit(f"{indent}    __record({self.operand(context)});")
            context.depth -= 1
            emit(f"{indent}}}")
        elif choice < 0.88 and self.functions and context.loop_depth == 0:
            emit(f"{indent}__record({self.call(context, 0)});")
        elif choice < 0.90:
            emit(f"{indent}__record({self.expression(context)});")
        elif choice < 0.92:
            emit(f'{indent}if ({self.expression(context)}) throw new TypeError("bail " + {self.operand(context)});')
        elif choice < 0.94:
            body = f"p {r.choice(['+', '*', '-'])} {self.operand(context)}"
            argument = self.expression(context)
            name = self.new_local(context)
            emit(f"{indent}let {name} = (p) => {body};")
            emit(f"{indent}__record({name}({argument}));")
        elif choice < 0.96:
            emit(
                f"{indent}for (const element of [{self.operand(context)}, {self.operand(context)}]) __record(typeof element);"
            )
        elif choice < 0.97:
            emit(f"{indent}__record(arguments.length + {self.int_literal()});")
        elif choice < 0.975 and context.assignable():
            a, b = r.choice(context.assignable()), r.choice(context.assignable())
            emit(f"{indent}[{a}, {b}] = [{b}, {a}];")
        elif choice < 0.98:
            receiver = self.receiver(context)
            emit(f"{indent}for (const key in Object({receiver})) __record(key);")
        else:
            receiver = self.receiver(context)
            emit(
                f'{indent}if ({receiver} !== null && typeof {receiver} === "object") delete {receiver}.{r.choice(["x", "y", "z"])};'
            )

    def function(self, index: int) -> None:
        r = self.random
        arity = r.randint(1, 3)
        parameters = [f"p{index}_{k}" for k in range(arity)]
        name = f"f{index}"
        self.lines.append(f"function {name}({', '.join(parameters)}) {{")
        context = Context(self, parameters, 0)
        # NB: Small functions get inlined into their callers.
        self.statements(context, r.randint(0, 2) if r.random() < 0.5 else r.randint(3, 10), "    ")
        self.lines.append(f"    return {self.expression(context)};")
        self.lines.append("}")
        self.functions.append((name, arity))

    # Values that warm up functions with stable types, and those that come later ------------------------------

    STABLE_ARGUMENTS = [
        "i",
        "i * 2 + 1",
        "(i & 7) - 3",
        "i + 0.5",
        '"s" + (i & 3)',
        "objects[i & 1]",
        "arrays[i & 1]",
        "i & 1 ? 3 : 4",
        "new Shape0(i, i + 1)",
    ]

    LATER_ARGUMENTS = [
        "2147483647",
        "-2147483648",
        "-0",
        "NaN",
        "1.5",
        '"7"',
        '"abc"',
        "undefined",
        "null",
        "true",
        "3n",
        "Symbol.iterator",
        "[1, , 3]",
        '[1.5, "two"]',
        "{ y: 1, x: 2 }",
        "Object.create({ x: 5 })",
        "Object.freeze({ x: 1, y: 2 })",
        "new Proxy({ x: 1 }, {})",
        "{ get x() { return 9; }, y: 1 }",
        "{ valueOf() { return 4; } }",
        '{ valueOf() { throw new Error("valueOf"); } }',
        "new Shape1(1, 2)",
        "new Int32Array([1, 2, 3])",
        "new Float64Array([0.5, NaN])",
        '""',
        "function () {}",
    ]

    PERTURBATIONS = [
        'Object.defineProperty(Shape0.prototype, "z", { get() { return 42; }, configurable: true });',
        "Shape0.prototype.method = function () { return -1; };",
        "Object.setPrototypeOf(objects[0], { x: 100, w: 3 });",
        'objects[1] = { w: 1, x: "string", y: 2.5 };',
        "arrays[0][10] = 1;",
        'arrays[1].push("tail");',
        "Object.freeze(arrays[1]);",
        "Object.freeze(objects[0]);",
        "delete globalObject.x;",
        "globalObject.x = 1.5;",
        "globalArray.length = 0;",
        'Array.prototype[1] = "from Array.prototype";',
        'Object.prototype.y = "from Object.prototype";',
        "shared.x = { nested: true };",
        "objects[0] = new Proxy(objects[0], {});",
    ]

    def program(self) -> str:
        r = self.random
        self.lines = [PRELUDE.replace("SEED", str(r.randint(1, 1 << 30)))]
        emit = self.lines.append
        emit("class Shape0 {")
        emit("    constructor(a, b) {")
        emit("        this.x = a;")
        emit("        this.y = b;")
        emit("    }")
        emit("    method() {")
        emit("        return this.x;")
        emit("    }")
        emit("}")
        emit("class Shape1 extends Shape0 {")
        emit("    constructor(a, b) {")
        emit("        super(b, a);")
        emit("        this.z = a;")
        emit("    }")
        emit("}")
        emit("const objects = [{ x: 1, y: 2 }, { x: 3, y: 4 }];")
        emit("const arrays = [[1, 2, 3, 4], [5, 6, 7, 8]];")
        emit("const globalObject = { x: 1, y: 2, z: 3 };")
        emit("const globalArray = [1, 2, 3];")
        emit("const shared = new Shape0(1, 2);")
        function_count = r.randint(2, 6)
        for index in range(function_count):
            self.function(index)

        def call_line(index: int, arguments_pool: List[str], indent: str) -> str:
            name, arity = self.functions[index]
            arguments = ", ".join(r.choice(arguments_pool) for _ in range(arity))
            return f"{indent}__call(() => {name}({arguments}));"

        warm_iterations = r.choice([5, 20, 50, 100])
        for phase in range(r.randint(2, 4)):
            emit(f"for (let i = 0; i < {warm_iterations}; ++i) {{")
            for index in range(function_count):
                if r.random() < 0.8:
                    emit(call_line(index, self.STABLE_ARGUMENTS[: r.randint(1, len(self.STABLE_ARGUMENTS))], "    "))
            emit("}")
            for index in range(function_count):
                if r.random() < 0.3:
                    emit(f"__jit.compile(f{index});")
            emit(f'__phase("warm {phase}");')
            for _ in range(r.randint(1, 3)):
                if r.random() < 0.6:
                    emit(f"try {{ {r.choice(self.PERTURBATIONS)} }} catch (e) {{ __record(e.message); }}")
            if r.random() < 0.2:
                index = r.randrange(function_count)
                name, arity = self.functions[index]
                emit(f"f{index} = function (...rest) {{ return rest.length; }};")
            # NB: The stable arguments use the loop variable, which the calls after the loop need too.
            emit(f"{{ let i = {r.randrange(warm_iterations)};")
            for _ in range(r.randint(2, 8)):
                emit(call_line(r.randrange(function_count), self.LATER_ARGUMENTS + self.STABLE_ARGUMENTS, "    "))
            emit("}")
            emit(f'__phase("changed {phase}");')
        emit("delete Array.prototype[1];")
        emit("delete Object.prototype.y;")
        return "\n".join(self.lines) + "\n"


def json_key(key: str) -> str:
    return key if key.isidentifier() else f'"{key}"'


def generate(seed: int) -> str:
    return Generator(seed).program()


if __name__ == "__main__":
    import sys

    print(generate(int(sys.argv[1]) if len(sys.argv) > 1 else 1), end="")
