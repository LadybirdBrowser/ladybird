export function withDryWetMix(processor, log, ui) {
    const { clampNumber, createRangeNumberRow, safeConnect, safeDisconnect } = ui;

    let rememberedMix = 0.5;
    let input = null;
    let dryGain = null;
    let wetGain = null;
    let output = null;

    const mixRow = createRangeNumberRow({
        label: "Mix",
        range: { min: "0", max: "100", value: "50", step: "1" },
        number: { min: "0", max: "100", value: "50", step: "1" },
        onChange: () => {
            const value = Number(mixRow.numberEl.value);
            rememberedMix = Number.isFinite(value) ? clampNumber(value / 100, 0, 1) : 0.5;
            applyMix();
        },
    });

    processor.uiRoot.insertBefore(mixRow.row, processor.uiRoot.children[1] || null);

    const innerEnsureAudio = processor.ensureAudio ? processor.ensureAudio.bind(processor) : null;
    const innerSaveState = processor.saveState ? processor.saveState.bind(processor) : null;
    const innerApplyState = processor.applyState ? processor.applyState.bind(processor) : null;
    const innerDisconnectAll = processor.disconnectAll ? processor.disconnectAll.bind(processor) : null;
    const innerTeardownAudio = processor.teardownAudio ? processor.teardownAudio.bind(processor) : null;
    const innerSetContext = processor.setContext ? processor.setContext.bind(processor) : null;
    const innerAudioNode = processor.audioNode ? processor.audioNode.bind(processor) : null;
    const innerOutputNode = processor.outputNode ? processor.outputNode.bind(processor) : null;

    function isBypassed() {
        if (!innerSaveState) return false;
        try {
            const state = innerSaveState();
            return !!(state && state.bypass);
        } catch (_) {
            return false;
        }
    }

    function updateMixUi() {
        const disabled = isBypassed();
        mixRow.rangeEl.disabled = disabled;
        mixRow.numberEl.disabled = disabled;
    }

    function setGain(node, value, label) {
        try {
            node.gain.value = value;
        } catch (e) {
            log(`${processor.kind}.mix ${label} gain set failed: ${e}`);
        }
    }

    function applyMix() {
        updateMixUi();

        if (!dryGain || !wetGain) return;

        if (isBypassed()) {
            setGain(dryGain, 1, "dry");
            setGain(wetGain, 0, "wet");
            return;
        }

        setGain(dryGain, 1 - rememberedMix, "dry");
        setGain(wetGain, rememberedMix, "wet");
    }

    function wireMixerGraph() {
        if (!input || !dryGain || !wetGain || !output) return;

        let wetInput = null;
        let wetOutput = null;
        try {
            if (!innerAudioNode || !innerOutputNode) throw new Error("processor endpoints missing");
            wetInput = innerAudioNode();
            wetOutput = innerOutputNode();
        } catch (e) {
            log(`${processor.kind}.mix node access failed: ${e}`);
            return;
        }

        safeDisconnect(input);
        safeDisconnect(dryGain);
        safeDisconnect(wetGain);
        safeDisconnect(wetOutput);

        safeConnect(input, dryGain);
        safeConnect(dryGain, output);
        safeConnect(input, wetInput);
        safeConnect(wetOutput, wetGain);
        safeConnect(wetGain, output);

        applyMix();
    }

    const syncMixFromProcessorState = event => {
        if (event.target === mixRow.rangeEl || event.target === mixRow.numberEl) return;
        applyMix();
    };

    processor.uiRoot.addEventListener("input", syncMixFromProcessorState);
    processor.uiRoot.addEventListener("change", syncMixFromProcessorState);

    processor.setContext = newCtx => {
        if (innerSetContext) innerSetContext(newCtx);
    };

    processor.ensureAudio = async context => {
        if (innerEnsureAudio) await innerEnsureAudio(context);

        if (!input) {
            input = context.createGain();
            dryGain = context.createGain();
            wetGain = context.createGain();
            output = context.createGain();
        }

        wireMixerGraph();
    };

    processor.saveState = () => {
        const state = innerSaveState?.();
        const saved = state && typeof state === "object" ? { ...state } : {};
        saved.mix = rememberedMix;
        return saved;
    };

    processor.applyState = state => {
        if (innerApplyState) innerApplyState(state);

        if (state && state.mix !== undefined) {
            const rawMix = Number(state.mix);
            rememberedMix = Number.isFinite(rawMix)
                ? clampNumber(rawMix > 1 ? rawMix / 100 : rawMix, 0, 1)
                : rememberedMix;
        }

        const mixPercent = Math.round(rememberedMix * 100);
        mixRow.rangeEl.value = String(mixPercent);
        mixRow.numberEl.value = String(mixPercent);
        applyMix();
    };

    processor.disconnectAll = () => {
        safeDisconnect(input);
        safeDisconnect(dryGain);
        safeDisconnect(wetGain);
        safeDisconnect(output);
        if (innerDisconnectAll) innerDisconnectAll();
    };

    processor.audioNode = () => {
        if (!input) throw new Error(`${processor.title} mix input missing`);
        return input;
    };

    processor.outputNode = () => {
        if (!output) throw new Error(`${processor.title} mix output missing`);
        return output;
    };

    processor.teardownAudio = () => {
        safeDisconnect(input);
        safeDisconnect(dryGain);
        safeDisconnect(wetGain);
        safeDisconnect(output);
        input = null;
        dryGain = null;
        wetGain = null;
        output = null;
        if (innerTeardownAudio) innerTeardownAudio();
    };

    const innerTeardown = processor.teardown?.bind(processor);
    processor.teardown = () => {
        processor.uiRoot.removeEventListener("input", syncMixFromProcessorState);
        processor.uiRoot.removeEventListener("change", syncMixFromProcessorState);
        if (innerTeardown) innerTeardown();
        else processor.teardownAudio();
    };

    return processor;
}
