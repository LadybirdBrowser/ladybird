// await as identifier reference inside nested function in static init
var await = 0;
var result;

class C {
    static {
        (function () {
            result = await;
        })();
    }
}

// arguments inside nested function in static init
class D {
    static {
        (function () {
            return arguments;
        })();
    }
}
