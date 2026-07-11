#include <fcntl.h>
#include <gpiod.h>
#include <linux/spi/spidev.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

// image2.h was generated for Arduino, where PROGMEM is an AVR storage
// attribute.  Jetson can access the arrays directly from normal read-only RAM.
#ifndef PROGMEM
#define PROGMEM
#endif
#include "image_nvidia.h"

namespace {
constexpr int WIDTH = 104;
constexpr int HEIGHT = 212;
constexpr int ROW_BYTES = WIDTH / 8;
constexpr int FRAME_BYTES = ROW_BYTES * HEIGHT;

enum class Color { White, Black, Red };

void sleep_ms(int ms) {
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

class GpioLine {
public:
  GpioLine(gpiod_chip *chip, unsigned int offset, bool output, int initial,
           const char *name)
      : line_(gpiod_chip_get_line(chip, offset)) {
    if (!line_)
      throw std::runtime_error(std::string("gpiod_chip_get_line failed: ") +
                               name);
    int rc = output ? gpiod_line_request_output(line_, name, initial)
                    : gpiod_line_request_input(line_, name);
    if (rc < 0)
      throw std::runtime_error(std::string("gpiod_line_request failed: ") +
                               name);
  }
  ~GpioLine() {
    if (line_)
      gpiod_line_release(line_);
  }
  GpioLine(const GpioLine &) = delete;
  GpioLine &operator=(const GpioLine &) = delete;
  void set(int value) {
    if (gpiod_line_set_value(line_, value) < 0)
      throw std::runtime_error("GPIO write failed");
  }
  int get() const {
    const int v = gpiod_line_get_value(line_);
    if (v < 0)
      throw std::runtime_error("GPIO read failed");
    return v;
  }

private:
  gpiod_line *line_{};
};

class SpiDevice {
public:
  SpiDevice(const std::string &path, uint32_t speed_hz) {
    fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
    if (fd_ < 0)
      throw std::runtime_error("Cannot open " + path + ": " +
                               std::strerror(errno));
    uint8_t mode = SPI_MODE_0;
    uint8_t bits = 8;
    if (ioctl(fd_, SPI_IOC_WR_MODE, &mode) < 0 ||
        ioctl(fd_, SPI_IOC_WR_BITS_PER_WORD, &bits) < 0 ||
        ioctl(fd_, SPI_IOC_WR_MAX_SPEED_HZ, &speed_hz) < 0) {
      throw std::runtime_error("SPI configuration failed");
    }
    speed_hz_ = speed_hz;
  }
  ~SpiDevice() {
    if (fd_ >= 0)
      ::close(fd_);
  }
  void write(const uint8_t *data, size_t len) {
    spi_ioc_transfer tr{};
    tr.tx_buf = reinterpret_cast<uintptr_t>(data);
    tr.len = static_cast<uint32_t>(len);
    tr.speed_hz = speed_hz_;
    tr.bits_per_word = 8;
    if (ioctl(fd_, SPI_IOC_MESSAGE(1), &tr) < 0)
      throw std::runtime_error("SPI transfer failed");
  }
  void write(uint8_t value) { write(&value, 1); }

private:
  int fd_{-1};
  uint32_t speed_hz_{};
};

class Canvas {
public:
  Canvas() : black_(FRAME_BYTES, 0xFF), red_(FRAME_BYTES, 0xFF) {}
  void load(const unsigned char *black, const unsigned char *red) {
    // Both display planes use 1 for white and 0 for their ink color.
    // Begin with an explicitly white background, then overlay the image.
    clear(Color::White);
    for (int y = 0; y < HEIGHT; ++y) {
      for (int x = 0; x < WIDTH; ++x) {
        const size_t i = static_cast<size_t>(y) * ROW_BYTES + x / 8;
        const uint8_t mask = static_cast<uint8_t>(0x80u >> (x & 7));
        if ((black[i] & mask) == 0)
          pixel(x, y, Color::Black);
        else if ((red[i] & mask) == 0)
          pixel(x, y, Color::Red);
      }
    }
  }
  void clear(Color c = Color::White) {
    std::fill(black_.begin(), black_.end(), c == Color::Black ? 0x00 : 0xFF);
    std::fill(red_.begin(), red_.end(), c == Color::Red ? 0x00 : 0xFF);
  }
  void pixel(int x, int y, Color c) {
    if (x < 0 || y < 0 || x >= WIDTH || y >= HEIGHT)
      return;
    const size_t i = static_cast<size_t>(y) * ROW_BYTES + x / 8;
    const uint8_t mask = static_cast<uint8_t>(0x80u >> (x & 7));
    black_[i] |= mask;
    red_[i] |= mask;
    if (c == Color::Black)
      black_[i] &= static_cast<uint8_t>(~mask);
    if (c == Color::Red)
      red_[i] &= static_cast<uint8_t>(~mask);
  }
  void line(int x0, int y0, int x1, int y1, Color c) {
    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
      pixel(x0, y0, c);
      if (x0 == x1 && y0 == y1)
        break;
      int e2 = 2 * err;
      if (e2 >= dy) {
        err += dy;
        x0 += sx;
      }
      if (e2 <= dx) {
        err += dx;
        y0 += sy;
      }
    }
  }
  void rect(int x, int y, int w, int h, Color c, bool fill = false) {
    if (fill) {
      for (int yy = y; yy < y + h; ++yy)
        for (int xx = x; xx < x + w; ++xx)
          pixel(xx, yy, c);
    } else {
      line(x, y, x + w - 1, y, c);
      line(x, y + h - 1, x + w - 1, y + h - 1, c);
      line(x, y, x, y + h - 1, c);
      line(x + w - 1, y, x + w - 1, y + h - 1, c);
    }
  }
  void circle(int cx, int cy, int r, Color c) {
    int x = r, y = 0, err = 0;
    while (x >= y) {
      pixel(cx + x, cy + y, c);
      pixel(cx + y, cy + x, c);
      pixel(cx - y, cy + x, c);
      pixel(cx - x, cy + y, c);
      pixel(cx - x, cy - y, c);
      pixel(cx - y, cy - x, c);
      pixel(cx + y, cy - x, c);
      pixel(cx + x, cy - y, c);
      if (err <= 0) {
        ++y;
        err += 2 * y + 1;
      }
      if (err > 0) {
        --x;
        err -= 2 * x + 1;
      }
    }
  }
  // Compact 5x7 glyphs required by this demo only.
  void text(int x, int y, const std::string &s, Color c, int scale = 1) {
    for (char ch : s) {
      draw_char(x, y, ch, c, scale);
      x += 6 * scale;
    }
  }
  const std::vector<uint8_t> &black() const { return black_; }
  const std::vector<uint8_t> &red() const { return red_; }

private:
  static std::array<uint8_t, 5> glyph(char c) {
    switch (c) {
    case ' ':
      return {0, 0, 0, 0, 0};
    case '-':
      return {8, 8, 8, 8, 8};
    case '.':
      return {0, 0, 0, 0x60, 0x60};
    case '0':
      return {0x3E, 0x51, 0x49, 0x45, 0x3E};
    case '1':
      return {0, 0x42, 0x7F, 0x40, 0};
    case '2':
      return {0x42, 0x61, 0x51, 0x49, 0x46};
    case '3':
      return {0x21, 0x41, 0x45, 0x4B, 0x31};
    case '4':
      return {0x18, 0x14, 0x12, 0x7F, 0x10};
    case '5':
      return {0x27, 0x45, 0x45, 0x45, 0x39};
    case '6':
      return {0x3C, 0x4A, 0x49, 0x49, 0x30};
    case '7':
      return {1, 0x71, 9, 5, 3};
    case '8':
      return {0x36, 0x49, 0x49, 0x49, 0x36};
    case '9':
      return {6, 0x49, 0x49, 0x29, 0x1E};
    case 'A':
      return {0x7E, 0x11, 0x11, 0x11, 0x7E};
    case 'D':
      return {0x7F, 0x41, 0x41, 0x22, 0x1C};
    case 'E':
      return {0x7F, 0x49, 0x49, 0x49, 0x41};
    case 'I':
      return {0x41, 0x41, 0x7F, 0x41, 0x41};
    case 'J':
      return {0x20, 0x40, 0x41, 0x3F, 1};
    case 'N':
      return {0x7F, 2, 4, 8, 0x7F};
    case 'O':
      return {0x3E, 0x41, 0x41, 0x41, 0x3E};
    case 'P':
      return {0x7F, 9, 9, 9, 6};
    case 'R':
      return {0x7F, 9, 0x19, 0x29, 0x46};
    case 'S':
      return {0x46, 0x49, 0x49, 0x49, 0x31};
    case 'T':
      return {1, 1, 0x7F, 1, 1};
    case 'X':
      return {0x63, 0x14, 8, 0x14, 0x63};
    default:
      return {0x7F, 0x41, 0x5D, 0x41, 0x7F};
    }
  }
  void draw_char(int x, int y, char c, Color color, int scale) {
    auto g = glyph(c);
    for (int col = 0; col < 5; ++col)
      for (int row = 0; row < 7; ++row)
        if (g[col] & (1u << row))
          rect(x + col * scale, y + row * scale, scale, scale, color, true);
  }
  std::vector<uint8_t> black_, red_;
};

class Epaper {
public:
  Epaper(SpiDevice &spi, GpioLine &dc, GpioLine &rst, GpioLine &busy,
         bool busy_active_high)
      : spi_(spi), dc_(dc), rst_(rst), busy_(busy),
        busy_active_high_(busy_active_high) {}
  void display(const Canvas &c) {
    reset();
    command(0x06, {0x17, 0x17, 0x17}); // booster soft start
    command(0x00, {0x8F});             // panel setting for GDEW0213Z16
    command(0x61, {0x68, 0x00, 0xD4}); // 104 x 212
    command(0x50, {0x77});             // VCOM/data interval for B/W/R
    command(0x04);
    wait_ready(10000); // power on after panel configuration
    command(0x10);
    data(c.black());
    command(0x13);
    data(c.red());
    command(0x12);
    wait_ready(30000); // display refresh
    command(0x02);
    wait_ready(10000);     // power off
    command(0x07, {0xA5}); // deep sleep
  }

private:
  void reset() {
    rst_.set(1);
    sleep_ms(20);
    rst_.set(0);
    sleep_ms(10);
    rst_.set(1);
    sleep_ms(20);
    wait_ready(5000);
  }
  void wait_ready(int timeout_ms) {
    const auto end = std::chrono::steady_clock::now() +
                     std::chrono::milliseconds(timeout_ms);
    while ((busy_.get() != 0) == busy_active_high_) {
      if (std::chrono::steady_clock::now() >= end)
        throw std::runtime_error("BUSY timeout; check polarity/wiring");
      sleep_ms(10);
    }
  }
  void command(uint8_t cmd, std::initializer_list<uint8_t> args = {}) {
    dc_.set(0);
    spi_.write(cmd);
    if (!args.size())
      return;
    dc_.set(1);
    for (uint8_t v : args)
      spi_.write(v);
  }
  void data(const std::vector<uint8_t> &v) {
    dc_.set(1);
    spi_.write(v.data(), v.size());
  }
  SpiDevice &spi_;
  GpioLine &dc_;
  GpioLine &rst_;
  GpioLine &busy_;
  bool busy_active_high_;
};

struct Args {
  std::string spi = "/dev/spidev0.0", chip = "gpiochip0";
  unsigned dc = 0, rst = 0, busy = 0;
  uint32_t speed = 2000000;
  bool busy_high = false;
  bool dc_set = false, rst_set = false, busy_set = false;
};
Args parse(int argc, char **argv) {
  Args a;
  for (int i = 1; i < argc; ++i) {
    std::string k = argv[i];
    auto val = [&]() {
      if (++i >= argc)
        throw std::runtime_error("missing value for " + k);
      return std::string(argv[i]);
    };
    if (k == "--spi")
      a.spi = val();
    else if (k == "--chip")
      a.chip = val();
    else if (k == "--dc") {
      a.dc = std::stoul(val());
      a.dc_set = true;
    } else if (k == "--rst") {
      a.rst = std::stoul(val());
      a.rst_set = true;
    } else if (k == "--busy") {
      a.busy = std::stoul(val());
      a.busy_set = true;
    } else if (k == "--speed")
      a.speed = std::stoul(val());
    else if (k == "--busy-active-low")
      a.busy_high = false;
    else if (k == "--busy-active-high")
      a.busy_high = true;
    else if (k == "--help") {
      std::cout << "Usage: sudo ./gdew0213z16 --spi /dev/spidev0.0 --chip "
                   "gpiochip0 --dc N --rst N --busy N [--speed 2000000] "
                   "[--busy-active-high]\nBUSY defaults to active-low for the "
                   "GDEW0213Z16.\n";
      std::exit(0);
    } else
      throw std::runtime_error("unknown option: " + k);
  }
  if (!a.dc_set || !a.rst_set || !a.busy_set)
    throw std::runtime_error(
        "Specify all GPIO line offsets with --dc, --rst and --busy");
  return a;
}
} // namespace

int main(int argc, char **argv) {
  try {
    static_assert(sizeof(epd_bitmap_image3B) == FRAME_BYTES,
                  "image2.h black bitmap must be 104x212 pixels");
    static_assert(sizeof(epd_bitmap_image3R) == FRAME_BYTES,
                  "image2.h red bitmap must be 104x212 pixels");
    const Args a = parse(argc, argv);
    gpiod_chip *chip = gpiod_chip_open_by_name(a.chip.c_str());
    if (!chip)
      throw std::runtime_error("Cannot open " + a.chip);
    try {
      GpioLine dc(chip, a.dc, true, 0, "epd-dc");
      GpioLine rst(chip, a.rst, true, 1, "epd-rst");
      GpioLine busy(chip, a.busy, false, 0, "epd-busy");
      SpiDevice spi(a.spi, a.speed);
      Canvas cv;
      cv.load(epd_bitmap_image3B, epd_bitmap_image3R);
      Epaper epd(spi, dc, rst, busy, a.busy_high);
      std::cout << "Refreshing e-paper...\n";
      epd.display(cv);
      std::cout << "Done. Display is now in deep sleep.\n";
    } catch (...) {
      gpiod_chip_close(chip);
      throw;
    }
    gpiod_chip_close(chip);
    return 0;
  } catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
  }
}
