#pragma once

/**
 * GC9A01 软件 SPI（DesktopRadar 初始化序列）
 * 引脚可运行时指定，便于轮询接线组合。
 */

#include <Arduino.h>

struct GC9A01_Pins {
  int rst;
  int cs;
  int dc;
  int mosi;
  int sclk;
  bool cs_active_high;  // 极少数模块 CS 极性相反
  const char* name;
};

class GC9A01_BB {
 public:
  static constexpr int W = 240;
  static constexpr int H = 240;

  void begin(const GC9A01_Pins& p) {
    pins_ = p;
    pinMode(pins_.rst, OUTPUT);
    pinMode(pins_.cs, OUTPUT);
    pinMode(pins_.dc, OUTPUT);
    pinMode(pins_.mosi, OUTPUT);
    pinMode(pins_.sclk, OUTPUT);

    cs(false);
    digitalWrite(pins_.dc, HIGH);
    digitalWrite(pins_.sclk, LOW);
    digitalWrite(pins_.mosi, LOW);

    hardReset();
    initDisplay();
  }

  void fill(uint16_t color565) {
    setWindow(0, 0, W - 1, H - 1);
    digitalWrite(pins_.dc, HIGH);
    cs(true);
    const uint8_t hi = color565 >> 8;
    const uint8_t lo = color565 & 0xFF;
    for (int i = 0; i < W * H; ++i) {
      write8(hi);
      write8(lo);
    }
    cs(false);
  }

  void fillRect(int x, int y, int w, int h, uint16_t color565) {
    if (w <= 0 || h <= 0) return;
    setWindow(x, y, x + w - 1, y + h - 1);
    digitalWrite(pins_.dc, HIGH);
    cs(true);
    const uint8_t hi = color565 >> 8;
    const uint8_t lo = color565 & 0xFF;
    const int n = w * h;
    for (int i = 0; i < n; ++i) {
      write8(hi);
      write8(lo);
    }
    cs(false);
  }

  static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
  }

 private:
  GC9A01_Pins pins_{};

  void cs(bool active) {
    const bool level = pins_.cs_active_high ? active : !active;
    digitalWrite(pins_.cs, level ? HIGH : LOW);
  }

  void write8(uint8_t v) {
    // Mode0；digitalWrite 本身已够慢，适合飞线
    for (int i = 7; i >= 0; --i) {
      digitalWrite(pins_.mosi, (v >> i) & 1);
      digitalWrite(pins_.sclk, HIGH);
      digitalWrite(pins_.sclk, LOW);
    }
  }

  void dataRaw(const uint8_t* d, size_t n) {
    digitalWrite(pins_.dc, HIGH);
    cs(true);
    for (size_t i = 0; i < n; ++i) write8(d[i]);
    cs(false);
  }

  void cmd(uint8_t c) {
    digitalWrite(pins_.dc, LOW);
    cs(true);
    write8(c);
    cs(false);
  }

  void data(const uint8_t* d, size_t n) {
    if (n) dataRaw(d, n);
  }

  void hardReset() {
    digitalWrite(pins_.rst, HIGH);
    delay(50);
    digitalWrite(pins_.rst, LOW);
    delay(50);
    digitalWrite(pins_.rst, HIGH);
    delay(150);
  }

  void setWindow(int x0, int y0, int x1, int y1) {
    cmd(0x2A);
    uint8_t xa[] = {
        (uint8_t)(x0 >> 8), (uint8_t)(x0 & 0xFF),
        (uint8_t)(x1 >> 8), (uint8_t)(x1 & 0xFF)};
    data(xa, 4);
    cmd(0x2B);
    uint8_t ya[] = {
        (uint8_t)(y0 >> 8), (uint8_t)(y0 & 0xFF),
        (uint8_t)(y1 >> 8), (uint8_t)(y1 & 0xFF)};
    data(ya, 4);
    cmd(0x2C);
  }

  void initDisplay() {
    struct Item {
      uint8_t c;
      const uint8_t* d;
      uint8_t n;
    };

    static const uint8_t d_eb[] = {0x14};
    static const uint8_t d_84[] = {0x40};
    static const uint8_t d_85[] = {0xFF};
    static const uint8_t d_86[] = {0xFF};
    static const uint8_t d_87[] = {0xFF};
    static const uint8_t d_88[] = {0x0A};
    static const uint8_t d_89[] = {0x21};
    static const uint8_t d_8a[] = {0x00};
    static const uint8_t d_8b[] = {0x80};
    static const uint8_t d_8c[] = {0x01};
    static const uint8_t d_8d[] = {0x01};
    static const uint8_t d_8e[] = {0xFF};
    static const uint8_t d_8f[] = {0xFF};
    static const uint8_t d_b6[] = {0x00, 0x20};
    static const uint8_t d_36[] = {0x08};
    static const uint8_t d_3a[] = {0x05};
    static const uint8_t d_90[] = {0x08, 0x08, 0x08, 0x08};
    static const uint8_t d_bd[] = {0x06};
    static const uint8_t d_bc[] = {0x00};
    static const uint8_t d_ff[] = {0x60, 0x01, 0x04};
    static const uint8_t d_c3[] = {0x13};
    static const uint8_t d_c4[] = {0x13};
    static const uint8_t d_c9[] = {0x22};
    static const uint8_t d_be[] = {0x11};
    static const uint8_t d_e1[] = {0x10, 0x0E};
    static const uint8_t d_df[] = {0x21, 0x0C, 0x02};
    static const uint8_t d_f0[] = {0x45, 0x09, 0x08, 0x08, 0x26, 0x2A};
    static const uint8_t d_f1[] = {0x43, 0x70, 0x72, 0x36, 0x37, 0x6F};
    static const uint8_t d_f2[] = {0x45, 0x09, 0x08, 0x08, 0x26, 0x2A};
    static const uint8_t d_f3[] = {0x43, 0x70, 0x72, 0x36, 0x37, 0x6F};
    static const uint8_t d_ed[] = {0x1B, 0x0B};
    static const uint8_t d_ae[] = {0x77};
    static const uint8_t d_cd[] = {0x63};
    static const uint8_t d_70[] = {0x07, 0x07, 0x04, 0x0E, 0x0F, 0x09, 0x07, 0x08, 0x03};
    static const uint8_t d_e8[] = {0x34};
    static const uint8_t d_62[] = {0x18, 0x0D, 0x71, 0xED, 0x70, 0x70,
                                   0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70};
    static const uint8_t d_63[] = {0x18, 0x11, 0x71, 0xF1, 0x70, 0x70,
                                   0x18, 0x13, 0x71, 0xF3, 0x70, 0x70};
    static const uint8_t d_64[] = {0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07};
    static const uint8_t d_66[] = {0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45,
                                   0x10, 0x00, 0x00, 0x00};
    static const uint8_t d_67[] = {0x00, 0x3C, 0x00, 0x00, 0x00, 0x01,
                                   0x54, 0x10, 0x32, 0x98};
    static const uint8_t d_74[] = {0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00};
    static const uint8_t d_98[] = {0x3E, 0x07};

    const Item seq[] = {
        {0xEF, nullptr, 0}, {0xEB, d_eb, 1}, {0xFE, nullptr, 0},
        {0xEF, nullptr, 0}, {0xEB, d_eb, 1}, {0x84, d_84, 1},
        {0x85, d_85, 1},    {0x86, d_86, 1}, {0x87, d_87, 1},
        {0x88, d_88, 1},    {0x89, d_89, 1}, {0x8A, d_8a, 1},
        {0x8B, d_8b, 1},    {0x8C, d_8c, 1}, {0x8D, d_8d, 1},
        {0x8E, d_8e, 1},    {0x8F, d_8f, 1}, {0xB6, d_b6, 2},
        {0x36, d_36, 1},    {0x3A, d_3a, 1}, {0x90, d_90, 4},
        {0xBD, d_bd, 1},    {0xBC, d_bc, 1}, {0xFF, d_ff, 3},
        {0xC3, d_c3, 1},    {0xC4, d_c4, 1}, {0xC9, d_c9, 1},
        {0xBE, d_be, 1},    {0xE1, d_e1, 2}, {0xDF, d_df, 3},
        {0xF0, d_f0, 6},    {0xF1, d_f1, 6}, {0xF2, d_f2, 6},
        {0xF3, d_f3, 6},    {0xED, d_ed, 2}, {0xAE, d_ae, 1},
        {0xCD, d_cd, 1},    {0x70, d_70, 9}, {0xE8, d_e8, 1},
        {0x62, d_62, 12},   {0x63, d_63, 12}, {0x64, d_64, 7},
        {0x66, d_66, 10},   {0x67, d_67, 10}, {0x74, d_74, 7},
        {0x98, d_98, 2},    {0x35, nullptr, 0}, {0x21, nullptr, 0},
        {0x11, nullptr, 0},
    };

    for (const auto& it : seq) {
      cmd(it.c);
      if (it.n) data(it.d, it.n);
    }
    delay(120);
    cmd(0x29);
    delay(20);
  }
};
