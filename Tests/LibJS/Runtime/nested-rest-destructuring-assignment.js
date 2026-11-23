test("nested array pattern that starts with a rest element preserves the assignment result", () => {
    const o = {};
    const source = [[1, 2]];
    const result = ([[...o.x]] = source);
    expect(o.x).toEqual([1, 2]);
    expect(result).toBe(source);
});

test("nested array pattern that starts with a rest element preserves the values around it", () => {
    const o = {};
    const array = [1, ([[...o.x]] = [[2]]), 3];
    expect(array).toEqual([1, [[2]], 3]);
    expect(o.x).toEqual([2]);

    const object = { a: 1, b: ([[...o.y]] = [[3, 4]]) };
    expect(object).toEqual({ a: 1, b: [[3, 4]] });
    expect(o.y).toEqual([3, 4]);

    expect(1 + ([[...o.z]] = [[5]]).length).toBe(2);
});

test("nested array pattern that starts with a rest element preserves the callee and earlier arguments", () => {
    const o = {};
    const source = { p: [1] };
    const receiver = {
        check(first, result) {
            expect(this).toBe(receiver);
            expect(first).toBe("before");
            expect(result).toBe(source);
            return "called";
        },
    };
    expect(
        receiver.check(
            "before",
            ({
                p: [...o.x],
            } = source)
        )
    ).toBe("called");
    expect(o.x).toEqual([1]);
});

test("rest elements with targets of every kind at the start of a nested array pattern", () => {
    const key = "k";
    const o = {};
    class Base {
        set x(value) {
            this.fromSuper = value;
        }
    }
    class Derived extends Base {
        #p;
        run() {
            return [
                ([[...o[key]]] = [[1]]),
                ([[...super.x]] = [[2]]),
                ([[...this.#p]] = [[3]]),
                ([[...[o.a]]] = [[4]]),
                ([[...{ [key]: o.b }]] = [[5]]),
                ([[...o.c] = [6]] = []),
                ([...[...o.d]] = [7]),
                ([, [...o.e]] = [0, [8]]),
            ].length;
        }
        get p() {
            return this.#p;
        }
    }
    const derived = new Derived();
    expect(derived.run()).toBe(8);
    expect(o.k).toEqual([1]);
    expect(derived.fromSuper).toEqual([2]);
    expect(derived.p).toEqual([3]);
    expect(o.a).toBe(4);
    expect(o.b).toBeUndefined();
    expect(o.c).toEqual([6]);
    expect(o.d).toEqual([7]);
    expect(o.e).toEqual([8]);
});
