await 0;

function errorOf(callback) {
    try {
        callback();
        return "none";
    } catch (error) {
        return error.name;
    }
}

export const errors = [
    errorOf(() => eval("new.target")),
    errorOf(() => eval("super.x")),
    errorOf(() => eval("super()")),
];
try {
    eval("new.target");
    errors.push("none");
} catch (error) {
    errors.push(error.name);
}

export const thisValues = [eval("this"), (() => eval("this"))()];

export const passed = true;
