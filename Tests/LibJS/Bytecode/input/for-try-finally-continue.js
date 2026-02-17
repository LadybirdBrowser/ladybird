for (var i = 0; i < 5; i++) {
    try {
        continue;
    } catch (e) {
    } finally {
    }
}
if (i !== 5) {
    throw "bad";
}
