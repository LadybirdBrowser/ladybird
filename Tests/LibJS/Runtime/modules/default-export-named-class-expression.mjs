export default (class MyClass {
    valueOf() {
        return 45;
    }
});
import C from "./default-export-named-class-expression.mjs";

export const passed = new C().valueOf() === 45 && C.name === "MyClass";
