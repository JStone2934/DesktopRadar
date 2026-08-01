#include "setup_screen.h"

#include <qrcode.h>
#include <stdio.h>

#include "config.h"

static void drawStatusLine(LGFX* lcd, int remainSec) {
  // 圆屏底部一条状态区
  const int y0 = LCD_HEIGHT - 22;
  lcd->fillRect(20, y0, LCD_WIDTH - 40, 20, TFT_BLACK);
  lcd->setTextColor(TFT_WHITE, TFT_BLACK);
  lcd->setTextDatum(BC_DATUM);
  lcd->setFont(&fonts::efontCN_12);
  char line[48];
  if (remainSec >= 0) {
    snprintf(line, sizeof(line), "%ds后跳过 / BOOT跳过", remainSec);
    lcd->drawString(line, LCD_WIDTH / 2, LCD_HEIGHT - 4);
  } else {
    lcd->drawString("等待保存设置...", LCD_WIDTH / 2, LCD_HEIGHT - 4);
  }
}

void setupScreenDraw(LGFX* lcd, int remainSec) {
  if (!lcd) {
    return;
  }

  lcd->fillScreen(TFT_BLACK);
  lcd->setTextColor(TFT_WHITE, TFT_BLACK);
  lcd->setTextDatum(TC_DATUM);
  lcd->setFont(&fonts::efontCN_12);

  char line[48];
  int y = 8;

  lcd->drawString("1.先连接热点", LCD_WIDTH / 2, y);
  y += 16;
  snprintf(line, sizeof(line), "%s", SOFTAP_SSID);
  lcd->drawString(line, LCD_WIDTH / 2, y);
  y += 18;

  lcd->drawString("2.再扫码或手动打开", LCD_WIDTH / 2, y);
  y += 16;
  lcd->drawString(CONFIG_PORTAL_URL, LCD_WIDTH / 2, y);
  y += 18;

  QRCode qrcode;
  uint8_t qrbuf[296];
  const int ok =
      qrcode_initText(&qrcode, qrbuf, 3, ECC_LOW, CONFIG_PORTAL_URL);
  if (ok != 0) {
    lcd->setTextDatum(MC_DATUM);
    lcd->drawString("QR fail", LCD_WIDTH / 2, LCD_HEIGHT / 2);
    drawStatusLine(lcd, remainSec);
    return;
  }

  const int quiet = 1;
  const int modules = qrcode.size + quiet * 2;
  const int maxSide = 80;
  int scale = maxSide / modules;
  if (scale < 2) {
    scale = 2;
  }
  const int side = modules * scale;
  const int ox = (LCD_WIDTH - side) / 2;
  const int oy = y;

  lcd->fillRect(ox, oy, side, side, TFT_WHITE);
  for (uint8_t row = 0; row < qrcode.size; row++) {
    for (uint8_t col = 0; col < qrcode.size; col++) {
      if (qrcode_getModule(&qrcode, col, row)) {
        lcd->fillRect(ox + (col + quiet) * scale, oy + (row + quiet) * scale,
                      scale, scale, TFT_BLACK);
      }
    }
  }

  drawStatusLine(lcd, remainSec);
}

void setupScreenUpdateStatus(LGFX* lcd, int remainSec) {
  if (!lcd) {
    return;
  }
  drawStatusLine(lcd, remainSec);
}
