window.test_driver_internal.click = function(element) {
    const boundingRect = element.getBoundingClientRect();
    const centerPoint = {
        x: boundingRect.left + boundingRect.width / 2,
        y: boundingRect.top + boundingRect.height / 2
    };
    window.internals.click(centerPoint.x, centerPoint.y);
    return Promise.resolve();
};

window.test_driver_internal.send_keys = function(element, keys) {
    window.internals.sendText(element, keys);
    return Promise.resolve();
}

window.test_driver_internal.get_computed_label = async function(element) {
    return await window.internals.getComputedLabel(element);
};

window.test_driver_internal.get_computed_role = async function(element) {
    return await window.internals.getComputedRole(element);
};

window.test_driver_internal.action_sequence = function(actions, context) {
    // Modifier key codes from WebDriver spec
    const SHIFT = "\uE008";
    const CTRL = "\uE009";
    const ALT = "\uE00A";
    const META = "\uE03D";

    // Modifier flags matching Internals.idl
    const MOD_SHIFT = 4;
    const MOD_CTRL = 2;
    const MOD_ALT = 1;
    const MOD_SUPER = 8;

    let modifiers = 0;
    const target = document.activeElement;

    // Collect all key actions with their tick index
    const tickActions = [];
    for (const source of actions) {
        if (source.type !== "key" || !source.actions) {
            continue;
        }
        for (let tick = 0; tick < source.actions.length; tick++) {
            const action = source.actions[tick];
            if (action.type !== "pause") {
                tickActions.push({ tick, action });
            }
        }
    }

    // Sort by tick index and process
    tickActions.sort((a, b) => a.tick - b.tick);

    for (const { action } of tickActions) {
        const key = action.value;
        const isKeyDown = action.type === "keyDown";

        // Update modifier state
        if (key === SHIFT) {
            modifiers = isKeyDown ? (modifiers | MOD_SHIFT) : (modifiers & ~MOD_SHIFT);
        } else if (key === CTRL) {
            modifiers = isKeyDown ? (modifiers | MOD_CTRL) : (modifiers & ~MOD_CTRL);
        } else if (key === ALT) {
            modifiers = isKeyDown ? (modifiers | MOD_ALT) : (modifiers & ~MOD_ALT);
        } else if (key === META) {
            modifiers = isKeyDown ? (modifiers | MOD_SUPER) : (modifiers & ~MOD_SUPER);
        } else if (isKeyDown) {
            // For non-modifier keys, only send on keyDown
            window.internals.sendText(target, key, modifiers);
        }
    }

    return Promise.resolve();
};
