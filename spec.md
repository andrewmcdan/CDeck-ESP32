# Supervisor MCU Protocol

This document describes the microcontroller that supervises the Raspberry Pi,
radiomodems, and power system. The Python TUI already speaks this protocol via
`SupvClient`; new firmware should implement the behaviour below so the UI works
without further changes.

## Hardware role

* Provide battery telemetry (pack voltage, percentage, current if available)
* Monitor physical toggle switches (LTE/Wi‑Fi/BT/bridge enable, lid state, etc.)
* Count unread messages coming from external bridges or panel indicators
* Control `poweroff_ok` so the MCU can cut power only after the Pi has halted
* Watchdog the Pi at boot and reset it if it fails to start the TUI

## Electrical interface

* UART at 115200 8N1 on `/dev/ttyUSB1`
* 3.3 V logic, idle high
* Messages are JSON encoded in UTF‑8, delimited by `\n`
* No flow control—keep lines under ~512 bytes

## Message framing

Every line is a JSON object. Requests initiated by the Pi include a string `id`
field so the MCU can correlate responses. Asynchronous MCU events omit `id`.

```json
{"id":"4","cmd":"get_status"}
{"id":"4","ok":true,"status":{"battery_pct":78,"pack_mv":11750,"mcu_temp_c":36.5}}
{"event":"switch","switch":{"lte":true,"wifi":false,"bt":false}}
```

## Commands the Pi sends

| Command        | When it is used                                  | Expected response                                      |
|----------------|--------------------------------------------------|--------------------------------------------------------|
| `get_status`   | Immediately after the UART comes up, and on demand | `{"id":"N","ok":true,"status":{…}}` with telemetry fields |
| `get_switches` | At startup and after reconnect                   | `{"id":"N","ok":true,"switch":{…}}`                     |
| `clear_unread` | After the TUI subscribes to mesh events so the external unread indicator can reset | Optional ack (`{"id":"N","ok":true}`)                  |
| `arm_poweroff` | Right before the Pi invokes `poweroff`           | `{"id":"N","ok":true,"poweroff_ok":true}` once it is safe to cut power |
| `ping` (future)| Optional keepalive                               | `{"id":"N","ok":true,"uptime_s":...}`                   |

Requests may include extra fields, e.g. `{"cmd":"clear_unread","id":"7","source":"telegram"}`—the MCU should ignore unknown keys.

## Events the MCU publishes

| Event name   | Payload fields                                                                 | Notes |
|--------------|---------------------------------------------------------------------------------|-------|
| `telemetry`  | `battery_pct`, `pack_mv`, `pack_ma`, `mcu_temp_c`, `unread_ext`, `last_msg_age_s`, `uptime_s` | Sent every 2 s (or when a value changes) |
| `switch`     | `switch` dict containing booleans for `lte`, `wifi`, `bt`, `bridge_enable`, `lid_open`, `charger_online` | Emit whenever a switch changes |
| `heltec`     | `heltec` string (`"ok"`, `"fault"`, `"disconnected"`)                           | Optional, if the MCU monitors the radio |
| `unread`     | `unread_ext`, `last_msg_age_s`                                                  | Alternative to telemetry spam |
| `watchdog`   | `state` string, `uptime_s`                                                      | Indicates boot watchdog state |

The MCU can also send the same structure as the `status` response without wrapping it in an `event`; `SupvClient` accepts both.

## Poweroff handshake

1. Pi requests `arm_poweroff`.
2. MCU prepares to drop main power (assert GPIO to supervisor, stop chargers, etc.) and replies `{"id":"…","ok":true,"poweroff_ok":true}`.
3. After receiving the reply, the Pi sets the physical `poweroff_ok` GPIO high and issues `poweroff`.
4. Once the Pi stops driving the heartbeat GPIO the supervisor cuts power.

If the MCU cannot honour the request it should reply `{"id":"…","ok":false,"error":"battery_low"}` so the Pi can abort the shutdown.

## Expected telemetry fields

* `battery_pct` – integer 0–100 (or `None` if unknown)
* `pack_mv` – integer millivolts
* `mcu_temp_c` – float degrees Celsius
* `unread_ext` – integer count of unread notifications on external indicators
* `last_msg_age_s` – seconds since the last mesh packet seen by the supervisor
* `switch` – dictionary with booleans for at least `lte`, `wifi`, `bt`
* `heltec` – text status of the LoRa module (“OK”, “DISCONNECTED”, “FAULT”)
* `mcu` – firmware version or health string
* `uptime_s` – MCU uptime

## Example boot timeline

1. MCU powers the Pi, starts streaming telemetry every 2s.
2. Pi opens the UART and immediately sends `get_status` + `get_switches`.
3. MCU responds with current data, Pi merges it into the UI.
4. Pi sends `clear_unread` after the mesh subscription is live.
5. During operation the MCU emits telemetry and switch events whenever things change.
6. On shutdown the Pi issues `arm_poweroff`, waits for `poweroff_ok`, asserts its GPIO, and halts.

Following this contract will let the future firmware drop in without changes to the TUI.
