test("basic functionality", () => {
    expect(String.prototype.match).toHaveLength(1);

    expect("hello friends".match(/hello/)).not.toBeNull();
    expect("hello friends".match(/enemies/)).toBeNull();

    expect("aaa".match(/a/)).toEqual(["a"]);
    expect("aaa".match(/a/g)).toEqual(["a", "a", "a"]);

    expect("aaa".match(/b/)).toBeNull();
    expect("aaa".match(/b/g)).toBeNull();
});

test("override exec with function", () => {
    let calls = 0;

    let re = /test/;
    let oldExec = re.exec.bind(re);
    re.exec = function (...args) {
        ++calls;
        return oldExec(...args);
    };

    expect("test".match(re)).not.toBeNull();
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
        "test".match(re);
    }).toThrow(TypeError);
    expect(calls).toBe(1);
});

test("override exec with non-function", () => {
    let re = /test/;
    re.exec = 3;
    expect("test".match(re)).not.toBeNull();
});

test("UTF-16", () => {
    expect("😀".match("foo")).toBeNull();
    expect("😀".match("\ud83d")).toEqual(["\ud83d"]);
    expect("😀".match("\ude00")).toEqual(["\ude00"]);
    expect("😀😀".match("\ud83d")).toEqual(["\ud83d"]);
    expect("😀😀".match("\ude00")).toEqual(["\ude00"]);
    expect("😀😀".match(/\ud83d/g)).toEqual(["\ud83d", "\ud83d"]);
    expect("😀😀".match(/\ude00/g)).toEqual(["\ude00", "\ude00"]);
});

test("escaped code points", () => {
    var string = "The quick brown fox jumped over the lazy dog's back";

    var re = /(?<𝓑𝓻𝓸𝔀𝓷>brown)/u;
    expect(string.match(re).groups.𝓑𝓻𝓸𝔀𝓷).toBe("brown");

    re = /(?<\u{1d4d1}\u{1d4fb}\u{1d4f8}\u{1d500}\u{1d4f7}>brown)/u;
    expect(string.match(re).groups.𝓑𝓻𝓸𝔀𝓷).toBe("brown");
    expect(string.match(re).groups.𝓑𝓻𝓸𝔀𝓷).toBe("brown");

    re = /(?<\ud835\udcd1\ud835\udcfb\ud835\udcf8\ud835\udd00\ud835\udcf7>brown)/u;
    expect(string.match(re).groups.𝓑𝓻𝓸𝔀𝓷).toBe("brown");
    expect(string.match(re).groups.𝓑𝓻𝓸𝔀𝓷).toBe("brown");
});

test("global match with many empty matches", () => {
    // A pattern like /a*/ on a string of non-'a' characters produces N+1
    // empty matches for a string of length N. This exercises the find_all
    // buffer growth logic.
    var str = "b".repeat(200);
    var result = str.match(/a*/g);
    expect(result.length).toBe(201);
    for (var i = 0; i < result.length; i++) {
        expect(result[i]).toBe("");
    }
});

test("nested quantified captures keep the last non-empty iteration", () => {
    expect("xyz123xyz".match(/((123)|(xyz)*)*/)).toEqual(["xyz123xyz", "xyz", undefined, "xyz"]);
});

test("greedy lookbehinds backtrack to the correct boundary", () => {
    const string = "hey THIS does match!";

    expect(string.match(/(?<=h.*)THIS/)).toEqual(["THIS"]);
    expect(string.match(/(?<!.*q.*?)(?<=h.*)THIS(?=.*!)/g)).toEqual(["THIS"]);
});

test("redundant optional alternatives do not exceed the backtrack limit", () => {
    const string = "a".repeat(25) + "b";

    expect(string.match(/^(a|a?)+$/)).toBeNull();
});

test("required tail literals fail fast when they never appear", () => {
    const string = "a".repeat(25);

    expect(string.match(/(a+)+b/)).toBeNull();
});

test("sticky and global flag set", () => {
    const string = "aaba";
    expect(string.match(/a/)).toEqual(["a"]);
    expect(string.match(/a/y)).toEqual(["a"]);
    expect(string.match(/a/g)).toEqual(["a", "a", "a"]);
    expect(string.match(/a/gy)).toEqual(["a", "a"]);
});
