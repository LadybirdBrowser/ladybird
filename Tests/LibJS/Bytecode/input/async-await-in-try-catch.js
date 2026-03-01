async function f(a) {
    try {
        with (a) {
            await 1;
        }
    } catch (e) {
    }
}
f({});
