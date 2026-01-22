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

test("sticky and global flag set", () => {
    const string = "aaba";
    expect(string.match(/a/)).toEqual(["a"]);
    expect(string.match(/a/y)).toEqual(["a"]);
    expect(string.match(/a/g)).toEqual(["a", "a", "a"]);
    expect(string.match(/a/gy)).toEqual(["a", "a"]);
});
