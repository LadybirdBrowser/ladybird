test("operands survive a class whose computed key branches", () => {
    function run() {
        let a = 1;
        let object = {};
        let condition = "x";
        let key = 2;
        return [
            a,
            (object[
                class {
                    [condition ? key : 0]() {}
                }.name
            ] += 1),
        ];
    }
    const result = run();
    expect(result).toHaveLength(2);
    expect(result[0]).toBe(1);
    expect(result[1]).toBeNaN();
});

test("operands survive an array pattern whose default branches", () => {
    function run() {
        let a = 1;
        let b = 2;
        let condition = true;
        let target;
        const pattern = ([target = condition ? 3 : 4] = []);
        return [a, b, ([target = condition ? 5 : 6] = []), (a += b), target, pattern.length];
    }
    const result = run();
    expect(result[0]).toBe(1);
    expect(result[1]).toBe(2);
    expect(result[2]).toEqual([]);
    expect(result[3]).toBe(3);
    expect(result[4]).toBe(5);
    expect(result[5]).toBe(0);
});

test("operands survive an object pattern whose computed key branches", () => {
    function run() {
        let a = 1;
        let object = { x: 2 };
        let condition = true;
        let target;
        return [a, object, ({ [condition ? "x" : "y"]: target } = object), target, a];
    }
    const result = run();
    expect(result[0]).toBe(1);
    expect(result[1].x).toBe(2);
    expect(result[2].x).toBe(2);
    expect(result[3]).toBe(2);
    expect(result[4]).toBe(1);
});
