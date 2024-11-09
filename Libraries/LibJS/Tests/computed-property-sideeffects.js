let calledToStringError = {};
let throwingToString = {
    toString: () => {
        throw calledToStringError;
    },
};
let calledValueOfError = {};
let throwingValueOf = {
    toString: undefined,
    valueOf: () => {
        throw calledValueOfError;
    },
};
let calledToStringAccessorError = {};
let throwingToStringAccessor = {
    get toString() {
        throw calledToStringAccessorError;
    },
};
let calledValueOfAccessorError = {};
let throwingValueOfAccessor = {
    toString: undefined,
    get valueOf() {
        throw calledValueOfAccessorError;
    },
};

test("Exceptions thrown by computed properties are caught", () => {
    var i = 0;
    var j = 0;
    var k = 0;
    expect(() => {
        return { first: k++, [throwingToString]: i++, second: j++ };
    }).toThrow(calledToStringError);
    expect(i).toBe(1);
    expect(j).toBe(0);
    expect(k).toBe(1);
    expect(() => {
        return { first: k++, [throwingValueOf]: i++, second: j++ };
    }).toThrow(calledValueOfError);
    expect(i).toBe(2);
    expect(j).toBe(0);
    expect(k).toBe(2);
    expect(() => {
        return { first: k++, [throwingToStringAccessor]: i++, second: j++ };
    }).toThrow(calledToStringAccessorError);
    expect(i).toBe(3);
    expect(j).toBe(0);
    expect(k).toBe(3);
    expect(() => {
        return { first: k++, [throwingValueOfAccessor]: i++, second: j++ };
    }).toThrow(calledValueOfAccessorError);
    expect(i).toBe(4);
    expect(j).toBe(0);
    expect(k).toBe(4);
});

test("Test toString and valueOf are only called once", () => {
    var counter = 0;
    var key1 = {
        valueOf: function () {
            expect(counter++).toBe(0);
            return "a";
        },
        toString: null,
    };
    var key2 = {
        valueOf: function () {
            expect(counter++).toBe(1);
            return "b";
        },
        toString: null,
    };
    var key3 = {
        get toString() {
            expect(counter++).toBe(2);
            return function () {
                return "c";
            };
        },
    };
    var key4 = {
        get valueOf() {
            expect(counter++).toBe(3);
            return function () {
                return "d";
            };
        },
        toString: null,
    };
    var key5 = {
        valueOf: function () {
            expect(counter++).toBe(4);
            return 0;
        },
        toString: null,
    };
    var key6 = {
        valueOf: function () {
            expect(counter++).toBe(5);
            return 2;
        },
        toString: null,
    };
    var key7 = {
        get toString() {
            expect(counter++).toBe(6);
            return function () {
                return 4;
            };
        },
    };
    var key8 = {
        get valueOf() {
            expect(counter++).toBe(7);
            return function () {
                return 8;
            };
        },
        toString: null,
    };

    var object = {
        [key1]: "a",
        [key2]: "b",
        [key3]: "c",
        [key4]: "d",
        [key5]: "e",
        [key6]: "f",
        [key7]: "g",
        [key8]: "h",
    };
    expect(counter).toBe(8);
    expect(object.a).toBe("a");
    expect(object.b).toBe("b");
    expect(object.c).toBe("c");
    expect(object.d).toBe("d");
    expect(object[0]).toBe("e");
    expect(object[2]).toBe("f");
    expect(object[4]).toBe("g");
    expect(object[8]).toBe("h");
    expect(Object.getOwnPropertyNames(object) + "").toBe(
        ["0", "2", "4", "8", "a", "b", "c", "d"] + ""
    );
});
