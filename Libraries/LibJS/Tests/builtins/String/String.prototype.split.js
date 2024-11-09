test("basic functionality", () => {
    expect(String.prototype.split).toHaveLength(2);

    expect("hello friends".split()).toEqual(["hello friends"]);
    expect("hello friends".split("")).toEqual([
        "h",
        "e",
        "l",
        "l",
        "o",
        " ",
        "f",
        "r",
        "i",
        "e",
        "n",
        "d",
        "s",
    ]);
    expect("hello friends".split(" ")).toEqual(["hello", "friends"]);

    expect("a,b,c,d".split(",")).toEqual(["a", "b", "c", "d"]);
    expect(",a,b,c,d".split(",")).toEqual(["", "a", "b", "c", "d"]);
    expect("a,b,c,d,".split(",")).toEqual(["a", "b", "c", "d", ""]);
    expect("a,b,,c,d".split(",")).toEqual(["a", "b", "", "c", "d"]);
    expect(",a,b,,c,d,".split(",")).toEqual(["", "a", "b", "", "c", "d", ""]);
    expect(",a,b,,,c,d,".split(",,")).toEqual([",a,b", ",c,d,"]);
});

test("limits", () => {
    expect("a b c d".split(" ", 0)).toEqual([]);
    expect("a b c d".split(" ", 1)).toEqual(["a"]);
    expect("a b c d".split(" ", 3)).toEqual(["a", "b", "c"]);
    expect("a b c d".split(" ", 100)).toEqual(["a", "b", "c", "d"]);
});

test("regex split", () => {
    class RegExp1 extends RegExp {
        [Symbol.split](str, limit) {
            const result = RegExp.prototype[Symbol.split].call(this, str, limit);
            return result.map(x => `(${x})`);
        }
    }

    expect("2016-01-02".split(new RegExp1("-"))).toEqual(["(2016)", "(01)", "(02)"]);
    expect("2016-01-02".split(new RegExp("-"))).toEqual(["2016", "01", "02"]);

    expect(/a*?/[Symbol.split]("ab")).toEqual(["a", "b"]);
    expect(/a*/[Symbol.split]("ab")).toEqual(["", "b"]);

    let captureResult = /<(\/)?([^<>]+)>/[Symbol.split]("A<B>bold</B>and<CODE>coded</CODE>");
    expect(captureResult).toEqual([
        "A",
        undefined,
        "B",
        "bold",
        "/",
        "B",
        "and",
        undefined,
        "CODE",
        "coded",
        "/",
        "CODE",
        "",
    ]);
});

test("UTF-16", () => {
    var s = "😀";
    expect(s.split()).toEqual(["😀"]);
    expect(s.split("😀")).toEqual(["", ""]);
    expect(s.split("\ud83d")).toEqual(["", "\ude00"]);
    expect(s.split("\ude00")).toEqual(["\ud83d", ""]);

    expect(s.split(/\ud83d/)).toEqual(["", "\ude00"]);
    expect(s.split(/\ude00/)).toEqual(["\ud83d", ""]);

    s = "😀😀😀";
    expect(s.split(/\ud83d/)).toEqual(["", "\ude00", "\ude00", "\ude00"]);
    expect(s.split(/\ude00/)).toEqual(["\ud83d", "\ud83d", "\ud83d", ""]);
});
