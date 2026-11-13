function argumentList(count, expression = "value++") {
    return Array(count).fill(expression).join(",");
}

function range(count, start = 0) {
    return Array.from({ length: count }, (_, index) => start + index);
}

describe("large call argument lists", () => {
    test("arguments are evaluated once, from left to right, across the operand limit", () => {
        for (const count of [199, 200, 201, 300]) {
            const call = new Function("collect", "value", `return collect(${argumentList(count)});`);
            expect(call((...values) => values, 0)).toEqual(range(count));
        }
    });

    test("later arguments preserve the callee, receiver, and earlier argument values", () => {
        const first = {
            method(...values) {
                return [this, values];
            },
        };
        const second = { method: () => "wrong method" };
        const call = new Function(
            "first",
            "second",
            "value",
            `let receiver = first;
             return receiver.method(${argumentList(200, "value")}, receiver = second, value = 99);`
        );
        expect(call(first, second, 1)).toEqual([first, [...Array(200).fill(1), second, 99]]);
    });

    test("optional calls skip every argument when the callee is nullish", () => {
        const call = new Function(
            "callee",
            "value",
            `const result = callee?.(${argumentList(300)}); return [result, value];`
        );
        expect(call(null, 0)).toEqual([undefined, 0]);
        expect(call((...values) => values, 0)).toEqual([range(300), 300]);
    });

    test("constructors and super calls receive every argument and the original new.target", () => {
        class Base {
            constructor(...values) {
                this.values = values;
                this.target = new.target;
            }
        }
        const construct = new Function("Base", "value", `return new Base(${argumentList(300)});`);
        const base = construct(Base, 0);
        expect(base.values).toEqual(range(300));
        expect(base.target).toBe(Base);

        const makeDerived = new Function(
            "Base",
            "value",
            `return class Derived extends Base { constructor() { super(${argumentList(300)}); } };`
        );
        const Derived = makeDerived(Base, 0);
        const derived = new Derived();
        expect(derived.values).toEqual(range(300));
        expect(derived.target).toBe(Derived);

        const makeDerivedWithSpread = new Function(
            "Base",
            "value",
            "iterable",
            `return class Derived extends Base { constructor() { super(${argumentList(201)}, ...iterable); } };`
        );
        const DerivedWithSpread = makeDerivedWithSpread(Base, 0, [700, 701]);
        expect(new DerivedWithSpread().values).toEqual([...range(201), 700, 701]);
    });

    test("direct eval retains the lexical environment and the evaluated callee", () => {
        const call = new Function(
            "eval",
            "value",
            `let visible = 37;
             return eval("visible", ${argumentList(199)}, eval = () => 99);`
        );
        expect(call(eval, 0)).toBe(37);
    });

    test("calls named async, nested calls, and spreads retain argument order", () => {
        const callAsync = new Function("async", "value", `return async(${argumentList(300)});`);
        expect(callAsync((...values) => values, 0)).toEqual(range(300));

        const call = new Function(
            "collect",
            "value",
            "iterable",
            `return collect(${argumentList(200)}, ...iterable, collect(${argumentList(300)}));`
        );
        expect(call((...values) => values, 0, [700, 701])).toEqual([...range(200), 700, 701, range(300, 200)]);
    });

    test("a large async arrow cover remains a parameter list", () => {
        let seen;
        const parameters = range(300)
            .map(index => `p${index}`)
            .join(",");
        const arrow = eval(`(async (${parameters}) => { seen = [p0, p299]; })`);
        expect(arrow).toHaveLength(300);
        expect(seen).toBeUndefined();
        arrow(...range(300));
        expect(seen).toEqual([0, 299]);
    });

    test("an abrupt argument stops evaluation before the call", () => {
        const error = new Error("argument failed");
        let evaluated = 0;
        let called = false;
        const call = new Function(
            "callee",
            "next",
            "fail",
            `return callee(${argumentList(200, "next()")}, fail(), ${argumentList(20, "next()")});`
        );
        try {
            call(
                () => {
                    called = true;
                },
                () => evaluated++,
                () => {
                    throw error;
                }
            );
            expect().fail("the argument should throw");
        } catch (caught) {
            expect(caught).toBe(error);
        }
        expect(evaluated).toBe(200);
        expect(called).toBeFalse();
    });
});
