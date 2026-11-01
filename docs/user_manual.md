---
title: "Good To Go"
subtitle: "User Manual"
lang: en-GB
papersize: a4
fontsize: 11pt
geometry: margin=2.5cm
numbersections: true
toc: true
toc-depth: 2
colorlinks: true
linkcolor: blue
urlcolor: blue
---

Good To Go shows when your bus, train or tram leaves, in realtime, so you know when to head out the door. This manual
is for the person using the device. To build one, see the
[README](https://github.com/lafftale1999/good-to-go#readme).

# The device

* **Screen:** two rows of 16 characters.
* **Button:** the only control. It is used with clicks and holds, see [Everyday use](#everyday-use).
* **USB cable:** powers the device. Any USB phone charger works.

You need:

* A **2.4 GHz WiFi network** with a password (WPA2 or WPA3). 5 GHz-only networks don't work.
* A **phone or computer** with a web browser for the setup.
* Two **API keys**, one from Trafiklab and one from Google. If the device came with them built in, you can skip them.
  Otherwise, ask the person who built it, or see
  [Create the API keys](https://github.com/lafftale1999/good-to-go#create-the-api-keys) in the README.

# First-time setup

1. **Plug in the device.** The screen shows `Configuring...` / `Soon good to go!`, then the name and password of the
   device's own WiFi network:

   ```
   WIFI: GTG12345
   PASS: 12345678
   ```

   The digits are random and change each time the device's network starts.

2. **Join that network** with your phone or computer, using the password on the screen. The screen changes to
   `Surf to webpage:` / `goodtogo.local`.

3. **Open `https://goodtogo.local`** in your browser.

   Your browser may warn that the connection isn't private. This is expected: the device uses its own certificate,
   which your browser doesn't know. Choose *Advanced* and continue to the page (the wording differs between
   browsers). To get rid of the warning, install the device's certificate, see
   [Generate certificates](https://github.com/lafftale1999/good-to-go#generate-certificates) in the README.

4. **Connect to your WiFi.**
   1. Press **Scan for networks** and choose your home network, or type its name.
   1. Enter the password and press **Connect**.

   Once the device is connected, the page moves on to the API keys. When you later leave the device's network, it
   shuts down, and the page is reachable at `https://goodtogo.local` on your home network instead.

5. **Add the API keys.** Enter the Trafiklab key and the Google key and press **Verify** for each. The device checks
   each key with its service, and the page shows a check mark and *Key verified* when it works. Keys built into the
   device are verified by themselves and shown as verified.

6. **Add your departures.** You can track up to 8.
   1. Type the name of your stop, e.g. *Sveavägen*, and press **Search**. The 10 best matches are listed with their
      town, so stops with the same name can be told apart.
   1. Choose your stop and press **Search for available departures**. This looks through a whole day of departures
      and can take up to a minute.
   1. Choose a line and direction and press **Add to departures**. Departures you already track are marked
      *Tracked*.
   1. Repeat for every departure you want. Your saved departures are listed at the top of the page, and the cross
      next to one removes it.

7. **Press Done.** The page says *Your device is good to go!* when everything is set up, or what is still missing.

The screen then shows `U R Good To Go!` / `Press to search.` The setup page stays on, and the screen shows
`Stop config mode` / `Hold 5 sec...` until you hold the button for 5 seconds, or until the page has been unused for
5 minutes.

# Everyday use

## The button

| Push                            | What happens                                                                  |
|---------------------------------|-------------------------------------------------------------------------------|
| **Click**                       | Shows the chosen departure. Fetches new times first if they are a minute or older. |
| **Double-click**                | Moves to the next departure.                                                  |
| **Hold 5 seconds**, then let go | Turns the setup page on or off.                                               |
| **Hold 20 seconds**             | Factory reset, see [Factory reset](#factory-reset).                           |

A double-click is two clicks in quick succession. When you hold the button, count the seconds: letting go between 5
and 20 seconds turns the setup page on or off, while at 20 seconds the factory reset starts at once.

## Reading a departure

```
515 Sundbybergs.
now, 13, 28 min
```

* **Top row:** the line and its destination. A name too long for the screen is cut, and ends with a full stop.
* **Bottom row:** the minutes until the next 3 departures. `now` means less than a minute.

The times count down by themselves, and new times are fetched in the background every minute while a departure is
shown. Realtime estimates are used when the operator has them, otherwise the timetable. Cancelled departures and
departures more than 60 minutes away are left out.

After the last departure, a double-click shows `Need more stops?` / `Hold 5 sec...`, and the next one starts over from
the first.

The screen has no å, ä or ö, so they are shown as a, a and o.

# Changing the setup

1. Hold the button for 5 seconds and let go.
2. If the device is connected to your WiFi, the screen shows `Surf to webpage:` / `goodtogo.local`. Open
   `https://goodtogo.local` from a phone or computer on the same network.

   If it isn't connected, the screen shows the device's own network and password instead. Join it and open the
   page as in [First-time setup](#first-time-setup).
3. Make your changes. Added and removed departures take effect right away, starting from the first departure.
4. Hold the button for 5 seconds again to turn the page off, or leave it: it turns off by itself after 5 minutes
   without use.

# Sleep

After **5 minutes** without a button push, or without use of the setup page, the device shows
`Going to sleep` / `Press to wake` and turns the screen off.

**Push the button once to wake it.** It then starts up again, which takes a few seconds: it connects to your WiFi,
checks the keys and fetches all departures (`Fetching...`), then shows `U R Good To Go!`.

# Messages on the screen

The departure messages on the bottom row keep the departure's name on the top row.

| Top row            | Bottom row         | Meaning                                                  |
|------------------------|------------------------|----------------------------------------------------|
| `Configuring...`   | `Soon good to go!` | Starting up. |
| `WIFI: GTG...`     | `PASS: ...`        | The setup page is on and the device has its own network. Join it to set it up. |
| `Surf to webpage:` | `goodtogo.local`   | Open `https://goodtogo.local`. |
| `Fetching...`      |                    | Fetching all departures after starting. |
| `U R Good To Go!`  | `Press to search.` | Ready. Click to see a departure. |
|                    | `Updating...`      | Fetching new times for the shown departure. |
|                    | `No departures...` | Nothing leaves within 60 minutes. |
|                    | `Fetch failed`     | The times couldn't be fetched. It is tried again in a minute or on the next click. |
| `Need more stops?` | `Hold 5 sec...`    | After the last departure. Hold the button to add more on the setup page. |
| `Stop config mode` | `Hold 5 sec...`    | The setup page is on. Hold the button for 5 seconds to turn it off. |
| `Connection lost!` | `Retrying: 2/5`    | The WiFi dropped, the device is trying to reconnect. |
| `Connection lost!` | `Starting config`  | Reconnecting failed, the setup page starts. See [Troubleshooting](#troubleshooting). |
| `Fetch failed 3x`  | `Starting config`  | Three fetches in a row failed, the setup page starts. |
| `Error:`           | `No connection...` | The device isn't connected to WiFi. |
| `Error:`           | `Time unsynched`   | The device doesn't know the time yet. It needs internet access. |
| `Error:`           | `API keys invalid` | A key is missing or was rejected. Enter it again on the setup page. |
| `Error:`           | `No stops added`   | No departures are saved. Add them on the setup page. |
| `Going to sleep`   | `Press to wake`    | Going to sleep, push the button to wake it. |
| `Factory reset`    | `Restarting...`    | Everything is erased, the device restarts into the setup. |
| `Factory reset`    | `failed, retry`    | The reset failed. Hold the button for 20 seconds again. |

# Troubleshooting

## The WiFi connection was lost

The device tries to reconnect 5 times (`Retrying: 1/5` to `5/5`). If it succeeds, it goes back to what it showed.
If not, the setup page starts with the device's own network. When your WiFi is back, either:

* push the button to wake the device once it has gone to sleep, which restarts it and reconnects, or
* join the device's network, open `https://goodtogo.local` and connect to your WiFi again.

## Fetch failed 3x

The device has WiFi but can't get departures. Your internet may be down, or Trafiklab may be unavailable. Check the
API keys on the setup page, and wait a few minutes before trying again. A fetch that works resets the count.

## goodtogo.local doesn't open

* Make sure your phone or computer is on the device's network, or on the same home network as the device.
* Type the whole address, including `https://`.
* Check that the screen shows that the setup page is on. If not, hold the button for 5 seconds.
* Turn off mobile data on your phone while joined to the device's network, so the phone doesn't use mobile data
  instead.

## My WiFi network isn't in the list

The device only sees 2.4 GHz networks, and hidden networks aren't listed. Networks without a password are listed,
marked *open*, but the device can't connect to them.

## The screen is blank

The device may be asleep: push the button once. If it stays blank, check the USB power.

## The screen lights up but shows no text

The contrast needs adjusting, with the small screw on the back of the screen.

## I've changed WiFi password or moved

Hold the button for 5 seconds. If the device can't connect, it shows its own network: join it and connect to the new
WiFi from the setup page.

# Factory reset

A factory reset erases the WiFi settings, the API keys you entered and all saved departures. API keys built into the
device stay, and are used again after the reset.

**Hold the button for 20 seconds.** The screen shows `Factory reset` / `Restarting...` and the device restarts into
[First-time setup](#first-time-setup). This works at any time, also during startup.

# Privacy and security

* **The setup page has no password.** While it is on, anyone on the same network can open it and change the
  settings. It turns off 5 minutes after it was last used, or when you hold the button for 5 seconds.
* **The WiFi password and API keys are stored unencrypted** on the device. Do a [factory reset](#factory-reset)
  before you give the device away.
* The device only contacts Trafiklab and Google for departures and stop towns, and time servers for the clock. Both
  services are reached over encrypted connections.
* **Use a strong WiFi password.** The device can be tricked into using the older WPA2 standard by someone imitating
  your network, which makes a weak password easier to guess.
