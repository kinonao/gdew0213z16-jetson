# GDEW0213Z16 on Jetson Orin NX

Linux spidev + libgpiod C++17 sample for the 104x212 black/white/red GDEW0213Z16.

## Wiring

The display and all logic signals must be 3.3 V.

- EPD VCC -> Jetson 3.3 V (or a suitable regulated 3.3 V supply)
- EPD GND -> Jetson GND
- EPD DIN -> SPI MOSI
- EPD CLK -> SPI SCLK
- EPD CS  -> SPI CS for the selected `/dev/spidevX.Y`
- EPD DC  -> free GPIO output
- EPD RST -> free GPIO output
- EPD BUSY -> free GPIO input

Use GPIO **line offsets**, not 40-pin header pin numbers. Find them with `gpioinfo`.

Tested Jetson 40-pin header connections:

- Pin 13: EPD DC (GPIO line offset 122)
- Pin 16: EPD RESET (GPIO line offset 126)
- Pin 18: EPD BUSY (GPIO line offset 125)

## Build

```bash
sudo apt update
sudo apt install -y build-essential cmake libgpiod-dev gpiod
cmake -S . -B build
cmake --build build -j
```

## Run

Example only; replace line offsets and SPI device with your actual values:

```bash
sudo ./build/gdew0213z16 \
  --spi /dev/spidev0.0 \
  --chip gpiochip0 \
  --dc 122 --rst 126 --busy 125
```

BUSY defaults to active-low, as required by the GDEW0213Z16. If your adapter
inverts the BUSY signal, use:

```bash
sudo ./build/gdew0213z16 ... --busy-active-high
```

Start at 2 MHz. If wiring is short and stable, higher rates may work; if the display does not refresh, first try `--speed 500000`.

## Notes

- Resolution is fixed at 104x212.
- Black and red are separate 2756-byte planes.
- The sample performs a full refresh and then enters deep sleep.
- To refresh again after deep sleep, hardware reset is required; the program already does this on every run.
- Do not connect 5 V logic to the panel.
