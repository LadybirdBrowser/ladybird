// A slow path call whose output register also holds one of its inputs must pass the slow path that input, not another
// one: `p >>> v1` with `p` an object must not call the slow path with `p` for both operands, which made the loop take
// the wrong branch on its first iteration.

test("slow path calls pass inputs that share the output register", () => {
    const out = [];
    class Shape0 {}
    function f0(p) {
        let v0 = new Shape0();
        let v1 = ((p !== p) | v0) >> (p * p == "x" in Object(v0));
        for (let i = 0; i < 3; ++i) {
            if ((p >>> v1) * ((v1 ^ 0) <= p - -2147483648)) {
                out.push("t" + i);
            } else {
                out.push(arguments.length);
                p *= (v1 - i != typeof v0) / (p | 0);
            }
        }
    }
    jit.prepare(f0);
    for (let i = 0; i < 5; ++i) f0(new Shape0());
    jit.compile(f0);
    out.length = 0;
    f0({
        valueOf() {
            return 4;
        },
    });
    expect(out).toEqual(["t0", "t1", "t2"]);
});
