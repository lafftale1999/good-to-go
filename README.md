# Good To Go

A small ESP32 device by the front door that shows when your bus, train or tram leaves, in realtime, just like the
departure boards at the stop.

## Background

My friend [calleengstrom](https://github.com/calleengstrom) showed me this great idea. We both live close to our bus
stop, and realtime information is often the difference between standing in the cold for 1 minute or for 10.
Now I have a device right next to the door that works just like SL's own realtime boards.

It has saved me a lot of trouble and freezing, and I hope it can help you too!

### What it does

* Shows up to 8 departures you choose, one at a time, on a 16x2 LCD: the line and destination on the top row and the
  next 3 departures on the bottom row, e.g. `3, 13, 28 min`.
* The times count down by themselves and are fetched again in the background every minute while shown.
* Everything is set up from your phone or computer at `https://goodtogo.local`: WiFi, API keys and departures. The
  page is protected by a password that the device shows on its screen, so only someone at the device can log in.
* One button controls it:

  | Push                   | Action                                                         |
  |------------------------|----------------------------------------------------------------|
  | Click                  | Show the next departure, refreshed if it is 60 s or older      |
  | Triple-click           | Go to deep sleep                                               |
  | Hold 5 s               | Turn the configuration page on or off, at 5 s while still held |
  | Hold 20 s              | Factory reset                                                  |

  While the configuration page is on, only the holds work: clicks and triple-clicks are ignored, so they can't hide
  the login password. Hold 5 s to turn the page off first.
* It goes to deep sleep after 5 minutes without use, or on a triple-click. One push wakes it.

Departures come from [Trafiklab](https://www.trafiklab.se/), which covers public transport in all of Sweden.

---

## 1. Prerequisites

### Build tools

The project is built with Espressif's **ESP-IDF**, which works the same on Windows, macOS and Linux. It is tested with
**ESP-IDF v6.1**.

1. Install [Git](https://git-scm.com/downloads).

2. Install ESP-IDF v6.1 with the
   [ESP-IDF Installation Manager (EIM)](https://docs.espressif.com/projects/idf-im-ui/en/latest/), or follow
   [Espressif's getting started guide](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/index.html)
   for your OS. The installer brings its own Python, CMake and compiler.

3. Clone the project with its submodules:

   ```sh
   git clone --recursive https://github.com/lafftale1999/good-to-go.git
   cd good-to-go
   ```

   If you cloned without `--recursive`, run `git submodule update --init --recursive`.

4. Open an **ESP-IDF terminal**, so `idf.py` is on the path. EIM adds a shortcut for it; otherwise run the export
   script of your installation (`export.sh`, `export.ps1` or `export.bat`).

Every command below is run from the project folder in that terminal.

### Generate certificates

The configuration page is served over HTTPS, so the device needs a certificate. The build embeds it from `cred/`
and fails without it. Generate it once:

| OS                 | Command                     |
|--------------------|-----------------------------|
| macOS / Linux      | `./cred/gen_cert.sh`        |
| Windows PowerShell | `.\cred\gen_cert.ps1`       |
| Windows cmd        | `cred\gen_cert.bat`         |

All three run the same `cred/gen_cert.py`, which only needs the Python from ESP-IDF. It creates:

| File                  | What it is                                                                  |
|-----------------------|-----------------------------------------------------------------------------|
| `cred/ca.pem`         | Your own local certificate authority. Trust it on your devices, see below.  |
| `cred/ca.key`         | The authority's private key. Keep it private, it is reused on later runs.   |
| `cred/servercert.pem` | The device's certificate for `goodtogo.local`, embedded in the firmware.   |
| `cred/prvtkey.pem`    | The device's private key, embedded in the firmware.                         |

Running the script again makes a new device certificate signed by the same authority, so your devices only have to
trust `ca.pem` once. `--force-ca` makes a new authority too. Everything in `cred/` except the scripts and
`api_roots.pem` stays out of Git.

**Trusting `ca.pem`** (recommended: without it the browser warns that the page isn't secure, and can't tell the
device from someone imitating it when you log in):

* **Windows:** `certutil -addstore -user Root cred\ca.pem`
* **macOS:** open `ca.pem` in Keychain Access, then set *Trust → When using this certificate* to *Always Trust*.
* **iPhone / iPad:** send the file to the device and install the profile, then turn it on under *Settings → General →
  About → Certificate Trust Settings*.
* **Android:** *Settings → Security → Encryption & credentials → Install a certificate → CA certificate*. The menu
  names vary between phone makers.
* **Firefox** keeps its own list: *Settings → Privacy & Security → Certificates → View Certificates → Import*.

### Create the API keys

The device needs two API keys. You enter them on the configuration page, or build them into the firmware, see
[Factory API keys](#1-optional-factory-api-keys).

#### Trafiklab Realtime API

Trafiklab gives the departures. Follow [Trafiklab's getting started guide](https://www.trafiklab.se/docs/getting-started/using-trafiklab/)
to create an account and a project, then add a **Trafiklab Realtime API** key to the project.

#### Google Maps Geocoding API

Many stops in Sweden share names. To tell them apart, the device uses the Google Maps Geocoding API to turn each stop's
coordinates into its postal town, which is shown next to the name when you search for a stop. The Geocoding API
requires a Google Cloud project **with billing enabled**.

1. **Create a Google account.** A regular, free account works: <https://accounts.google.com/signin>
2. **Create an API key.** Follow Google's instructions:
   <https://developers.google.com/maps/documentation/geocoding/get-api-key>
3. **Restrict the key.** Under *API restrictions*, limit the key to the **Geocoding API** only. Then go to
   *Google Maps Platform → Quotas → Geocoding API* and cap the requests per day, so a leaked key can't run up a bill.

---

## 2. Hardware

| Part                         | Notes                                                                                   |
|------------------------------|-----------------------------------------------------------------------------------------|
| ESP32 development board      | The original ESP32 (e.g. ESP32-DevKitC or ESP32-WROOM-32). At least 2 MB flash, no PSRAM needed. |
| 16x2 LCD (1602) with I2C     | An HD44780 1602 display with a PCF8574 I2C backpack at address `0x27`.                  |
| Push button                  | Any momentary push button.                                                              |
| Jumper wires                 | Female-female for boards with pin headers, or a breadboard.                            |
| USB cable                    | Fits your board (USB-C on most newer boards) and carries data, not only power.         |

* **WiFi:** the ESP32 only supports **2.4 GHz** networks secured with WPA2 or WPA3.
* **No resistor is needed for the button.** The ESP32's internal pull-down resistor is used.
* The LCD is optional. The device works without it, which can help when testing.

### Wiring

| From         | To ESP32 pin   |
|--------------|----------------|
| LCD `GND`    | `GND`          |
| LCD `VCC`    | `5V` / `VIN`   |
| LCD `SDA`    | `GPIO21`       |
| LCD `SCL`    | `GPIO22`       |
| Button, leg 1 | `3V3`         |
| Button, leg 2 | `GPIO4`       |

The pins are set in `components/project_config/include/project_config.h`. If the LCD lights up but shows no text,
turn the contrast screw on the backpack.

> Most I2C backpacks pull SDA and SCL up to their own supply. Powered from 5 V, the data lines sit at 5 V, which is
> above the ESP32's rated 3.3 V. It usually works, but a level shifter, or a backpack powered from 3.3 V if your
> display is readable at that voltage, is the safe choice.

---

## 3. Build and flash

### 1. Optional: factory API keys

If you give the device away or sell it, you can build your API keys into the firmware, so the new owner only has to
set up WiFi and departures. Create `cred/credentials.h`:

```c
#define FACTORY_TRAFIKLAB_API_KEY "your-trafiklab-api-key"
#define FACTORY_GOOGLE_API_KEY    "your-google-api-key"
```

Either key can be left out. A factory key is only used while no key is saved on the device, and is saved once the
service accepts it. A key entered on the configuration page takes over from it.

> The keys are stored as plain text in the firmware. Anyone with the device can read them out of the flash, so
> restrict them as described above and only use this for devices you trust the owner of.

### 2. Build

```sh
idf.py set-target esp32
idf.py build
```

`set-target` is only needed the first time. The first build also downloads the mDNS component and builds the
LittleFS image with the web page.

### 3. Flash

Connect the board with USB and flash it, with `PORT` replaced by the board's serial port:

```sh
idf.py -p PORT flash monitor
```

| OS      | Typical port                                         |
|---------|------------------------------------------------------|
| Windows | `COM3`, see *Device Manager → Ports (COM & LPT)*     |
| macOS   | `/dev/cu.usbserial-*` or `/dev/cu.SLAB_USBtoUART`    |
| Linux   | `/dev/ttyUSB0` or `/dev/ttyACM0`                     |

If no port shows up, install the USB driver for your board's USB chip, usually
[CP210x](https://www.silabs.com/developer-tools/usb-to-uart-bridge-vcp-drivers) or CH340. `monitor` shows the
device's log, quit it with `Ctrl+]`.

### 4. Set it up

1. The LCD shows the name and password of the device's own WiFi network, `GTG` followed by digits. Join it with your
   phone or computer.
2. The LCD now shows `goodtogo.local` and the login password, e.g. `PASS: K7MPX2RQ9D`. Open
   **`https://goodtogo.local`** and log in with that password. The LCD then shows `Configuring...`.
3. Follow the steps on the page:
   1. **WiFi:** choose your home network and enter its password. The device's own network shuts down once you leave
      it, and the page is then reachable at `https://goodtogo.local` on your home network.
   2. **API keys:** enter and verify both keys, unless the device already has them.
   3. **Departures:** search for your stop, choose the departures you want (up to 8) and add them.
   4. **Done.**
4. The device shows `U R Good To Go!`. Click the button to see the first departure.

To change the setup later, hold the button for 5 seconds, open `https://goodtogo.local` again and log in with the
password on the LCD.

### How the login works

* A new password is made every time the configuration mode starts: 10 capital letters and digits, without the
  easily mixed up I, O, 0 and 1. Small letters are accepted when typing it.
* `POST /api/login` with the right password starts a session. The browser stores its random token in an `HttpOnly`,
  `Secure`, `SameSite=Strict` cookie and sends it with every request.
* A session ends 5 minutes after its last successful request, one answered with a `2xx` status: only those renew
  it. All sessions end when the configuration mode stops. Up to 4 browsers can be logged in at a time.
* Every API endpoint but the login answers `401` without a valid session, and the page then shows the login form.
  The page's own files are open, as the login form is part of them.
* After 5 wrong passwords in a row, the login is locked for 30 seconds.
* Only successful logged in requests count as activity, so neither someone who isn't logged in nor a request that
  keeps failing can keep the configuration mode or the device awake.

---

## 4. Known limitations

**Using it**

* The departure screen shows the line and destination, not the stop name.
* The LCD has no å, ä or ö, so they are shown as a, a and o.
* A fetch blocks the device while it runs. Button presses meanwhile are handled afterwards.
* Only Swedish public transport is supported, as the data comes from Trafiklab.
* WiFi must be 2.4 GHz.

**Connection and errors**

* If the WiFi drops while running, the device retries 5 times and then starts the configuration mode. It doesn't
  reconnect by itself after that, even if the network comes back: reconnect it from the configuration page, or press
  the button to wake it from sleep, which restarts it.
* After 3 failed fetches in a row the configuration mode starts.
* A configuration page left open and logged in on the API keys step keeps the configuration mode running, as the page
  checks the device every 3 seconds.
* The login password is only on the LCD until something else is shown there, e.g. `Configuring...` once someone
  logs in. To see a password again, turn the configuration mode off and on, which makes a new one.

**Security**

* Anyone who can see the device's screen can log in to the configuration page while it is on. It turns off 5 minutes
  after the last successful request once the device is configured.
* Until `ca.pem` is trusted on your phone or computer, the browser can't tell the real device from someone imitating
  it on the network, who could then catch the login password. Trust `ca.pem`, see
  [Generate certificates](#generate-certificates).
* WiFi credentials and API keys are stored in plain text in the device's flash.
* The device accepts WPA2 even if your network uses WPA3. Someone imitating your network could then capture a WPA2
  handshake and try to guess your WiFi password offline, so use a strong WiFi password.
* The clock is synced with NTP, which isn't authenticated.

---

## License

Good To Go is released under the [MIT License](LICENSE): you may use, copy, change and share it, also commercially,
as long as the copyright notice and license text are kept. It comes without any warranty, and the authors aren't
liable for how it is used.

Bundled third-party code keeps its own license: [cJSON](https://github.com/DaveGamble/cJSON) (MIT) in
`main/lib/cJSON`, and the [esp_littlefs](https://github.com/joltwallet/esp_littlefs) (MIT) submodule.
