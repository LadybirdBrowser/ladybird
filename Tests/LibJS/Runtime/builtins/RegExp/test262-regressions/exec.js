// https://github.com/tc39/test262/blob/main/test/staging/sm/RegExp/exec.js
describe("RegExp.prototype.exec", () => {
    function checkExec(description, regex, args, obj) {
        const lastIndex = obj.lastIndex;
        const index = obj.index;
        const input = obj.input;
        const indexArray = obj.indexArray;

        const res = regex.exec.apply(regex, args);

        expect(Array.isArray(res)).toBe(true);
        expect(regex.lastIndex).toBe(lastIndex);
        expect(res.index).toBe(index);
        expect(res.input).toBe(input);
        expect(res.length).toBe(indexArray.length);
        for (let i = 0, sz = indexArray.length; i < sz; i++) expect(res[i]).toBe(indexArray[i]);
    }

    test("exec called on non-RegExp throws TypeError", () => {
        const exec = RegExp.prototype.exec;
        expect(() => {
            exec.call(null);
        }).toThrow(TypeError);
        expect(() => {
            exec.call("");
        }).toThrow(TypeError);
        expect(() => {
            exec.call(5);
        }).toThrow(TypeError);
        expect(() => {
            exec.call({});
        }).toThrow(TypeError);
        expect(() => {
            exec.call([]);
        }).toThrow(TypeError);
        expect(() => {
            exec.call();
        }).toThrow(TypeError);
        expect(() => {
            exec.call(true);
        }).toThrow(TypeError);
        expect(() => {
            exec.call(Object.create(RegExp.prototype));
        }).toThrow(TypeError);
        expect(() => {
            exec.call(Object.create(/a/));
        }).toThrow(TypeError);
    });

    test("exec converts string argument with ToString", () => {
        let called = false;
        let r = /a/;
        expect(r.lastIndex).toBe(0);

        checkExec(
            "/a/",
            r,
            [
                {
                    toString: function () {
                        called = true;
                        return "ba";
                    },
                },
            ],
            { lastIndex: 0, index: 1, input: "ba", indexArray: ["a"] }
        );
        expect(called).toBe(true);

        called = false;
        expect(() => {
            r.exec({
                toString: null,
                valueOf: function () {
                    called = true;
                    throw 17;
                },
            });
        }).toThrow();

        expect(called).toBe(true);

        called = false;
        let obj = (r.lastIndex = {
            valueOf: function () {
                expect().fail("shouldn't have been called");
            },
        });
        expect(() => {
            r.exec({
                toString: null,
                valueOf: function () {
                    expect(called).toBe(false);
                    called = true;
                    throw 17;
                },
            });
        }).toThrow();

        expect(called).toBe(true);
        expect(r.lastIndex).toBe(obj);
    });

    test("exec ToInteger conversion on lastIndex throws", () => {
        let r = /b/;
        r.lastIndex = { valueOf: {}, toString: {} };
        expect(() => {
            r.exec("foopy");
        }).toThrow(TypeError);
        r.lastIndex = {
            valueOf: function () {
                throw new TypeError();
            },
        };
        expect(() => {
            r.exec("foopy");
        }).toThrow(TypeError);
    });

    test("exec ignores lastIndex for non-global regexps", () => {
        let obj = {
            valueOf: function () {
                return 5;
            },
        };
        let r = /abc/;
        r.lastIndex = obj;

        checkExec("/abc/ take one", r, ["abc-------abc"], {
            lastIndex: obj,
            index: 0,
            input: "abc-------abc",
            indexArray: ["abc"],
        });

        checkExec("/abc/ take two", r, ["abc-------abc"], {
            lastIndex: obj,
            index: 0,
            input: "abc-------abc",
            indexArray: ["abc"],
        });
    });

    test("exec handles negative lastIndex", () => {
        let r = /abc()?/;
        r.lastIndex = -5;
        checkExec("/abc()?/ with lastIndex -5", r, ["abc-------abc"], {
            lastIndex: -5,
            index: 0,
            input: "abc-------abc",
            indexArray: ["abc", undefined],
        });

        r = /abc/;
        r.lastIndex = -17;
        let res = r.exec("cdefg");
        expect(res).toBe(null);
        expect(r.lastIndex).toBe(-17);

        r = /abc/g;
        r.lastIndex = -42;
        res = r.exec("cdefg");
        expect(res).toBe(null);
        expect(r.lastIndex).toBe(0);
    });

    test("exec updates lastIndex for global regexps", () => {
        let r = /abc/g;
        r.lastIndex = 17;
        expect(r.exec("sdfs")).toBe(null);
        expect(r.lastIndex).toBe(0);

        r = /abc/g;
        r.lastIndex = 2;
        checkExec("/abc/g", r, ["00abc"], { lastIndex: 5, index: 2, input: "00abc", indexArray: ["abc"] });

        r = /a(b)c/g;
        r.lastIndex = 2;
        checkExec("/a(b)c/g take two", r, ["00abcd"], {
            lastIndex: 5,
            index: 2,
            input: "00abcd",
            indexArray: ["abc", "b"],
        });
    });
});
