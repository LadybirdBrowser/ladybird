const REGISTRY = new Map();
let globalListenersInstalled = false;

function syncDialogs() {
    if (!window.ladybird) {
        return;
    }

    const activeHash = location.hash.slice(1);

    for (const [hash, { dialog, onOpen }] of REGISTRY) {
        const shouldBeOpen = hash === activeHash;
        if (shouldBeOpen && !dialog.open) {
            onOpen?.();
            if (!dialog.open) {
                dialog.showModal();
            }
        } else if (!shouldBeOpen && dialog.open) {
            dialog.close();
        }
    }
}

export function registerDialogDeepLink({ hash, tab, dialog, onOpen }) {
    REGISTRY.set(hash, { tab, dialog, onOpen });

    dialog.addEventListener("close", () => {
        if (location.hash.slice(1) === hash) {
            history.back();
        }
    });

    if (globalListenersInstalled) {
        return;
    }
    globalListenersInstalled = true;

    window.addEventListener("hashchange", syncDialogs);

    document.addEventListener("WebUILoaded", () => {
        const hash = location.hash.slice(1);
        const config = REGISTRY.get(hash);
        if (!config) {
            return;
        }

        history.replaceState(null, "", `#${config.tab}`);
        location.hash = hash;
    });
}

export function tabForDeepLink(hash) {
    return REGISTRY.get(hash)?.tab ?? hash;
}
