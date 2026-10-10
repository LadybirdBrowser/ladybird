// The JIT's graph builder panicked while inlining loop() into outer(): the inlined method reads `this` for the first
// time inside a loop, so the slot had no value yet where the loop header merged it.
const o = {
    n: 2,
    loop() {
        for (var i = 0; i < this.n; i++) {}
        return this;
    },
};
function outer(o) {
    return o.loop();
}
for (let k = 0; k < 100; k++) outer(o);
print("ok");
