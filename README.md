# CITNABS

**Completely Inconspicuous Totally Not A Bomb Summoning Device**

A two-unit paging system built on M5StickS3. Press a button on one and the other beeps. That's it. Does exactly what it says on the tin and absolutely nothing else.

![two M5StickS3 units doing completely normal things](https://img.shields.io/badge/threat_level-negligible-brightgreen)

## Hardware

- 2× [M5StickS3](https://docs.m5stack.com/en/core/M5StickC%20PLUS2) (ESP32-S3, 135×240 LCD, built-in speaker)
- 1× USB-C cable that actually carries data (the hard part)

## Features

- **Zero infrastructure** — ESP-NOW peer-to-peer, no router, no server, no cloud, no subscription
- **Auto-pairing** — flash both units with the same firmware, they find each other on first boot and remember each other across reboots
- **Acknowledge & ETA** — dismiss the alarm on the summoned unit, summoner sees a 20-second "ON THE WAY" countdown
- **Anti-stacking** — repeat button mashes are rejected while an action is already in flight
- **Out-of-range feedback** — if the summoner can't reach the summonee it beeps itself so you know to escalate (yell, walk faster, etc.)
- **Display sleep** — screen dims after 5 seconds idle, wakes on button press or incoming event
- **Charging screen** — battery level + charge indicator when plugged in
- **Low battery chirp** — periodic warning tone below 15%
- **Volume control** — double-tap the side button to cycle MUTE → LOW → MED → HIGH
- **Blue on black** — because it looks cool
- **Battery-optimized** — automatic CPU light-sleep + WiFi modem power-save + slower heartbeat. Idle current drops from ~80 mA to ~5–10 mA; expected runtime on the StickS3's 200 mAh cell is ~20 hours vs ~3 hours unoptimized.

## Button map

| Button | Action |
|--------|--------|
| BtnA | Summon peer / dismiss incoming alarm |
| BtnB single-tap | Cancel the "on the way" countdown (quiet) |
| BtnB double-tap | Cycle volume (MUTE → LOW → MED → HIGH) |
| BtnB hold at boot | Factory-reset pairing |

## Building & flashing

Requires [Arduino CLI](https://arduino.github.io/arduino-cli/) and the M5Stack ESP32 core 3.x.

```bash
# Install core and libraries (once)
arduino-cli core install m5stack:esp32
arduino-cli lib install M5Unified M5GFX

# Compile
arduino-cli compile --fqbn m5stack:esp32:m5stack_sticks3 .

# Flash (repeat for second unit)
arduino-cli upload --fqbn m5stack:esp32:m5stack_sticks3 --port /dev/cu.usbmodem2101 .
```

Flash both units with identical firmware. Unit 1 will show `PAIRING...` — power on unit 2 and they'll find each other within a second. Pairing is stored in NVS and survives reflashes.

## State machine

```
IDLE ──[A pressed]──► SUMMONING (awaiting ack, self-beeps if no signal)
                            │
                       [peer acks]
                            │
                            ▼
                      ON THE WAY (20s countdown, B to quiet)

      [peer summoned me] ──► ALARM (beeping, red screen)
                                  │
                             [A pressed]
                                  │
                                  ▼
                             sends ACK → peer enters ON THE WAY
```

## P10 audit

See [P10_AUDIT.md](P10_AUDIT.md). Zero warnings from user code under `--warnings all`.

## License

MIT
