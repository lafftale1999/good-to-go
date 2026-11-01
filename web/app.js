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
const savedCount = document.getElementById("saved-count");
const savedList = document.getElementById("saved-list");
const savedStatus = document.getElementById("saved-status");
const doneButton = document.getElementById("done");
const doneStatus = document.getElementById("done-status");
const loginStep = document.getElementById("login-step");
const loginForm = document.getElementById("login-form");
const loginStatus = document.getElementById("login-status");

/* Every API request but the login goes through here. The device answers 401 when the session
 * cookie is missing or has timed out, 5 minutes after the last request. The login form is then
 * shown, and the caller's promise never settles, so it shows no error of its own */
async function api(url, options) {
    const response = await fetch(url, options);
    if (response.status === 401) {
        showLogin();
        return new Promise(() => {});
    }
    return response;
}

function showLogin() {
    if (keysPoll !== null) {
        clearInterval(keysPoll);
        keysPoll = null;
    }

    for (const step of [wifiStep, keysStep, stopsStep]) {
        step.hidden = true;
    }
    statusText.textContent = "";
    loginStep.hidden = false;
    loginForm.elements.password.focus();
}

/* The device sets the session cookie, which the browser then sends with every request. The page
 * is loaded again, so it starts at the right step with nothing left over from before */
loginForm.addEventListener("submit", async (event) => {
    event.preventDefault();

    setBusy(loginForm, true);
    loginStatus.textContent = "Logging in...";

    try {
        const response = await fetch("/api/login", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ password: loginForm.elements.password.value.trim() }),
        });

        if (response.ok) {
            location.reload();
            return;
        }
        loginStatus.textContent = await response.text();
    } catch {
        loginStatus.textContent = "Could not reach the device, try again.";
    }
    setBusy(loginForm, false);
});

/* Collapses an .options list to the picked item when one is picked. The other items are only
 * hidden, so the button below the list shows them again without asking the device */
function collapsible(list, button, singular, plural) {
    function expand() {
        for (const item of list.children) {
            item.hidden = false;
        }
        button.hidden = true;
    }

    function collapseTo(picked) {
        const others = [...list.children].filter((item) => item !== picked);
        for (const item of others) {
            item.hidden = true;
        }
        button.textContent = `Show ${others.length} more ${others.length === 1 ? singular : plural}`;
        button.hidden = others.length === 0;
    }

    button.addEventListener("click", expand);
    list.addEventListener("change", (event) => collapseTo(event.target.closest("li")));

    /* called after the list is filled anew, which starts it expanded */
    return { reset: () => { button.hidden = true; } };
}

const stopCollapse = collapsible(stopList, document.getElementById("more-stops"), "stop", "stops");
const departureCollapse = collapsible(departureList, document.getElementById("more-departures"),
                                      "departure", "departures");

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
        const response = await api("/api/wifi/status", { cache: "no-store" });
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
        const response = await api("/api/wifi/scan", { cache: "no-store" });
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
        const response = await api("/api/wifi/connect", {
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

/* After the WiFi step the device verifies its stored keys by itself, so the key step checks
 * back until both are verified */
const KEYS_POLL_INTERVAL_MS = 3000;
let keysPoll = null;

async function refreshKeys() {
    try {
        const response = await api("/api/keys/status", { cache: "no-store" });
        showKeysVerified(await response.json());
    } catch {
        /* nothing verified is shown, the keys can still be added */
    }
}

function showKeysStep() {
    wifiStep.hidden = true;
    keysStep.hidden = false;
    statusText.textContent = "";

    refreshKeys();
    if (keysPoll === null) {
        keysPoll = setInterval(refreshKeys, KEYS_POLL_INTERVAL_MS);
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
            const response = await api(`/api/keys/${keyForm.dataset.service}`, {
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
    if (keysPoll !== null) {
        clearInterval(keysPoll);
        keysPoll = null;
    }

    /* the step can be shown straight after loading, when the device was already configured */
    if (!stopsStep.hidden) {
        return;
    }
    wifiStep.hidden = true;
    keysStep.hidden = true;
    stopsStep.hidden = false;
    statusText.textContent = "";
    stopForm.elements.query.focus();
    loadSaved();
}

/* saved holds {"targets": [{"stop": {...}, "departure": {...}}, ...], "max": n} as returned
 * by GET and POST /api/targets, in the order the departures were added */
function showSaved(saved) {
    const count = saved.targets.length;
    savedCount.textContent = count === 0
        ? "No departures have been added."
        : `${count}/${saved.max} departures`;

    savedList.replaceChildren(...saved.targets.map(({ stop, departure }) => {
        const description = `${MODE_NAMES[departure.mode] ?? departure.mode} ${departureText(departure)}`;

        const text = document.createElement("span");
        text.className = "saved-text";
        text.textContent = description;

        const where = document.createElement("span");
        where.className = "details";
        where.textContent = stop.town ? `${stop.name}, ${stop.town}` : stop.name;

        const remove = document.createElement("button");
        remove.type = "button";
        remove.className = "remove";
        remove.textContent = "✕";
        remove.title = "Remove";
        remove.setAttribute("aria-label", `Remove ${description} from ${stop.name}`);
        remove.addEventListener("click", () => removeSaved(remove, stop, departure));

        const item = document.createElement("li");
        item.append(text, where, remove);
        return item;
    }));
}

function sameDeparture(a, b) {
    return a.line === b.line && a.mode === b.mode && a.destination === b.destination;
}

/* A departure that is no longer tracked can be picked again from the listed departures */
function unmarkTracked(stop, departure) {
    if (departuresStop === null || departuresStop.id !== stop.id) {
        return;
    }

    for (const input of departureList.querySelectorAll("input")) {
        if (sameDeparture(JSON.parse(input.value), departure)) {
            input.disabled = false;
            input.closest("label").querySelector(".details").textContent =
                MODE_NAMES[departure.mode] ?? departure.mode;
        }
    }
}

async function removeSaved(button, stop, departure) {
    button.disabled = true;
    savedStatus.textContent = "";

    try {
        const response = await api("/api/targets", {
            method: "DELETE",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ stop_id: stop.id, departure }),
        });

        if (!response.ok) {
            savedStatus.textContent = await response.text();
            button.disabled = false;

            /* removed elsewhere, show what the device has */
            if (response.status === 404) {
                loadSaved();
            }
            return;
        }

        /* the saved departures after removing, like GET /api/targets */
        showSaved(await response.json());
        unmarkTracked(stop, departure);
    } catch {
        savedStatus.textContent = "Could not reach the device, try again.";
        button.disabled = false;
    }
}

async function loadSaved() {
    try {
        const response = await api("/api/targets", { cache: "no-store" });
        if (response.ok) {
            showSaved(await response.json());
            return;
        }
    } catch {
        /* falls through to the message below */
    }
    savedCount.textContent = "Could not load the saved departures.";
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
    departureCollapse.reset();
    departureStatus.textContent = "";
    addButton.hidden = true;
    addStatus.textContent = "";
}

/* stops arrive busiest first as [{"id": "...", "name": "...", "town": "..."}, ...] */
function showStops(stops) {
    stopsById = new Map(stops.map((stop) => [stop.id, stop]));
    stopList.replaceChildren(...stops.map((stop) => optionItem("stop", stop.id, stop.name, stop.town)));
    stopCollapse.reset();
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
    departureCollapse.reset();
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
        const response = await api("/api/targets", {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify({ stop: departuresStop, departure }),
        });

        if (!response.ok) {
            addStatus.textContent = await response.text();
            return;
        }

        /* the saved departures after adding, like GET /api/targets */
        const saved = await response.json();
        showSaved(saved);

        const mode = MODE_NAMES[departure.mode] ?? departure.mode;
        addStatus.textContent = `Added ${mode} ${departureText(departure)} from ${departuresStop.name} `
                              + `(${saved.targets.length} of ${saved.max}).`;
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
        const response = await api("/api/departures/available", {
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
        const response = await api("/api/stops/search", {
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

/* The device's configuration becomes valid by itself, the button only asks whether it is and
 * tells what is still missing. Answered like /api/status, with 409 while it isn't valid */
doneButton.addEventListener("click", async () => {
    doneButton.disabled = true;
    doneStatus.textContent = "";

    try {
        const response = await api("/api/config/apply", { method: "POST" });
        const status = await response.json();

        if (status.valid_config) {
            doneStatus.textContent = "Your device is good to go! You can close this page.";
            return;
        }

        const missing = [];
        if (!status.connected) {
            missing.push("a WiFi connection");
        }
        if (!status.keys_verified) {
            missing.push("verified API keys");
        }
        if (!status.has_departures) {
            missing.push("at least one saved departure");
        }
        doneStatus.textContent = `Not done yet, the device still needs ${missing.join(", ")}.`;
    } catch {
        doneStatus.textContent = "Could not reach the device, try again.";
    } finally {
        doneButton.disabled = false;
    }
});

/* The device's flags decide the first step: WiFi when not connected, then the keys until they are
 * verified, then the departures. Covers both a reload and a device configured from NVS at boot */
(async () => {
    try {
        const response = await api("/api/status", { cache: "no-store" });
        const status = await response.json();

        if (status.connected && status.keys_verified) {
            showStopsStep();
        } else if (status.connected) {
            showKeysStep();
        } else {
            wifiStep.hidden = false;
        }
    } catch {
        /* start at the WiFi step */
        wifiStep.hidden = false;
    }
})();
