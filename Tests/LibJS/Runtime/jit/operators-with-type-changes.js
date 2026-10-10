// Every operator, compiled after warming up with operands of one kind, then given operands of every other kind: the
// compiled code must produce what the interpreter produces (or exit to it), with the same side effects, and keep
// working for the kind it was compiled for afterwards.
//
// The cases are generated from the tables below, so a new operator or value kind is one line. Each case compiles a
// fresh function (new Function gives it its own feedback), and a twin of it that is never compiled is the reference.

const operandKinds = {
    int32: [0, 1, -7, 42, 1000, 3],
    int32Edges: [2147483647, -2147483648, 1073741824, -1],
    negativeZero: [-0],
    double: [1.5, -2.25, 0.1, 1e300],
    nan: [NaN],
    infinity: [Infinity, -Infinity],
    numericString: ["3", " 12 ", "", "0x10"],
    string: ["abc", "a", "été", "zz"],
    boolean: [true, false],
    undefined: [undefined],
    null: [null],
    valueOfObject: [log => ({ valueOf: () => (log.push("valueOf"), 5) })],
    toStringObject: [log => ({ valueOf: undefined, toString: () => (log.push("toString"), "7") })],
    array: [() => [2], () => []],
    bigint: [1n, -3n, 2n ** 64n],
    symbol: [Symbol.iterator],
    throwingObject: [
        log => ({
            valueOf() {
                log.push("throw");
                throw new Error("valueOf threw");
            },
        }),
    ],
    plainObject: [() => ({ a: 1 })],
    func: [() => function f() {}],
};

// The kinds operands have while the functions warm up: those with feedback the JIT speculates on.
const warmKinds = ["int32", "double", "string", "boolean", "valueOfObject", "undefined"];

const binaryOperators = [
    "+",
    "-",
    "*",
    "/",
    "%",
    "**",
    "&",
    "|",
    "^",
    "<<",
    ">>",
    ">>>",
    "==",
    "!=",
    "===",
    "!==",
    "<",
    "<=",
    ">",
    ">=",
];

const unaryOperators = ["-", "+", "~", "!", "typeof ", "void "];

// Statements that use operands `a` and `b` and return a result, beyond the plain operators.
const compoundForms = [
    ["increment", "let x = a; x++; return x;"],
    ["decrement", "let x = a; --x; return [x, x--, x];"],
    ["compound assignment", "let x = a; x += b; x *= 2; return x;"],
    ["conditional", "return a ? b : a;"],
    ["logical and", "return a && b;"],
    ["logical or", "return a || b;"],
    ["nullish", "return a ?? b;"],
    ["comparison branch", "if (a < b) return 1; if (a > b) return 2; return 3;"],
    ["equality branch", "if (a === b) return 'same'; return a == b ? 'loose' : 'different';"],
    ["loop", "let s = 0; for (let i = 0; i < 4; ++i) s = s + a * i - b; return s;"],
    ["int32 loop", "let s = 0; for (let i = a, n = 0; i < 6 && n < 8; ++i, ++n) s = (s + i) | 0; return s;"],
    ["template", "return `${a}:${b}`;"],
    ["in", "return a in { 1: 1, abc: 2 };"],
    [
        "switch",
        "switch (a) { case 1: return 'one'; case 'abc': return 'abc'; case b: return 'b'; default: return 'other'; }",
    ],
];

let nextValueIndex = 0;

function operandValue(kind, log) {
    const values = operandKinds[kind];
    const value = values[nextValueIndex++ % values.length];
    // NB: Objects are made fresh for every call, by a function that gets the log of side effects.
    return typeof value === "function" ? value(log) : value;
}

function describeValue(value) {
    if (typeof value === "bigint") return `${value}n`;
    if (typeof value === "symbol") return value.toString();
    if (Object.is(value, -0)) return "-0";
    if (typeof value === "function") return "function";
    if (Array.isArray(value)) return `[${value.map(describeValue).join(", ")}]`;
    if (typeof value === "string") return JSON.stringify(value);
    if (typeof value === "object" && value !== null) return "object";
    return String(value);
}

// What calling `f` with the operands did: its result or the exception it threw, and the side effects in order.
function outcome(f, kinds, indexSeed) {
    const log = [];
    nextValueIndex = indexSeed;
    const operands = kinds.map(kind => operandValue(kind, log));
    let result;
    try {
        result = `returned ${describeValue(f(...operands))}`;
    } catch (e) {
        result = `threw ${e.constructor.name}: ${e.message}`;
    }
    return `${result} [${log.join(", ")}]`;
}

// Warms up a fresh function with operands of `warmKind`, compiles it, then calls it with operands of every kind and
// compares each outcome with the reference function's. Returns the descriptions of the differences.
function checkTypeChanges(name, parameters, body, arity) {
    const failures = [];
    for (const warmKind of warmKinds) {
        const reference = new Function(...parameters, body);
        if (jit.enabled) jit.neverCompile(reference);
        const warmKinds = new Array(arity).fill(warmKind);
        // NB: Operators that throw for the warm kind (like ** of bigints) are not worth compiling for it.
        if (outcome(reference, warmKinds, 0).startsWith("threw")) continue;

        const compiled = new Function(...parameters, body);
        jit.prepare(compiled);
        for (let i = 0; i < 12; ++i) outcome(compiled, warmKinds, i);
        const wasCompiled = jit.compile(compiled);
        if (jit.enabled && !wasCompiled) {
            failures.push(`${name} warmed with ${warmKind}: not compiled`);
            continue;
        }
        for (const probeKind of Object.keys(operandKinds)) {
            const probeKinds = arity === 1 ? [probeKind] : [warmKind, probeKind];
            const swappedKinds = [probeKind, warmKind];
            for (const kinds of arity === 1 ? [probeKinds] : [probeKinds, swappedKinds]) {
                for (let seed = 0; seed < 2; ++seed) {
                    const expected = outcome(reference, kinds, seed);
                    const actual = outcome(compiled, kinds, seed);
                    if (expected !== actual)
                        failures.push(
                            `${name} warmed with ${warmKind}, called with ${kinds}: expected ${expected}, got ${actual}`
                        );
                }
            }
        }
        // The code (or its recompile) still works for the kind it was compiled for.
        for (let i = 0; i < 4; ++i) {
            const expected = outcome(reference, warmKinds, i);
            const actual = outcome(compiled, warmKinds, i);
            if (expected !== actual)
                failures.push(
                    `${name} warmed with ${warmKind}, called again with it: expected ${expected}, got ${actual}`
                );
        }
    }
    return failures;
}

describe("binary operators", () => {
    for (const operator of binaryOperators) {
        test(`a ${operator} b`, () => {
            expect(checkTypeChanges(`a ${operator} b`, ["a", "b"], `return a ${operator} b;`, 2)).toEqual([]);
        });
        test(`a ${operator} constant`, () => {
            const failures = [
                ...checkTypeChanges(`a ${operator} 3`, ["a"], `return a ${operator} 3;`, 1),
                ...checkTypeChanges(`(-1) ${operator} a`, ["a"], `return (-1) ${operator} a;`, 1),
            ];
            expect(failures).toEqual([]);
        });
    }
});

describe("unary operators", () => {
    for (const operator of unaryOperators) {
        test(`${operator}a`, () => {
            expect(checkTypeChanges(`${operator}a`, ["a"], `return ${operator}a;`, 1)).toEqual([]);
        });
    }
});

describe("compound forms", () => {
    for (const [name, body] of compoundForms) {
        test(name, () => {
            expect(checkTypeChanges(name, ["a", "b"], body, 2)).toEqual([]);
        });
    }
});
