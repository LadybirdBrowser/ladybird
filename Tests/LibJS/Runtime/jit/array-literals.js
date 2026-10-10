// JIT code allocates the arrays of array literals itself, with their elements
// packed. These tests run every function many times, so that with a low JIT
// threshold they are compiled.

const iterations = 100;

describe("array literals in JIT code", () => {
    test("elements, lengths and growth", () => {
        function pair(a, b) {
            return [a, b];
        }
        function empty() {
            return [];
        }
        function constants() {
            return [1, 2.5, "three", null, undefined, true];
        }
        for (let i = 0; i < iterations; ++i) {
            const array = pair(i, { value: i });
            expect(Array.isArray(array)).toBeTrue();
            expect(Object.getPrototypeOf(array)).toBe(Array.prototype);
            expect(array.length).toBe(2);
            expect(array[0]).toBe(i);
            expect(array[1].value).toBe(i);
            expect(array[2]).toBeUndefined();
            expect(Object.keys(array)).toEqual(["0", "1"]);

            const grown = empty();
            expect(grown.length).toBe(0);
            for (let j = 0; j < 20; ++j) grown.push(j);
            expect(grown.length).toBe(20);
            expect(grown[19]).toBe(19);

            const literal = constants();
            expect(literal).toEqual([1, 2.5, "three", null, undefined, true]);
            literal[10] = "far";
            expect(literal.length).toBe(11);
            expect(literal[8]).toBeUndefined();
            expect(8 in literal).toBeFalse();
            literal.length = 2;
            const holey = [i, , i + 1, ,];
            expect(holey.length).toBe(4);
            expect(1 in holey).toBeFalse();
            expect(3 in holey).toBeFalse();
            expect(holey[2]).toBe(i + 1);
            expect(literal).toEqual([1, 2.5]);
        }
    });

    test("arrays with more elements than allocate inline, and collections", () => {
        // NB: An array literal of 70 elements.
        const wide = new Function("i", `return [${Array(70).fill("i").join(", ")}];`);
        function nested(i) {
            const inner = [i, [i + 1, [i + 2]]];
            gc();
            return [inner, inner[1]];
        }
        for (let i = 0; i < iterations; ++i) {
            const array = wide(i);
            expect(array.length).toBe(70);
            expect(array.every(element => element === i)).toBeTrue();
            const tree = nested(i);
            expect(tree[0][1][1][0]).toBe(i + 2);
            expect(tree[1][0]).toBe(i + 1);
            expect(JSON.stringify(tree[0])).toBe(`[${i},[${i + 1},[${i + 2}]]]`);
        }
    });
});
