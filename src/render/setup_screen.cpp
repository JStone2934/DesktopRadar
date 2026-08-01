#include "setup_screen.h"

#include <qrcode.h>
#include <stdio.h>

#include "config.h"

void setupScreenDraw(LGFX* lcd, int remainSec) {
  if (!lcd) {
    return;
  }

  lcd->fillScreen(TFT_BLACK);
  lcd->setTextColor(TFT_WHITE, TFT_BLACK);
  lcd->setTextDatum(TC_DATUM);

  lcd->setFont(&fonts::Font2);
  char line[40];
  snprintf(line, sizeof(line), "WiFi: %s", SOFTAP_SSID);
  lcd->drawString(line, LCD_WIDTH / 2, 14);

  lcd->drawString(CONFIG_PORTAL_URL, LCD_WIDTH / 2, 32);
  lcd->drawString("先连WiFi再打开网址", LCD_WIDTH / 2, 48);

  if (remainSec >= 0) {
    snprintf(line, sizeof(line), "%ds", remainSec);
    lcd->setTextDatum(BC_DATUM);
    lcd->drawString(line, LCD_WIDTH / 2, LCD_HEIGHT - 8);
    lcd->setTextDatum(TC_DATUM);
  }

  QRCode qrcode;
  uint8_t qrbuf[296];  // version 3 ECC_LOW enough for short URL
  const int ok =
      qrcode_initText(&qrcode, qrbuf, 3, ECC_LOW, CONFIG_PORTAL_URL);
  if (ok != 0) {
    lcd->setTextDatum(MC_DATUM);
    lcd->drawString("QR fail", LCD_WIDTH / 2, LCD_HEIGHT / 2);
    return;
  }

  const int quiet = 2;
  const int modules = qrcode.size + quiet * 2;
  const int maxSide = 140;
  int scale = maxSide / modules;
  if (scale < 2) {
    scale = 2;
  }
  const int side = modules * scale;
  const int ox = (LCD_WIDTH - side) / 2;
  const int oy = 62;

  lcd->fillRect(ox, oy, side, side, TFT_WHITE);
  for (uint8_t y = 0; y < qrcode.size; y++) {
    for (uint8_t x = 0; x < qrcode.size; x++) {
      if (qrcode_getModule(&qrcode, x, y)) {
        lcd->fillRect(ox + (x + quiet) * scale, oy + (y + quiet) * scale,
                      scale, scale, TFT_BLACK);
      }
    }
  }
}
