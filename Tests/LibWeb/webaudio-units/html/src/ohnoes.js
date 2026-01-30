export function createOhNoesProcessor(log, ui) {
    const { h, createCheckboxRow, safeDisconnect } = ui;

    let ctx = null;
    let node = null;

    const uiRoot = h("div", null);

    const pathRow = h("div", "row");
    pathRow.append(h("label", null, "Base path"));
    const pathInput = document.createElement("input");
    pathInput.type = "text";
    pathInput.placeholder = "/tmp/ladybird-audio";
    pathInput.value = "";
    pathRow.append(pathInput);
    pathRow.append(h("div", "hint-inline", ""));

    const dumpRow = createCheckboxRow({
        label: "Dump wav",
        checked: false,
        onChange: enabled => {
            if (!node) return;
            try {
                if (enabled) node.start();
                else node.stop();
            } catch (e) {
                log(`OhNoes start/stop failed: ${e}`);
            }
            pathInput.disabled = enabled;
        },
        hint: "",
    });

    const stripRow = createCheckboxRow({
        label: "Strip zeros",
        checked: false,
        onChange: enabled => {
            if (!node) return;
            try {
                node.setStripZeroBuffers(enabled);
            } catch (e) {
                log(`OhNoes setStripZeroBuffers failed: ${e}`);
            }
        },
        hint: "",
    });

    pathInput.addEventListener("input", () => {
        if (!node) return;
        try {
            node.setPath(pathInput.value);
        } catch (e) {
            log(`OhNoes setPath failed: ${e}`);
        }
    });

    uiRoot.append(pathRow);
    uiRoot.append(dumpRow.row);
    uiRoot.append(stripRow.row);

    function ensureAudio(context) {
        if (node) return;
        ctx = context;

        try {
            node = window.internals.createOhNoesNode(ctx, pathInput.value || "");
        } catch (e) {
            node = null;
            log(`createOhNoesNode failed: ${e}`);
            return;
        }

        // OhNoesNode defaults to "emitting"; stop it while suspended so setPath is legal.
        try {
            if (ctx && ctx.state === "running") node.start();
            else node.stop();
        } catch (e) {
            log(`OhNoes start/stop failed: ${e}`);
        }
    }

    return {
        kind: "ohnoes",
        title: "OhNoesNode",
        uiRoot,
        setContext(newCtx) {
            ctx = newCtx;
        },
        ensureAudio,
        setControlsDisabled(disabled) {
            pathInput.disabled = disabled;
            stripRow.cb.disabled = disabled;
        },
        onContextSuspended() {
            if (!node) return;
            try {
                node.stop();
            } catch (e) {
                log(`OhNoes stop failed: ${e}`);
            }
        },
        onContextResumed() {
            if (!node) return;
            try {
                node.start();
            } catch (e) {
                log(`OhNoes start failed: ${e}`);
            }
        },
        disconnectAll() {
            safeDisconnect(node);
        },
        audioNode() {
            if (!node) throw new Error("OhNoesNode missing");
            return node;
        },
        outputNode() {
            if (!node) throw new Error("OhNoesNode missing");
            return node;
        },
        teardownAudio() {
            safeDisconnect(node);
            node = null;
        },
        teardown() {
            this.teardownAudio();
        },
    };
}
