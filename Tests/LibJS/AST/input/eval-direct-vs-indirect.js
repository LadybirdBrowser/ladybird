function direct() {
    eval("1");
}
function indirect() {
    (0, eval)("1");
}
