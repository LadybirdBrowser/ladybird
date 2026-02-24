export function createOscillatorSource(log, ui) {
    const { h, createSelectRow, createRangeNumberRow, parseNumber, safeDisconnect } = ui;

    let ctx = null;
    let osc = null;

    const uiRoot = h("div", null);

    const typeRow = createSelectRow({
        label: "Type",
        optionsList: [
            { value: "sine", text: "sine", selected: true },
            { value: "square", text: "square" },
            { value: "sawtooth", text: "sawtooth" },
            { value: "triangle", text: "triangle" },
        ],
        onChange: v => {
            if (!osc) return;
            try {
                osc.type = v;
            } catch (e) {
                log(`osc.type set failed: ${e}`);
            }
        },
    });

    const freqRow = createRangeNumberRow({
        label: "Frequency (Hz)",
        range: { min: "20", max: "2000", value: "440", step: "1" },
        number: { min: "0", max: "24000", value: "440", step: "1" },
        onChange: () => {
            if (!osc) return;
            const f = parseNumber(freqRow.numberEl, 440);
            try {
                osc.frequency.value = f;
            } catch (e) {
                log(`osc.frequency.value set failed: ${e}`);
            }
        },
    });

    const detuneRow = createRangeNumberRow({
        label: "Detune (cents)",
        range: { min: "-1200", max: "1200", value: "0", step: "1" },
        number: { min: "-12000", max: "12000", value: "0", step: "1" },
        onChange: () => {
            if (!osc) return;
            const d = parseNumber(detuneRow.numberEl, 0);
            try {
                osc.detune.value = d;
            } catch (e) {
                log(`osc.detune.value set failed: ${e}`);
            }
        },
    });

    uiRoot.append(typeRow.row);
    uiRoot.append(freqRow.row);
    uiRoot.append(detuneRow.row);

    function ensureAudio(context) {
        if (osc) return;

        ctx = context;
        osc = ctx.createOscillator();

        // Start immediately. Note: output amplitude is not attenuated here;
        // add a GainNode explicitly if you want level control.
        try {
            osc.start();
        } catch (e) {
            log(`osc.start failed: ${e}`);
        }

        // Apply initial params.
        try {
            osc.type = typeRow.select.value;
        } catch (_) {}
        try {
            osc.frequency.value = parseNumber(freqRow.numberEl, 440);
        } catch (_) {}
        try {
            osc.detune.value = parseNumber(detuneRow.numberEl, 0);
        } catch (_) {}
    }

    return {
        kind: "oscillator",
        title: "OscillatorNode",
        uiRoot,
        setContext(newCtx) {
            ctx = newCtx;
        },
        ensureAudio,
        serializeState() {
            return {
                type: typeRow.select.value,
                frequency: freqRow.numberEl.value,
                detune: detuneRow.numberEl.value,
            };
        },
        applyState(state) {
            if (!state) return;
            if (state.type) typeRow.select.value = state.type;
            if (state.frequency !== undefined) freqRow.numberEl.value = String(state.frequency);
            if (state.detune !== undefined) detuneRow.numberEl.value = String(state.detune);
            typeRow.select.dispatchEvent(new Event("change"));
            freqRow.numberEl.dispatchEvent(new Event("input"));
            detuneRow.numberEl.dispatchEvent(new Event("input"));
        },
        disconnectAll() {
            if (osc) safeDisconnect(osc);
        },
        outputNode() {
            return osc;
        },
        teardownAudio() {
            if (osc) {
                try {
                    osc.stop();
                } catch (_) {}
            }
            safeDisconnect(osc);
            osc = null;
        },
        teardown() {
            this.teardownAudio();
        },
    };
}
