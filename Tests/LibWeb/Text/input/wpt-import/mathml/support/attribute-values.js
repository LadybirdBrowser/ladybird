AttributeValueTransforms = {
    lowercase: function(value) { return value.toLowerCase(); },
    uppercase: function(value) { return value.toUpperCase(); },
    alternate_case: function(value) {
        var transformedValue = "";
        for (var i = 0; i < value.length; i++) {
            transformedValue += i % 2 ?
                value.charAt(i).toLowerCase() :
                value.charAt(i).toUpperCase();
        }
        return transformedValue;
    },
    // TODO: Should we perform this transform too?
    // https://github.com/mathml-refresh/mathml/issues/122
    // add_leading_and_trimming_whitespace: function(value) {
    //    var space = "\0020\0009\000A\000D";
    //    return `${space}${space}${value}${space}${space}`;
    // },
};

function TransformAttributeValues(transform, attributeNames) {
    if (typeof attributeNames === "string")
        attributeNames = [attributeNames];
    attributeNames.forEach(name => {
        Array.from(document.querySelectorAll(`[${name}]`)).forEach(element => {
            var value = element.getAttribute(name);
            var transformedValue = AttributeValueTransforms[transform](value);
            element.setAttribute(name, transformedValue);
        });
    });
}
