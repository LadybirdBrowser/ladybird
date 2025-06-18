import {
    testElement,
    writingModes,
    testCSSValues,
    testComputedValues,
    makeDeclaration
} from "./test-shared.js";

// Values to use while testing
const testValues = {
    "length": ["1px", "2px", "3px", "4px", "5px"],
    "color": ["rgb(1, 1, 1)", "rgb(2, 2, 2)", "rgb(3, 3, 3)", "rgb(4, 4, 4)", "rgb(5, 5, 5)"],
    "border-style": ["solid", "dashed", "dotted", "double", "groove"],
};

/**
 * Creates a group of physical and logical box properties, such as
 *
 * { physical: {
 *     left: "margin-left", right: "margin-right",
 *     top: "margin-top", bottom: "margin-bottom",
 *   }, logical: {
 *     inlineStart: "margin-inline-start", inlineEnd: "margin-inline-end",
 *     blockStart: "margin-block-start", blockEnd: "margin-block-end",
 *   }, shorthands: {
 *     "margin": ["margin-top", "margin-right", "margin-bottom", "margin-left"],
 *     "margin-inline": ["margin-inline-start", "margin-inline-end"],
 *     "margin-block": ["margin-block-start", "margin-block-end"],
 *   }, type: ["length"], prerequisites: "...", property: "margin-*" }
 *
 * @param {string} property
 *        A string representing the property names, like "margin-*".
 * @param {Object} descriptor
 * @param {string|string[]} descriptor.type
 *        Describes the kind of values accepted by the property, like "length".
 *        Must be a key or a collection of keys from the `testValues` object.
 * @param {Object={}} descriptor.prerequisites
 *        Represents property declarations that are needed by `property` to work.
 *        For example, border-width properties require a border style.
 */
export function createBoxPropertyGroup(property, descriptor) {
    const logical = {};
    const physical = {};
    const shorthands = {};
    for (const axis of ["inline", "block"]) {
        const shorthand = property.replace("*", axis);
        const longhands = [];
        shorthands[shorthand] = longhands;
        for (const side of ["start", "end"]) {
            const logicalSide = axis + "-" + side;
            const camelCase = logicalSide.replace(/-(.)/g, (match, $1) => $1.toUpperCase());
            const longhand = property.replace("*", logicalSide);
            logical[camelCase] = longhand;
            longhands.push(longhand);
        }
    }
    const isInset = property === "inset-*";
    let prerequisites = "";
    for (const physicalSide of ["left", "right", "top", "bottom"]) {
        physical[physicalSide] = isInset ? physicalSide : property.replace("*", physicalSide);
        prerequisites += makeDeclaration(descriptor.prerequisites, physicalSide);
    }
    shorthands[property.replace("-*", "")] =
        ["top", "right", "bottom", "left"].map(physicalSide => physical[physicalSide]);
    const type = [].concat(descriptor.type);
    return { logical, physical, shorthands, type, prerequisites, property };
}

/**
 * Creates a group physical and logical box-corner properties.
 *
 * @param {string} property
 *        A string representing the property names, like "border-*-radius".
 * @param {Object} descriptor
 * @param {string|string[]} descriptor.type
 *        Describes the kind of values accepted by the property, like "length".
 *        Must be a key or a collection of keys from the `testValues` object.
 * @param {Object={}} descriptor.prerequisites
 *        Represents property declarations that are needed by `property` to work.
 *        For example, border-width properties require a border style.
 */
export function createCornerPropertyGroup(property, descriptor) {
    const logical = {};
    const physical = {};
    const shorthands = {};
    for (const logicalCorner of ["start-start", "start-end", "end-start", "end-end"]) {
        const prop = property.replace("*", logicalCorner);
        const [block_side, inline_side] = logicalCorner.split("-");
        const b = "block" + block_side.charAt(0).toUpperCase() + block_side.slice(1);
        const i = "inline" + inline_side.charAt(0).toUpperCase() + inline_side.slice(1);
        const index = b + "-" + i; // e.g. "blockStart-inlineEnd"
        logical[index] = prop;
    }
    let prerequisites = "";
    for (const physicalCorner of ["top-left", "top-right", "bottom-left", "bottom-right"]) {
        const prop = property.replace("*", physicalCorner);
        physical[physicalCorner] = prop;
        prerequisites += makeDeclaration(descriptor.prerequisites, physicalCorner);
    }
    const type = [].concat(descriptor.type);
    return { logical, physical, shorthands, type, prerequisites, property };
}

/**
 * Creates a group of physical and logical sizing properties.
 *
 * @param {string} prefix
 *        One of "", "max-" or "min-".
 */
export function createSizingPropertyGroup(prefix) {
    return {
        logical: {
            inline: `${prefix}inline-size`,
            block: `${prefix}block-size`,
        },
        physical: {
            horizontal: `${prefix}width`,
            vertical: `${prefix}height`,
        },
        type: ["length"],
        prerequisites: makeDeclaration({ display: "block" }),
        property: (prefix ? prefix.slice(0, -1) + " " : "") + "sizing",
    };
}

/**
 * Tests a grup of logical and physical properties in different writing modes.
 *
 * @param {Object} group
 *        An object returned by createBoxPropertyGroup or createSizingPropertyGroup.
 */
export function runTests(group) {
    const values = testValues[group.type[0]].map(function (_, i) {
        return group.type.map(type => testValues[type][i]).join(" ");
    });
    const logicals = Object.values(group.logical);
    const physicals = Object.values(group.physical);
    const shorthands = group.shorthands ? Object.entries(group.shorthands) : null;
    const is_corner = group.property == "border-*-radius";

    test(function () {
        const expected = [];
        for (const [i, logicalProp] of logicals.entries()) {
            testElement.style.setProperty(logicalProp, values[i]);
            expected.push([logicalProp, values[i]]);
        }
        testCSSValues("logical properties in inline style", testElement.style, expected);
    }, `Test that logical ${group.property} properties are supported.`);
    testElement.style.cssText = "";

    const shorthandValues = {};
    for (const [shorthand, longhands] of shorthands || []) {
        let valueArray;
        if (group.type.length > 1) {
            valueArray = [values[0]];
        } else {
            valueArray = testValues[group.type].slice(0, longhands.length);
        }
        shorthandValues[shorthand] = valueArray;
        const value = valueArray.join(" ");
        const expected = [[shorthand, value]];
        for (let [i, longhand] of longhands.entries()) {
            expected.push([longhand, valueArray[group.type.length > 1 ? 0 : i]]);
        }
        test(function () {
            testElement.style.setProperty(shorthand, value);
            testCSSValues("shorthand in inline style", testElement.style, expected);
            const stylesheet = `.test { ${group.prerequisites} }`;
            testComputedValues("shorthand in computed style", stylesheet, expected);
        }, `Test that ${shorthand} shorthand sets longhands and serializes correctly.`);
        testElement.style.cssText = "";
    }

    for (const writingMode of writingModes) {
        for (const style of writingMode.styles) {
            const writingModeDecl = makeDeclaration(style);

            const associated = {};
            for (const [logicalSide, logicalProp] of Object.entries(group.logical)) {
                let physicalProp;
                if (is_corner) {
                    const [block_side, inline_side] = logicalSide.split("-");
                    const physicalSide1 = writingMode[block_side];
                    const physicalSide2 = writingMode[inline_side];
                    let physicalCorner;
                    // mirror "left-top" to "top-left" etc
                    if (["top", "bottom"].includes(physicalSide1)) {
                        physicalCorner = physicalSide1 + "-" + physicalSide2;
                    } else {
                        physicalCorner = physicalSide2 + "-" + physicalSide1;
                    }
                    physicalProp = group.physical[physicalCorner];
                } else {
                    physicalProp = group.physical[writingMode[logicalSide]];
                }
                associated[logicalProp] = physicalProp;
                associated[physicalProp] = logicalProp;
            }

            // Test that logical properties are converted to their physical
            // equivalent correctly when all in the group are present on a single
            // declaration, with no overwriting of previous properties and
            // no physical properties present.  We put the writing mode properties
            // on a separate declaration to test that the computed values of these
            // properties are used, rather than those on the same declaration.
            test(function () {
                let decl = group.prerequisites;
                const expected = [];
                for (const [i, logicalProp] of logicals.entries()) {
                    decl += `${logicalProp}: ${values[i]}; `;
                    expected.push([logicalProp, values[i]]);
                    expected.push([associated[logicalProp], values[i]]);
                }
                testComputedValues("logical properties on one declaration, writing " +
                    `mode properties on another, '${writingModeDecl}'`,
                    `.test { ${writingModeDecl} } .test { ${decl} }`,
                    expected);
            }, `Test that logical ${group.property} properties share computed values `
            + `with their physical associates, with '${writingModeDecl}'.`);

            // Test logical shorthand properties.
            if (shorthands) {
                test(function () {
                    for (const [shorthand, longhands] of shorthands) {
                        let valueArray = shorthandValues[shorthand];
                        const decl = group.prerequisites + `${shorthand}: ${valueArray.join(" ")}; `;
                        const expected = [];
                        for (let [i, longhand] of longhands.entries()) {
                            const longhandValue = valueArray[group.type.length > 1 ? 0 : i];
                            expected.push([longhand, longhandValue]);
                            expected.push([associated[longhand], longhandValue]);
                        }
                        testComputedValues("shorthand properties on one declaration, writing " +
                            `mode properties on another, '${writingModeDecl}'`,
                            `.test { ${writingModeDecl} } .test { ${decl} }`,
                            expected);
                    }
                }, `Test that ${group.property} shorthands set the computed value of both `
                + `logical and physical longhands, with '${writingModeDecl}'.`);
            }

            // Test that logical and physical properties are cascaded together,
            // honoring their relative order on a single declaration
            // (a) with a single logical property after the physical ones
            // (b) with a single physical property after the logical ones
            test(function () {
                for (const lastIsLogical of [true, false]) {
                    const lasts = lastIsLogical ? logicals : physicals;
                    const others = lastIsLogical ? physicals : logicals;
                    for (const lastProp of lasts) {
                        let decl = writingModeDecl + group.prerequisites;
                        const expected = [];
                        for (const [i, prop] of others.entries()) {
                            decl += `${prop}: ${values[i]}; `;
                            const valueIdx = associated[prop] === lastProp ? others.length : i;
                            expected.push([prop, values[valueIdx]]);
                            expected.push([associated[prop], values[valueIdx]]);
                        }
                        decl += `${lastProp}: ${values[others.length]}; `;
                        testComputedValues(`'${lastProp}' last on single declaration, '${writingModeDecl}'`,
                            `.test { ${decl} }`,
                            expected);
                    }
                }
            }, `Test that ${group.property} properties honor order of appearance when both `
            + `logical and physical associates are declared, with '${writingModeDecl}'.`);

            // Test that logical and physical properties are cascaded properly when
            // on different declarations
            // (a) with a logical property in the high specificity rule
            // (b) with a physical property in the high specificity rule
            test(function () {
                for (const highIsLogical of [true, false]) {
                    let lowDecl = writingModeDecl + group.prerequisites;
                    const high = highIsLogical ? logicals : physicals;
                    const others = highIsLogical ? physicals : logicals;
                    for (const [i, prop] of others.entries()) {
                        lowDecl += `${prop}: ${values[i]}; `;
                    }
                    for (const highProp of high) {
                        const highDecl = `${highProp}: ${values[others.length]}; `;
                        const expected = [];
                        for (const [i, prop] of others.entries()) {
                            const valueIdx = associated[prop] === highProp ? others.length : i;
                            expected.push([prop, values[valueIdx]]);
                            expected.push([associated[prop], values[valueIdx]]);
                        }
                        testComputedValues(`'${highProp}', two declarations, '${writingModeDecl}'`,
                            `#test { ${highDecl} } .test { ${lowDecl} }`,
                            expected);
                    }
                }
            }, `Test that ${group.property} properties honor selector specificty when both `
            + `logical and physical associates are declared, with '${writingModeDecl}'.`);
        }
    }
}
