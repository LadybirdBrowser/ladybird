//
// Vendor-Prefix Helper Functions For Testing CSS
//

(function(root) {
'use strict';

var prefixCache = {};

// convert "foo-bar" to "fooBar"
function camelCase(str) {
    return str.replace(/\-(\w)/g, function(match, letter){
        return letter.toUpperCase();
    });
}

// vendor-prefix a css property
root.addVendorPrefix = function (name) {
    var prefix = getVendorPrefix(name);
    if (prefix === false) {
        // property unknown to browser
        return name;
    }

    return prefix + name;
};

// vendor-prefix a css property value
root.addValueVendorPrefix = function (property, value) {
    var prefix = getValueVendorPrefix(property, value);
    if (prefix === false) {
        // property unknown to browser
        return name;
    }

    return prefix + value;
};

// identify vendor-prefix for css property
root.getVendorPrefix = function(name) {
    if (prefixCache[name] !== undefined) {
        return prefixCache[name];
    }

    var elem = document.createElement("div");
    name = camelCase(name);

    if (name in elem.style) {
        return prefixCache[name] = "";
    }

    var prefixes = ["Webkit", "Moz", "O", "ms"];
    var styles = ["-webkit-", "-moz-", "-o-", "-ms-"];
    var _name = name.substring(0, 1).toUpperCase() + name.substring(1);

    for (var i = 0, length = prefixes.length; i < length; i++) {
        if (prefixes[i] + _name in elem.style) {
            return prefixCache[name] = styles[i];
        }
    }

    return prefixCache[name] = name in elem.style ? "" : false;
};

// identify vendor-prefix for css property value
root.getValueVendorPrefix = function(property, value) {
    var elem = document.createElement("div");
    // note: webkit needs the element to be attached to the dom
    document.body.appendChild(elem);
    var styles = ["-webkit-", "-moz-", "-o-", "-ms-", ""];
    var _property = getVendorPrefix(property) + property;
    for (var i=0, length = styles.length; i < length; i++) {
        var _value = styles[i] + value;
        elem.setAttribute('style', _property + ": " + _value);
        var _computed = computedStyle(elem, _property);
        if (_computed && _computed !== 'none') {
            document.body.removeChild(elem);
            return styles[i];
        }
    }
    document.body.removeChild(elem);
    return false;
};


})(window);
