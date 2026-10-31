const wifiForm = document.getElementById("wifi-form");
const statusText = document.getElementById("status");
const scanButton = document.getElementById("scan");
const networkList = document.getElementById("networks");
const wifiStep = document.getElementById("wifi-step");
const keysStep = document.getElementById("keys-step");
const keyForms = document.querySelectorAll(".key-form");

const POLL_INTERVAL_MS = 1000;

/* wifi_err_reason_t values from esp_wifi_types.h */
const WRONG_PASSWORD_REASONS = [15, 202, 204];
const NOT_FOUND_REASONS = [201];
const UNSUPPORTED_SECURITY_REASONS = [210, 211];

function failureMessage(reason) {
    if (WRONG_PASSWORD_REASONS.includes(reason)) {
        return "Wrong password, try again.";
    }
    if (NOT_FOUND_REASONS.includes(reason)) {
        return "Network not found, check the name and that it is in range.";
    }
    if (UNSUPPORTED_SECURITY_REASONS.includes(reason)) {
        return "The network must use WPA2 or WPA3.";
    }
    return `Could not connect (reason ${reason}).`;
}

function setBusy(form, busy) {
    for (const element of form.elements) {
        element.disabled = busy;
    }
}

/* The device's access point can drop for a moment while it moves to the router's
 * channel, so failed requests are retried until the device reports a result */
async function pollStatus() {
    let status;
    try {
        const response = await fetch("/api/wifi/status", { cache: "no-store" });
        status = await response.json();
    } catch {
        setTimeout(pollStatus, POLL_INTERVAL_MS);
        return;
    }

    if (status.state === "connected") {
        showKeysStep();
    } else if (status.state === "failed") {
        statusText.textContent = failureMessage(status.reason);
        setBusy(wifiForm, false);
    } else {
        setTimeout(pollStatus, POLL_INTERVAL_MS);
    }
}

function signalLabel(rssi) {
    if (rssi >= -60) {
        return "Strong";
    }
    if (rssi >= -70) {
        return "Good";
    }
    if (rssi >= -80) {
        return "Weak";
    }
    return "Very weak";
}

/* networks arrive sorted strongest first */
function showNetworks(networks) {
    networkList.replaceChildren();

    for (const network of networks) {
        const ssid = document.createElement("span");
        ssid.className = "ssid";
        ssid.textContent = network.ssid;

        const details = document.createElement("span");
        details.className = "details";
        details.textContent = `${signalLabel(network.rssi)}${network.secure ? " 🔒" : ", open"}`;

        const button = document.createElement("button");
        button.type = "button";
        button.title = `${network.rssi} dBm`;
        button.append(ssid, details);
        button.addEventListener("click", () => {
            wifiForm.elements.ssid.value = network.ssid;
            wifiForm.elements.pass.focus();
        });

        const item = document.createElement("li");
        item.append(button);
        networkList.append(item);
    }
}

/* The device's access point pauses while scanning, so the request takes a few seconds */
scanButton.addEventListener("click", async () => {
    setBusy(wifiForm, true);
    statusText.textContent = "Scanning...";

    try {
        const response = await fetch("/api/wifi/scan", { cache: "no-store" });
        if (!response.ok) {
            statusText.textContent = await response.text();
            return;
        }

        const networks = await response.json();
        showNetworks(networks);
        statusText.textContent = networks.length === 0 ? "No networks found." : "";
    } catch {
        statusText.textContent = "Scan failed, try again.";
    } finally {
        setBusy(wifiForm, false);
    }
});

wifiForm.addEventListener("submit", async (event) => {
    event.preventDefault();

    const data = new FormData(wifiForm);
    setBusy(wifiForm, true);
    statusText.textContent = "Connecting...";

    try {
        const response = await fetch("/api/wifi/connect", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ ssid: data.get("ssid"), pass: data.get("pass") }),
        });

        if (!response.ok) {
            statusText.textContent = await response.text();
            setBusy(wifiForm, false);
            return;
        }
    } catch {
        /* the AP may already have switched channel, the status poll picks it up */
    }

    pollStatus();
});

/* keys holds {"trafiklab": bool, "google": bool} as returned by /api/keys/status */
function showKeysVerified(keys) {
    for (const keyForm of keyForms) {
        if (keys[keyForm.dataset.service]) {
            keyForm.querySelector(".key-status").textContent = "✓ Key verified";
        }
    }

    if (Object.values(keys).every(Boolean)) {
        statusText.textContent = "All set! You can switch back to your own WiFi.";
    }
}

async function showKeysStep() {
    wifiStep.hidden = true;
    keysStep.hidden = false;
    statusText.textContent = "";

    try {
        const response = await fetch("/api/keys/status", { cache: "no-store" });
        showKeysVerified(await response.json());
    } catch {
        /* nothing verified is shown, the keys can still be added */
    }
}

/* The device checks the key with a request to the service, which takes a few seconds */
for (const keyForm of keyForms) {
    const keyStatus = keyForm.querySelector(".key-status");

    keyForm.addEventListener("submit", async (event) => {
        event.preventDefault();

        setBusy(keyForm, true);
        keyStatus.textContent = "Verifying...";

        try {
            const response = await fetch(`/api/keys/${keyForm.dataset.service}`, {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ key: keyForm.elements.key.value.trim() }),
            });

            if (!response.ok) {
                keyStatus.textContent = await response.text();
                return;
            }

            showKeysVerified(await response.json());
        } catch {
            keyStatus.textContent = "Could not reach the device, try again.";
        } finally {
            setBusy(keyForm, false);
        }
    });
}

/* a reload after the WiFi step continues with the keys */
(async () => {
    try {
        const response = await fetch("/api/wifi/status", { cache: "no-store" });
        const status = await response.json();
        if (status.state === "connected") {
            showKeysStep();
        }
    } catch {
        /* stay on the WiFi step */
    }
})();
