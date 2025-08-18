(() => {
    globalThis.handleSDLInputEvents = () => {
        internals.handleSDLInputEvents();
        return new Promise(resolve => {
            setTimeout(() => resolve(), 0);
        });
    };

    globalThis.getStringifiedGamepads = () => {
        const gamepads = navigator.getGamepads().map(gamepad => ({
            id: gamepad.id,
            index: gamepad.index,
            connected: gamepad.connected,
            mapping: gamepad.mapping,
            axes: gamepad.axes,
            buttons: gamepad.buttons.map(button => ({
                pressed: button.pressed,
                touched: button.touched,
                value: button.value,
            })),
            vibrationActuator: {
                effects: gamepad.vibrationActuator.effects,
            },
        }));

        return JSON.stringify(gamepads);
    };

    globalThis.listenForGamepadConnected = () => {
        window.addEventListener("gamepadconnected", ({ gamepad }) => {
            println(
                `Received gamepadconnected event for '${gamepad.id}' at index ${gamepad.index}`
            );
        });
    };

    globalThis.listenForGamepadDisconnected = () => {
        window.addEventListener("gamepaddisconnected", ({ gamepad }) => {
            println(
                `Received gamepaddisconnected event for '${gamepad.id}' at index ${gamepad.index}`
            );
        });
    };
})();
