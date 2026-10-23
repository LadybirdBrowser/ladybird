async function fetchOperatorDictionary() {
    let response = await fetch(`/mathml/support/operator-dictionary.json`);
    return response.json();
}

function splitKey(key) {
    var value = key.split(" ")
    return {
        characters: value[0],
        form: value[1]
    };
}

function spaceIndexToLength(index) {
    // See https://w3c.github.io/mathml-core/#operator-dictionary
    return ["0",
            "0.05555555555555555em",
            "0.1111111111111111em",
            "0.16666666666666666em",
            "0.2222222222222222em",
            "0.2777777777777778em",
            "0.3333333333333333em",
            "0.3888888888888889em"
           ][index];
}

function defaultPropertyValue(entry, name) {
    switch (name) {
    case "lspace":
    case "rspace":
        return spaceIndexToLength(entry.hasOwnProperty(name) ? entry[name] : 5);
    break
    case "largeop":
    case "movablelimits":
    case "stretchy":
    case "symmetric":
    case "accent":
        return entry[name];
    default:
        throw `Unknown property ${name}`;
    }
}
