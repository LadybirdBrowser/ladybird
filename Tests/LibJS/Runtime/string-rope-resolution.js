// Concatenated strings are ropes until something reads their code units. These tests read ropes of different shapes
// and sizes, small and large ones, built on either side, mixing ASCII and other code units.

function pieces(count, mixed) {
    const result = [];
    for (let i = 0; i < count; ++i) result.push(mixed && i % 3 === 1 ? `é${i}\u{1F600}` : `p${i}`);
    return result;
}

describe("resolving ropes", () => {
    test("ropes built left to right", () => {
        for (const mixed of [false, true]) {
            for (let count = 2; count < 40; ++count) {
                const parts = pieces(count, mixed);
                let rope = "";
                for (const part of parts) rope = rope + part;
                expect(rope.indexOf(parts[count - 1])).toBe(rope.length - parts[count - 1].length);
                expect(rope).toBe(parts.join(""));
            }
        }
    });

    test("ropes built right to left and as trees", () => {
        for (const mixed of [false, true]) {
            for (let count = 2; count < 40; ++count) {
                const parts = pieces(count, mixed);
                let rope = "";
                for (let i = count - 1; i >= 0; --i) rope = parts[i] + rope;
                expect(rope).toBe(parts.join(""));

                const tree = (from, to) =>
                    to - from === 1 ? parts[from] : tree(from, (from + to) >> 1) + tree((from + to) >> 1, to);
                const balanced = tree(0, count);
                expect(balanced.charCodeAt(balanced.length - 1)).toBe(parts.join("").charCodeAt(balanced.length - 1));
                expect(balanced).toBe(parts.join(""));
            }
        }
    });

    test("ropes that share sides", () => {
        const left = "a" + "b";
        const both = left + left;
        const again = both + left + both;
        expect(again).toBe("abababababab".slice(0, 10));
        expect(both).toBe("abab");
    });
});
