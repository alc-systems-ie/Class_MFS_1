# `gpio_pin_test` — bench pin test for the nRF54L15 DK

A standalone Zephyr application, independent of the MFS_1 build. Each button
toggles one raw GPIO and one indicator LED together, so **the LED is lit exactly
when the pin is asserted high**.

| Button | Test pin | Software LED | Hardwired LED |
|---|---|---|---|
| Button 0 — P1.13 | **P2.07** (stands in for `fire1`) | LED1 — P1.10 | LED2 |
| Button 1 — P1.09 | **P2.09** (stands in for `fire2`) | LED3 — P1.14 | LED0 |

Both pins are `GPIO_ACTIVE_HIGH` and start low. Presses are debounced by 30 ms
in a work item; the button is re-read after the delay, so a bounce spike that
has already released does not toggle anything.

Each press lights **two** LEDs. That is deliberate — see below.

## Pin levels

DK GPIOs swing **0 – 1.8 V**, not 3.3 V. The onboard buck supplies VDDM at a
default of 1.8 V (Normal Voltage Mode); it is adjustable 1.8–3.3 V, but only
through the **Board Configurator** app in nRF Connect for Desktop — there is no
`nrfutil` subcommand for it. Worth remembering before driving a MOSFET gate or a
3 V logic input.

## Why P2.07 and not P2.05 (which is what MFS_1 actually uses as `fire1`)

**P2.00–P2.05 do not reach the P2 connector directly.** They pass through two
TMUX1574 analog muxes (U3 and U8) whose shared `SEL` net carries a 100 kΩ
pull-up to `VDD:SW_PU`, so by default all six are routed to the onboard MX25R64
QSPI flash (the `_B` side of the mux), not to the header (the `_A` side).

Disabling `&spi00` in the overlay stops the SPI *driver* owning the pin, but it
does nothing to the mux. Confirmed on the bench: the SoC drove P2.05, the LED
lit, and the header pad stayed dead. Reaching it needs board rework — **cut
SB11, short SB22**.

**P2.06–P2.10 bypass the muxes entirely** and go straight to the connector,
which is why P2.07 is a drop-in replacement and P2.05 is not.

This matters beyond this test app: the main MFS_1 DK overlay maps `fire1` to
P2.05, so **`fire1` cannot be bench-verified on a stock DK** without that
rework. Worth knowing before the bench checklist leans on it.

## Why two LEDs per press

Both test pins are also wired to DK LEDs — **P2.07 is LED2, P2.09 is LED0**.
They are disabled as LED *nodes* in the overlay so nothing else can claim them,
but the copper remains, so asserting a test pin lights its DK LED whether
firmware asks for it or not.

That is useful, not accidental:

- **LED2 / LED0** report the state of the *pin itself*.
- **LED1 / LED3** report what the *firmware believes* (driven in software).

If those ever disagree, the fault is between the SoC pad and the LED, not in the
application. That is precisely the distinction that identified the P2.05 mux
problem.

Note P2.07 is also the debugger's SWO line (SB24, closed by default) and
TRACEDATA[0]. Neither is in use here — RTT runs over SWD and needs no SWO — but
do not enable SWO output while this test is running.

## Build and flash

```sh
cd tools/gpio_pin_test
west build -b nrf54l15dk/nrf54l15/cpuapp -p always
west flash --recover
```

`--recover` is not optional on this board — plain `west flash` has reported
success while the old image kept running.

No `credentials.conf` is needed; this app has no BLE, crypto or settings.

## Watching it

Logging is RTT (house style), so the DK's VCOM is silent by design:

```sh
JLinkRTTLogger -Device nRF54L15_M33 -If SWD -Speed 4000 -RTTChannel 0 <file>
```

To use the DK's USB serial instead, swap the two backend lines in `prj.conf`:

```ini
CONFIG_LOG_BACKEND_RTT=n
CONFIG_LOG_BACKEND_UART=y
```

Expected output on a press:

```
Channel A: P2.07 HIGH, LED on
Channel A: P2.07 LOW, LED off
```

## Measuring the pins

P2.07 and P2.09 are on connector **P2**, which maps 1:1 — position `07` is
P2.07, position `09` is P2.09. The GPIO_P2 header carries **no ground pin**;
take ground from the P4 test points or pins 2/4 of P3, P5 or P7.
