// Unwinding a deep recursion that catches the stack overflow exits compiled code in every frame, at an addition that
// never ran before. Each exit looked for the frames its code remembered among all live frames, one stack walk per
// remembered frame, so measuring the stack depth like this took minutes.

test("measuring the stack depth with a recursion that catches the stack overflow finishes", () => {
    function depth(warm) {
        if (warm) return 0;
        try {
            return 1 + depth(false);
        } catch {
            return 1;
        }
    }
    jit.prepare(depth);
    depth(true);
    expect(jit.compile(depth)).toBe(jit.enabled);
    expect(depth(false)).toBeGreaterThan(1000);
});
