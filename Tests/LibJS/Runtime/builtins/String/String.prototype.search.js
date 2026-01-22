test("basic functionality", () => {
    expect(String.prototype.search).toHaveLength(1);

    expect("hello friends".search("h")).toBe(0);
    expect("hello friends".search("e")).toBe(1);
    expect("hello friends".search("l")).toBe(2);
    expect("hello friends".search("o")).toBe(4);
    expect("hello friends".search("z")).toBe(-1);

    expect("abc123def".search(/\d/)).toBe(3);
    expect("abcdef".search(/\d/)).toBe(-1);
});

test("override exec with function", () => {
    let calls = 0;

    let re = /test/;
    let oldExec = re.exec.bind(re);
    re.exec = function (...args) {
        ++calls;
        return oldExec(...args);
    };

    expect("test".search(re)).toBe(0);
    expect(calls).toBe(1);
});

test("override exec with bad function", () => {
    let calls = 0;

    let re = /test/;
    re.exec = function (...args) {
        ++calls;
        return 4;
    };

    expect(() => {
        "test".search(re);
    }).toThrow(TypeError);
    expect(calls).toBe(1);
});

test("override exec with non-function", () => {
    let re = /test/;
    re.exec = 3;
    expect("test".search(re)).toBe(0);
});

test("UTF-16", () => {
    var s = "😀";
    expect(s.search("😀")).toBe(0);
    expect(s.search("\ud83d")).toBe(0);
    expect(s.search("\ude00")).toBe(1);
    expect(s.search("foo")).toBe(-1);

    s = "\u{80}\u{160}";
    expect(s.search("\u{80}")).toBe(0);
    expect(s.search("\u{160}")).toBe(1);
    expect(s.search("foo")).toBe(-1);
});
