const wifiForm = document.getElementById("wifi-form");
const statusText = document.getElementById("status");
const scanButton = document.getElementById("scan");
const networkList = document.getElementById("networks");
const wifiStep = document.getElementById("wifi-step");
const keysStep = document.getElementById("keys-step");
const keyForms = document.querySelectorAll(".key-form");
const stopsStep = document.getElementById("stops-step");
const stopForm = document.getElementById("stop-form");
const stopStatus = document.getElementById("stop-status");
const stopList = document.getElementById("stops");
const departuresButton = document.getElementById("find-departures");
const departureStatus = document.getElementById("departure-status");
const departureList = document.getElementById("departures");
const addButton = document.getElementById("add-departure");
const addStatus = document.getElementById("add-status");

/* the stops of the last search by id, and the stop whose departures are listed */
let stopsById = new Map();
let departuresStop = null;

/* route.transport_mode values from Trafiklab */
const MODE_NAMES = { BUS: "Bus", METRO: "Metro", TRAIN: "Train", TRAM: "Tram", TAXI: "Taxi", BOAT: "Boat" };

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
        showStopsStep();
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

function showStopsStep() {
    keysStep.hidden = true;
    stopsStep.hidden = false;
    statusText.textContent = "";
    stopForm.elements.query.focus();
}

/* A selectable option in an .options list, the value is not shown */
function optionItem(group, value, text, details) {
    const input = document.createElement("input");
    input.type = "radio";
    input.name = group;
    input.value = value;

    const main = document.createElement("span");
    main.className = "option-text";
    main.textContent = text;

    const label = document.createElement("label");
    label.append(input, main);

    if (details) {
        const extra = document.createElement("span");
        extra.className = "details";
        extra.textContent = details;
        label.append(extra);
    }

    const item = document.createElement("li");
    item.append(label);
    return item;
}

function clearDepartures() {
    departureList.replaceChildren();
    departureStatus.textContent = "";
    addButton.hidden = true;
    addStatus.textContent = "";
}

/* stops arrive busiest first as [{"id": "...", "name": "...", "town": "..."}, ...] */
function showStops(stops) {
    stopsById = new Map(stops.map((stop) => [stop.id, stop]));
    stopList.replaceChildren(...stops.map((stop) => optionItem("stop", stop.id, stop.name, stop.town)));
    departuresButton.hidden = true;
    clearDepartures();
}

function departureText(departure) {
    return `${departure.line} ${departure.destination}`.trim();
}

/* A tracked departure can't be picked again */
function markTracked(input) {
    input.checked = false;
    input.disabled = true;
    input.closest("label").querySelector(".details").textContent = "✓ Tracked";
}

/* options arrive sorted by mode, line and destination as
 * [{"line": "...", "mode": "...", "destination": "...", "tracked": bool}, ...].
 * The departure is kept as the value for adding it */
function showDepartures(options) {
    departureList.replaceChildren(...options.map((option) => {
        const departure = { line: option.line, mode: option.mode, destination: option.destination };
        const item = optionItem("departure", JSON.stringify(departure), departureText(departure),
                                MODE_NAMES[option.mode] ?? option.mode);
        if (option.tracked) {
            markTracked(item.querySelector("input"));
        }
        return item;
    }));
}

/* picking another stop asks for its departures again */
stopList.addEventListener("change", () => {
    departuresButton.hidden = false;
    clearDepartures();
});

/* tracked departures are disabled, so any picked one can be added */
departureList.addEventListener("change", () => {
    addButton.hidden = false;
    addStatus.textContent = "";
});

addButton.addEventListener("click", async () => {
    const input = departureList.querySelector("input:checked");
    if (input === null || departuresStop === null) {
        return;
    }

    const departure = JSON.parse(input.value);
    addButton.disabled = true;
    addStatus.textContent = "Adding...";

    try {
        const response = await fetch("/api/targets", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ stop: departuresStop, departure }),
        });

        if (!response.ok) {
            addStatus.textContent = await response.text();
            return;
        }

        /* {"count": n, "max": n} */
        const result = await response.json();
        const mode = MODE_NAMES[departure.mode] ?? departure.mode;
        addStatus.textContent = `Added ${mode} ${departureText(departure)} from ${departuresStop.name} `
                              + `(${result.count} of ${result.max}).`;
        markTracked(input);
        addButton.hidden = true;
    } catch {
        addStatus.textContent = "Could not reach the device, try again.";
    } finally {
        addButton.disabled = false;
    }
});

/* The device samples the departures of a whole Wednesday with 8 requests, which can take up to a minute */
departuresButton.addEventListener("click", async () => {
    const stop = stopList.querySelector("input:checked");
    if (stop === null) {
        return;
    }

    departuresButton.disabled = true;
    clearDepartures();
    departuresStop = stopsById.get(stop.value) ?? null;
    departureStatus.textContent = "Searching the day's departures, this can take up to a minute...";

    try {
        const response = await fetch("/api/departures/available", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ stop_id: stop.value }),
        });

        if (!response.ok) {
            departureStatus.textContent = await response.text();
            return;
        }

        /* {"options": [...], "samples": 8, "failed": n} */
        const result = await response.json();
        showDepartures(result.options);

        const incomplete = result.failed > 0
            ? ` ${result.failed} of ${result.samples} samples failed, the list may be incomplete.`
            : "";
        departureStatus.textContent = result.options.length === 0
            ? `No departures found from this stop on a Wednesday.${incomplete}`
            : `Choose the departure to follow.${incomplete}`;
    } catch {
        departureStatus.textContent = "Could not reach the device, try again.";
    } finally {
        departuresButton.disabled = false;
    }
});

/* The device asks Trafiklab and then Google for each stop's town, which takes a few seconds */
stopForm.addEventListener("submit", async (event) => {
    event.preventDefault();

    const query = stopForm.elements.query.value.trim();
    if (query === "") {
        stopStatus.textContent = "Enter a stop name.";
        return;
    }

    setBusy(stopForm, true);
    showStops([]);
    stopStatus.textContent = "Searching...";

    try {
        const response = await fetch("/api/stops/search", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ query }),
        });

        if (!response.ok) {
            stopStatus.textContent = await response.text();
            return;
        }

        const stops = await response.json();
        showStops(stops);
        stopStatus.textContent = stops.length === 0 ? "No stops found, try another name." : "";
    } catch {
        stopStatus.textContent = "Could not reach the device, try again.";
    } finally {
        setBusy(stopForm, false);
    }
});

/* a reload after the WiFi step continues with the keys, or the stops once the keys are verified */
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
