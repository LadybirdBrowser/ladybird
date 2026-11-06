const repeat = (first, rest, count) => {
    const parts = [first];
    for (let i = 1; i < count; ++i) parts.push(rest(i));
    return parts.join("");
};

test("a long chain of one operator", () => {
    expect(eval(repeat("1", () => " + 1", 16000))).toBe(16000);
});

test("a long chain of alternating operators", () => {
    expect(eval(repeat("1", i => (i % 2 ? " * 1" : " + 1"), 16000))).toBe(8000);
});

test("a long chain of alternating logical operators", () => {
    expect(eval(repeat("1", i => (i % 2 ? " && 1" : " || 1"), 16000))).toBe(1);
});

test("a long chain in a function", () => {
    const f = new Function(`return ${repeat("1", () => " + 1", 16000)};`);
    expect(f()).toBe(16000);
});

test("a template normalizes line terminators", () => {
    const tag = strings => [strings[0], strings.raw[0]];
    expect(eval("tag`a\rb\r\nc`")).toEqual(["a\nb\nc", "a\nb\nc"]);
    expect(eval("`a\rb`")).toBe("a\nb");
});

describe("chains of operators of any length, at the top level of code", () => {
    const length = 500000;
    const prelude = "var a = function () { return a; }; a.b = a; a[0] = a;";
    const chains = {
        "binary operators": ["1" + "+1".repeat(length), length + 1],
        "logical operators": ["true" + "&&true".repeat(length), true],
        "member expressions": ["typeof a" + ".b".repeat(length), "function"],
        "computed member expressions": ["typeof a" + "[0]".repeat(length), "function"],
        calls: ["typeof a" + "()".repeat(length), "function"],
        "tagged templates": ["typeof a" + "``".repeat(length), "function"],
        "a chain in an operand": ["[0" + "+1".repeat(length) + "][0]", length],
        "a chain in a class field": ["new (class { x = 1" + "+1".repeat(length) + "; })().x", length + 1],
    };

    for (const [name, [source, expected]] of Object.entries(chains)) {
        test(name, () => {
            expect((0, eval)(`${prelude}${source}`)).toBe(expected);
            expect(new Function(`${prelude}return ${source}`)()).toBe(expected);
        });
    }

    test("in the targets of a destructuring assignment", () => {
        const member = (0, eval)(`${prelude}[a${".b".repeat(length)}.c] = [1]; a.c`);
        expect(member).toBe(1);
        const key = (0, eval)(`${prelude}[a[0${"+1".repeat(length)}]] = [1]; Object.keys(a).length`);
        expect(key).toBe(3);
    });
});

test("nested code that eval gets at the end of the stack does not run out of it", () => {
    const nested = "(".repeat(1900) + "1" + ")".repeat(1900);
    function atTheEndOfTheStack() {
        try {
            return atTheEndOfTheStack();
        } catch (error) {
            try {
                return eval(nested);
            } catch {
                return 1;
            }
        }
    }
    expect(atTheEndOfTheStack()).toBe(1);
});
