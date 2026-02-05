'use strict';

function test_shorthand_value(property, value, longhands) {
    const stringifiedValue = JSON.stringify(value);

    for (let longhand of Object.keys(longhands).sort()) {
        test(function(){
            var div = document.getElementById('target') || document.createElement('div');
            div.style[property] = "";
            try {
                div.style[property] = value;

                const readValue = div.style[longhand];
                assert_equals(readValue, longhands[longhand], longhand + " should be canonical");

                div.style[longhand] = "";
                div.style[longhand] = readValue;
                assert_equals(div.style[longhand], readValue, "serialization should round-trip");
            } finally {
                div.style[property] = "";
            }
        }, "e.style['" + property + "'] = " + stringifiedValue + " should set " + longhand);
    }

    test(function(){
        var div = document.getElementById('target') || document.createElement('div');
        div.style[property] = "";
        try {
            const expectedLength = div.style.length;
            div.style[property] = value;
            assert_true(CSS.supports(property, value));
            for (let longhand of Object.keys(longhands).sort()) {
                div.style[longhand] = "";
            }
            assert_equals(div.style.length, expectedLength);
        } finally {
            div.style[property] = "";
        }
    }, "e.style['" + property + "'] = " + stringifiedValue + " should not set unrelated longhands");
}

/**
 * Helper to be called from inside test().
 */
function is_property_in_longhands(t, property_name) {
    let e = document.createElement("div");
    document.body.append(e);
    t.add_cleanup(() => e.remove());
    let cs = getComputedStyle(e);
    return Array.from(cs).includes(property_name);
}

/**
 * This function is designed mainly to test the distinction between
 * legacy name aliases and legacy shorthands.
 */
function test_is_legacy_name_alias(old_name, new_name) {
    test(t => {
        let e = document.createElement("div");
        e.style.setProperty(old_name, "inherit");
        assert_equals(e.style.getPropertyValue(old_name), "inherit",
                      `${old_name} is supported`);
        assert_equals(e.style.getPropertyValue(new_name), "inherit",
                      `${old_name} is an alias for ${new_name}`);
        assert_equals(e.style.cssText, `${new_name}: inherit;`,
                      `declarations serialize using new name ${new_name}`);

        e = document.createElement("div");
        e.style.setProperty(old_name, "var(--v)");
        assert_equals(e.style.getPropertyValue(new_name), "var(--v)",
                      `${old_name} is a legacy name alias rather than a shorthand`)

        e = document.createElement("div");
        e.style.setProperty(new_name, "var(--w)");
        assert_equals(e.style.getPropertyValue(old_name), "var(--w)",
                      `${old_name} is a legacy name alias rather than a shorthand`)

        assert_false(is_property_in_longhands(t, old_name),
                     `${old_name} is not in getComputedStyle() list of longhands`);
    }, `${old_name} is a legacy name alias for ${new_name}`);
}

/**
 * This function is designed mainly to test the distinction between
 * legacy name aliases and legacy shorthands.
 */
function test_is_legacy_shorthand(old_name, new_name) {
    test(t => {
        let e = document.createElement("div");
        e.style.setProperty(old_name, "inherit");
        assert_equals(e.style.getPropertyValue(old_name), "inherit",
                      `${old_name} is supported`);
        assert_equals(e.style.getPropertyValue(new_name), "inherit",
                      `${old_name} is an alias for ${new_name}`);
        assert_equals(e.style.cssText, `${new_name}: inherit;`,
                      `declarations serialize using new name ${new_name}`);

        e = document.createElement("div");
        e.style.setProperty(old_name, "var(--v)");
        assert_equals(e.style.getPropertyValue(new_name), "",
                      `${old_name} is a shorthand rather than a legacy name alias`)

        assert_false(is_property_in_longhands(t, old_name),
                     `${old_name} is not in getComputedStyle() list of longhands`);
    }, `${old_name} is a legacy name alias for ${new_name}`);
}
