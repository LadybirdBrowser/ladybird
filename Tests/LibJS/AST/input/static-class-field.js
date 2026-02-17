// "static" is not a keyword; it's a contextual identifier
var C = class {
    static x = 1;
    static y = 2;
};

// Static init blocks
var D = class {
    static {
        var x = 1;
    }
};
