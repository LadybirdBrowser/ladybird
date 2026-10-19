// Compiled code starts for-of loops over arrays without asking the runtime while the realm's arrays are iterated with
// the realm's array iterators, whose next method is the builtin one, which it reads from the realm at every loop start.
// Changing either method, also while the code runs, makes the next loop start use the new method.
//
// NB: Every change of an iterator method makes the realm's arrays observably iterated for good, so the test that makes
//     one comes last, and array-iterator-next-dependency.js changes the other method in a realm of its own. The
//     functions come from the Function constructor, so that each run of a test (see LIBJS_TEST_REPEAT) compiles new
//     ones.

function compiled(parameters, body, ...warmUpArguments) {
    const f = new Function(...parameters, body);
    jit.prepare(f);
    for (let i = 0; i < 10; ++i) f(...warmUpArguments);
    expect(jit.compile(f)).toBe(jit.enabled);
    return f;
}

const collectBody = "const values = []; for (const value of iterable) values.push(value); return values;";

describe("for-of loops over arrays in compiled code", () => {
    test("iterables other than plain arrays of the realm take the slow path", () => {
        const collect = compiled(["iterable"], collectBody, [1, 2, 3]);
        expect(collect([4, 5])).toEqual([4, 5]);
        expect(collect([])).toEqual([]);
        expect(collect([1, , 3])).toEqual([1, undefined, 3]);
        const named = [1, 2];
        named.label = "named";
        expect(collect(named)).toEqual([1, 2]);
        const own = [1, 2];
        own[Symbol.iterator] = function* () {
            yield "own";
        };
        expect(collect(own)).toEqual(["own"]);
        class Subclass extends Array {}
        expect(collect(Subclass.of(3, 4))).toEqual([3, 4]);
        const otherPrototype = Object.setPrototypeOf([7, 8], {
            *[Symbol.iterator]() {
                yield "other";
            },
        });
        expect(collect(otherPrototype)).toEqual(["other"]);
        expect(() => collect(Object.setPrototypeOf([9], null))).toThrow(TypeError);
        expect(collect(new Map([[1, 2]]))).toEqual([[1, 2]]);
        expect(collect(new Set([9]))).toEqual([9]);
        expect(collect("hi")).toEqual(["h", "i"]);
        expect(collect(new Int8Array([1, 2]))).toEqual([1, 2]);
        expect(collect({ length: 2, 0: "a", 1: "b", [Symbol.iterator]: Array.prototype.values })).toEqual(["a", "b"]);
        expect(
            collect(
                (function* () {
                    yield 1;
                })()
            )
        ).toEqual([1]);
        expect(() => collect({})).toThrow(TypeError);
        expect(() => collect(undefined)).toThrow(TypeError);
        expect(collect([6, 7])).toEqual([6, 7]);
    });

    test("arrays that change while their loop runs", () => {
        const visit = compiled(
            ["array", "change"],
            "const values = []; for (const value of array) { values.push(value); change(array, values.length); } return values;",
            [1, 2, 3],
            () => {}
        );
        const grow = (array, count) => {
            if (count === 1) array.push(4);
        };
        const shrink = array => {
            array.length = 1;
        };
        const name = (array, count) => {
            if (count === 1) array.label = "named";
        };
        const ownIteratorMethod = (array, count) => {
            if (count === 1)
                array[Symbol.iterator] = function* () {
                    yield "own";
                };
        };
        expect(visit([1, 2, 3], grow)).toEqual([1, 2, 3, 4]);
        expect(visit([1, 2, 3], shrink)).toEqual([1]);
        expect(visit([1, 2, 3], name)).toEqual([1, 2, 3]);
        const array = [1, 2, 3];
        expect(visit(array, ownIteratorMethod)).toEqual([1, 2, 3]);
        expect(visit(array, () => {})).toEqual(["own"]);
    });

    test("loops in inlined callees", () => {
        const sum = new Function("values", "let total = 0; for (const value of values) total += value; return total;");
        jit.prepare(sum);
        const sumBoth = compiled(["sum", "a", "b"], "return sum(a) + sum(b);", sum, [1, 2], [3]);
        expect(sumBoth(sum, [1, 2, 3], [4])).toBe(10);
        const named = [5];
        named.label = "named";
        expect(sumBoth(sum, named, new Set([6]))).toBe(11);
        expect(sumBoth(sum, [], [1, , 2])).toBeNaN();
    });
});

describe("a changed iterator method", () => {
    // NB: The code calls the same function between its loops every time, so that it goes on in compiled code after it.
    let action = () => {};
    const between = () => action();

    test("an iterator method of arrays replaced while compiled code runs is called from the next loop on", () => {
        const originalIteratorMethod = Array.prototype[Symbol.iterator];
        const twoLoops = compiled(
            ["array", "between"],
            "let first = ''; for (const value of array) { first += value; between(); } let second = ''; for (const value of array) second += value; return [first, second];",
            [1, 2],
            between
        );
        action = () => {
            Array.prototype[Symbol.iterator] = function* () {
                yield "replaced";
            };
        };
        try {
            expect(twoLoops([1, 2], between)).toEqual(["12", "replaced"]);
            action = () => {};
            expect(twoLoops([1, 2], between)).toEqual(["replaced", "replaced"]);
        } finally {
            action = () => {};
            Array.prototype[Symbol.iterator] = originalIteratorMethod;
        }
        expect(twoLoops([1, 2], between)).toEqual(["12", "12"]);
    });
});
