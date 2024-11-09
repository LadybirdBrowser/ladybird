describe("ability to work with generic non-array objects", () => {
    test("push, pop", () => {
        [undefined, "foo", -42, 0].forEach(length => {
            const o = { length };

            expect(Array.prototype.push.call(o, "foo")).toBe(1);
            expect(o).toHaveLength(1);
            expect(o[0]).toBe("foo");
            expect(Array.prototype.push.call(o, "bar", "baz")).toBe(3);
            expect(o).toHaveLength(3);
            expect(o[0]).toBe("foo");
            expect(o[1]).toBe("bar");
            expect(o[2]).toBe("baz");

            expect(Array.prototype.pop.call(o)).toBe("baz");
            expect(o).toHaveLength(2);
            expect(Array.prototype.pop.call(o)).toBe("bar");
            expect(o).toHaveLength(1);
            expect(Array.prototype.pop.call(o)).toBe("foo");
            expect(o).toHaveLength(0);
            expect(Array.prototype.pop.call(o)).toBeUndefined();
            expect(o).toHaveLength(0);

            o.length = length;
            expect(Array.prototype.pop.call(o)).toBeUndefined();
            expect(o).toHaveLength(0);
        });
    });

    test("splice", () => {
        const o = { length: 3, 0: "hello", 2: "serenity" };
        const removed = Array.prototype.splice.call(o, 0, 2, "hello", "friends");
        expect(o).toHaveLength(3);
        expect(o[0]).toBe("hello");
        expect(o[1]).toBe("friends");
        expect(o[2]).toBe("serenity");
        expect(removed).toHaveLength(2);
        expect(removed[0]).toBe("hello");
        expect(removed[1]).toBeUndefined();
    });

    test("slice", () => {
        {
            const o = { length: 3, 0: "hello", 2: "serenity" };
            const slice = Array.prototype.slice.call(o, 0, 2);
            expect(o).toHaveLength(3);
            expect(o[0]).toBe("hello");
            expect(o[1]).toBeUndefined();
            expect(o[2]).toBe("serenity");
            expect(slice).toHaveLength(2);
            expect(slice[0]).toBe("hello");
            expect(slice[1]).toBeUndefined();
        }
        {
            const o = { length: 5, 0: "foo", 1: "bar", 3: "baz" };
            expect(Array.prototype.slice.call(o)).toEqual([
                "foo",
                "bar",
                undefined,
                "baz",
                undefined,
            ]);
            expect(Array.prototype.slice.call(o, 0, 3)).toEqual(["foo", "bar", undefined]);
            expect(Array.prototype.slice.call(o, 0, 15)).toEqual([
                "foo",
                "bar",
                undefined,
                "baz",
                undefined,
            ]);

            expect(Array.prototype.slice.call(o, 1)).toEqual(["bar", undefined, "baz", undefined]);
            expect(Array.prototype.slice.call(o, 15)).toEqual([]);

            expect(Array.prototype.slice.call(o, -1)).toEqual([undefined]);
            expect(Array.prototype.slice.call(o, -2)).toEqual(["baz", undefined]);

            expect(Array.prototype.slice.call(o, 1, -1)).toEqual(["bar", undefined, "baz"]);
            expect(Array.prototype.slice.call(o, 2, -2)).toEqual([undefined]);

            expect(Array.prototype.slice.call(o, 3, -3)).toEqual([]);
            expect(Array.prototype.slice.call(o, 0, 0)).toEqual([]);
            expect(Array.prototype.slice.call(o, 10, 10)).toEqual([]);
        }
    });

    test("join", () => {
        expect(Array.prototype.join.call({})).toBe("");
        expect(Array.prototype.join.call({ length: "foo" })).toBe("");
        expect(Array.prototype.join.call({ length: 3 })).toBe(",,");
        expect(Array.prototype.join.call({ length: 2, 0: "foo", 1: "bar" })).toBe("foo,bar");
        expect(Array.prototype.join.call({ length: 2, 0: "foo", 1: "bar", 2: "baz" })).toBe(
            "foo,bar"
        );
        expect(Array.prototype.join.call({ length: 3, 1: "bar" }, "~")).toBe("~bar~");
        expect(Array.prototype.join.call({ length: 3, 0: "foo", 1: "bar", 2: "baz" }, "~")).toBe(
            "foo~bar~baz"
        );
    });

    test("toString", () => {
        expect(Array.prototype.toString.call({})).toBe("[object Object]");
        expect(Array.prototype.toString.call({ join: "foo" })).toBe("[object Object]");
        expect(Array.prototype.toString.call({ join: () => "foo" })).toBe("foo");
    });

    test("indexOf", () => {
        expect(Array.prototype.indexOf.call({})).toBe(-1);
        expect(Array.prototype.indexOf.call({ 0: undefined })).toBe(-1);
        expect(Array.prototype.indexOf.call({ length: 1, 0: undefined })).toBe(0);
        expect(Array.prototype.indexOf.call({ length: 1, 2: "foo" }, "foo")).toBe(-1);
        expect(Array.prototype.indexOf.call({ length: 5, 2: "foo" }, "foo")).toBe(2);
        expect(Array.prototype.indexOf.call({ length: 5, 2: "foo", 4: "foo" }, "foo", 3)).toBe(4);
    });

    test("lastIndexOf", () => {
        expect(Array.prototype.lastIndexOf.call({})).toBe(-1);
        expect(Array.prototype.lastIndexOf.call({ 0: undefined })).toBe(-1);
        expect(Array.prototype.lastIndexOf.call({ length: 1, 0: undefined })).toBe(0);
        expect(Array.prototype.lastIndexOf.call({ length: 1, 2: "foo" }, "foo")).toBe(-1);
        expect(Array.prototype.lastIndexOf.call({ length: 5, 2: "foo" }, "foo")).toBe(2);
        expect(Array.prototype.lastIndexOf.call({ length: 5, 2: "foo", 4: "foo" }, "foo")).toBe(4);
        expect(Array.prototype.lastIndexOf.call({ length: 5, 2: "foo", 4: "foo" }, "foo", -2)).toBe(
            2
        );
    });

    test("includes", () => {
        expect(Array.prototype.includes.call({})).toBeFalse();
        expect(Array.prototype.includes.call({ 0: undefined })).toBeFalse();
        expect(Array.prototype.includes.call({ length: 1, 0: undefined })).toBeTrue();
        expect(Array.prototype.includes.call({ length: 1, 2: "foo" }, "foo")).toBeFalse();
        expect(Array.prototype.includes.call({ length: 5, 2: "foo" }, "foo")).toBeTrue();
    });

    test("shift", () => {
        expect(Array.prototype.shift.call({})).toBeUndefined();
        expect(Array.prototype.shift.call({ length: 0 })).toBeUndefined();

        const o = { length: 5, 0: "a", 1: "b", 3: "c" };
        const front = Array.prototype.shift.call(o);
        expect(front).toEqual("a");
        expect(o).toEqual({ length: 4, 0: "b", 2: "c" });
    });

    test("unshift", () => {
        {
            const o = { length: 5, 0: "a", 1: "b", 3: "c" };
            const front = "z";
            Array.prototype.unshift.call(o, front);
            expect(o[0]).toEqual(front);
            expect(o[1]).toEqual("a");
            expect(o.length).toEqual(6);
        }
        {
            const o = { length: 5, 0: "a", 1: "b", 3: "c" };
            const front = "z";
            Array.prototype.unshift.call(o, front, front);
            expect(o[0]).toEqual(front);
            expect(o[1]).toEqual(front);
            expect(o[2]).toEqual("a");
            expect(o.length).toEqual(7);
        }
    });

    test("copyWithin", () => {
        const initial_o = { length: 5, 0: "foo", 1: "bar", 3: "baz" };
        {
            const o = { length: 5, 0: "foo", 1: "bar", 3: "baz" };
            // returns value and modifies
            expect(Array.prototype.copyWithin.call(o, 0, 0)).toEqual(o);
            expect(o).toEqual(initial_o);
        }

        {
            const o = {};
            expect(Array.prototype.copyWithin.call(o, 1, 16, 32)).toEqual(o);
            expect(o).toEqual({});
        }

        {
            const o = { length: 100 };
            expect(Array.prototype.copyWithin.call(o, 1, 16, 32)).toEqual(o);
            expect(o).toEqual({ length: 100 });
        }

        {
            const o = { length: 5, 0: "foo", 1: "bar", 3: "baz" };
            // returns value and modifies
            expect(Array.prototype.copyWithin.call(o, 2, 0)).toEqual(o);
            expect(o).toEqual({ length: 5, 0: "foo", 1: "bar", 2: "foo", 3: "bar" });
        }
    });

    const o = { length: 5, 0: "foo", 1: "bar", 3: "baz" };

    test("every", () => {
        const visited = [];
        Array.prototype.every.call(o, value => {
            visited.push(value);
            return true;
        });
        expect(visited).toEqual(["foo", "bar", "baz"]);
    });

    test("find, findIndex", () => {
        ["find", "findIndex"].forEach(name => {
            const visited = [];
            Array.prototype[name].call(o, value => {
                visited.push(value);
                return false;
            });
            expect(visited).toEqual(["foo", "bar", undefined, "baz", undefined]);
        });
    });

    test("filter, forEach, map, some", () => {
        ["filter", "forEach", "map", "some"].forEach(name => {
            const visited = [];
            Array.prototype[name].call(o, value => {
                visited.push(value);
                return false;
            });
            expect(visited).toEqual(["foo", "bar", "baz"]);
        });
    });

    test("reduce", () => {
        const visited = [];
        Array.prototype.reduce.call(
            o,
            (_, value) => {
                visited.push(value);
                return false;
            },
            "initial"
        );
        expect(visited).toEqual(["foo", "bar", "baz"]);
    });

    test("reduceRight", () => {
        const visited = [];
        Array.prototype.reduceRight.call(
            o,
            (_, value) => {
                visited.push(value);
                return false;
            },
            "initial"
        );
        expect(visited).toEqual(["baz", "bar", "foo"]);
    });

    test("reverse", () => {
        const o = { length: 5, 0: "foo", 1: "bar", 3: "baz" };
        expect(Array.prototype.reverse.call(o)).toEqual({
            length: 5,
            4: "foo",
            3: "bar",
            1: "baz",
        });
        expect(Array.prototype.reverse.call({})).toEqual({});
        expect(Array.prototype.reverse.call({ length: 10 })).toEqual({ length: 10 });
        expect(Array.prototype.reverse.call({ length: 1, 0: "foo" })).toEqual({
            length: 1,
            0: "foo",
        });
    });

    test("concat", () => {
        expect(Array.prototype.concat.call(o)).toEqual([o]);
        expect(Array.prototype.concat.call(o, true)).toEqual([o, true]);
        expect(Array.prototype.concat.call({}, o)).toEqual([{}, o]);

        const spreadable = {
            length: 5,
            0: "foo",
            1: "bar",
            3: "baz",
            [Symbol.isConcatSpreadable]: true,
        };
        expect(Array.prototype.concat.call(spreadable)).toEqual([
            "foo",
            "bar",
            undefined,
            "baz",
            undefined,
        ]);
        expect(Array.prototype.concat.call(spreadable, [1, 2])).toEqual([
            "foo",
            "bar",
            undefined,
            "baz",
            undefined,
            1,
            2,
        ]);
        expect(Array.prototype.concat.call([1], spreadable, [2])).toEqual([
            1,
            "foo",
            "bar",
            undefined,
            "baz",
            undefined,
            2,
        ]);
    });

    test("toReversed", () => {
        const result = Array.prototype.toReversed.call(o);
        expect(result).toEqual([undefined, "baz", undefined, "bar", "foo"]);
        expect(result).not.toBe(o);
    });

    test("toSorted", () => {
        const result = Array.prototype.toSorted.call(o);
        expect(result).toEqual(["bar", "baz", "foo", undefined, undefined]);
        expect(result).not.toBe(o);
    });

    test("toSpliced", () => {
        const result = Array.prototype.toSpliced.call(o, 1, 2, "hello", "friends");
        expect(result).toEqual(["foo", "hello", "friends", "baz", undefined]);
        expect(result).not.toBe(o);
    });

    test("with", () => {
        const result = Array.prototype.with.call(o, 2, "hello");
        expect(result).toEqual(["foo", "bar", "hello", "baz", undefined]);
        expect(result).not.toBe(o);
    });
});
