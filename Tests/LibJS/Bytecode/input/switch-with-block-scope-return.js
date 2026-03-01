function f(x) {
    switch (x) {
        case 1:
            return "one";
        case 2:
            const y = "two";
            return (() => y)();
        default:
            return "other";
    }
}
f(1);
