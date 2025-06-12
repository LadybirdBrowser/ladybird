test("length is 1", () => {
    expect(Set.prototype.forEach).toHaveLength(1);
});

describe("errors", () => {
    test("requires at least one argument", () => {
        expect(() => {
            new Set().forEach();
        }).toThrowWithMessage(TypeError, "undefined is not a function");
    });

    test("callback must be a function", () => {
        expect(() => {
            new Set().forEach(undefined);
        }).toThrowWithMessage(TypeError, "undefined is not a function");
    });
});

describe("normal behavior", () => {
    test("never calls callback with empty set", () => {
        var callbackCalled = 0;
        expect(
            new Set().forEach(() => {
                callbackCalled++;
            })
        ).toBeUndefined();
        expect(callbackCalled).toBe(0);
    });

    test("calls callback once for every item", () => {
        var callbackCalled = 0;
        expect(
            new Set([1, 2, 3]).forEach(() => {
                callbackCalled++;
            })
        ).toBeUndefined();
        expect(callbackCalled).toBe(3);
    });

    test("callback receives value twice and set", () => {
        var a = new Set([1, 2, 3]);
        a.forEach((value1, value2, set) => {
            expect(a.has(value1)).toBeTrue();
            expect(value1).toBe(value2);
            expect(set).toBe(a);
        });
    });
});

describe("modification during iteration", () => {
    test("adding items during forEach also get visited", () => {
        const set = new Set([1, 2]);
        const visited = [];
        set.forEach(val => {
            if (val <= 2) set.add(4 * val);

            visited.push(val);
        });
        expect(set).toHaveSize(4);

        expect(visited).toEqual([1, 2, 4, 8]);
    });

    test("removing an item before it is visited means it doesn't get visited", () => {
        const set = new Set([1, 2, 3]);
        const visited = [];
        set.forEach(val => {
            visited.push(val);
            if (val === 1) {
                expect(set.delete(2)).toBeTrue();
            } else {
                expect(val).toBe(3);
                expect(set.delete(2)).toBeFalse();
            }
        });
        expect(set).toHaveSize(2);
        expect(visited).toEqual([1, 3]);
    });

    test("removing an item after it was visited and adding it again means it gets visited twice", () => {
        const set = new Set([1, 2, 3]);
        const visited = [];
        set.forEach(val => {
            visited.push(val);
            if (val === 2) {
                expect(set.delete(1)).toBeTrue();
            } else if (val === 3) {
                expect(set).toHaveSize(2);
                set.add(1);
                expect(set).toHaveSize(3);
            }
        });
        expect(set).toHaveSize(3);
        expect(visited).toEqual([1, 2, 3, 1]);
    });

    test("adding a new item and removing it before it gets visited means it never gets visited", () => {
        const set = new Set([1, 2]);
        const visited = [];
        set.forEach(val => {
            visited.push(val);
            if (val === 1) {
                set.add(3);
                expect(set).toHaveSize(3);
            } else if (val === 2) {
                expect(set).toHaveSize(3);
                expect(set.delete(3)).toBeTrue();
                expect(set).toHaveSize(2);
            }
            expect(val).not.toBe(3);
        });
        expect(set).toHaveSize(2);
        expect(visited).toEqual([1, 2]);
    });

    test("removing and adding in the same iterations", () => {
        const set = new Set([1, 2, 3]);
        const visited = [];
        let first = true;
        set.forEach(val => {
            visited.push(val);
            if (val === 1 && first) {
                expect(set.delete(1)).toBeTrue();
                set.add(1);
            }

            first = false;
        });
        expect(set).toHaveSize(3);

        expect(visited).toEqual([1, 2, 3, 1]);
    });

    test("removing and readding the same item means it can get visited n times", () => {
        let n = 3;

        const set = new Set([1, 2]);

        const visited = [];
        set.forEach(val => {
            visited.push(val);
            if (n-- > 0) {
                expect(set.delete(val)).toBeTrue();
                set.add(val);
            }
        });

        expect(set).toHaveSize(2);
        expect(visited).toEqual([1, 2, 1, 2, 1]);
    });
});
