#include "setup_screen.h"

#include <math.h>
#include <qrcode.h>
#include <stdio.h>

#include "config.h"

static void drawStatusLine(LGFX* lcd, int remainSec) {
  // 底部仅刷新倒计时数字
  const int y0 = LCD_HEIGHT - 30;
  lcd->fillRect(70, y0, LCD_WIDTH - 140, 28, TFT_BLACK);
  lcd->setTextColor(TFT_WHITE, TFT_BLACK);
  lcd->setTextDatum(BC_DATUM);
  lcd->setTextSize(1.0f);
  if (remainSec >= 0) {
    lcd->setFont(&fonts::Font4);
    char line[12];
    snprintf(line, sizeof(line), "%d", remainSec);
    lcd->drawString(line, LCD_WIDTH / 2, LCD_HEIGHT - 4);
  }
}

/** 圆屏在 [y0,y1] 竖直带内，左右各需预留的安全边距。 */
static int circleInsetX(int y0, int y1, int pad) {
  const float cx = (LCD_WIDTH - 1) * 0.5f;
  const float cy = (LCD_HEIGHT - 1) * 0.5f;
  const float r = LCD_WIDTH * 0.5f - (float)pad;
  auto insetAt = [&](int y) -> int {
    const float dy = (float)y - cy;
    const float t = r * r - dy * dy;
    if (t <= 0.0f) {
      return LCD_WIDTH / 2;
    }
    const float half = sqrtf(t);
    const int left = (int)ceilf(cx - half);
    return left < 0 ? 0 : left;
  };
  const int a = insetAt(y0);
  const int b = insetAt(y1);
  return a > b ? a : b;
}

/** 二维码右侧：按下 BOOT 键跳过（竖排三行）。 */
static void drawBootHint(LGFX* lcd, int qrRight, int qrTop, int qrSide,
                         int rightLimit) {
  lcd->setTextColor(TFT_WHITE, TFT_BLACK);
  lcd->setTextDatum(TC_DATUM);
  lcd->setFont(&fonts::efontCN_12);
  lcd->setTextSize(1.0f);

  int tx = qrRight + (rightLimit - qrRight) / 2;
  if (tx <= qrRight + 8) {
    tx = qrRight + 20;
  }
  const int lineH = 16;
  const int blockH = lineH * 3;
  int ty = qrTop + (qrSide - blockH) / 2;
  if (ty < qrTop) {
    ty = qrTop;
  }

  lcd->drawString("按下", tx, ty);
  lcd->drawString("BOOT", tx, ty + lineH);
  lcd->drawString("键跳过", tx, ty + lineH * 2);
}

void setupScreenDraw(LGFX* lcd, int remainSec) {
  if (!lcd) {
    return;
  }

  lcd->fillScreen(TFT_BLACK);
  lcd->setTextColor(TFT_WHITE, TFT_BLACK);
  lcd->setTextDatum(TC_DATUM);
  lcd->setFont(&fonts::efontCN_12);
  lcd->setTextSize(1.5f);

  char line[48];
  int y = 6;

  lcd->drawString("1.先连接热点", LCD_WIDTH / 2, y);
  y += 22;
  snprintf(line, sizeof(line), "%s", SOFTAP_SSID);
  lcd->drawString(line, LCD_WIDTH / 2, y);
  y += 24;

  lcd->drawString("2.再扫码或手动打开", LCD_WIDTH / 2, y);
  y += 22;
  lcd->drawString(CONFIG_PORTAL_URL, LCD_WIDTH / 2, y);
  y += 18;

  lcd->setTextSize(1.0f);

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
  const int hintW = 52;
  const int pad = 6;
  const int bottomReserve = 32;
  const int topY = y;
  const int roomBottom = LCD_HEIGHT - bottomReserve;

  // 先按高度估一个边长，再按圆屏左右弦宽收紧，保证方块完整落在圆内
  int scale = (roomBottom - topY) / modules;
  if (scale > 3) {
    scale = 3;
  }
  if (scale < 2) {
    scale = 2;
  }

  int side = 0;
  int ox = 0;
  int oy = 0;
  int rightLimit = LCD_WIDTH;
  for (;;) {
    side = modules * scale;
    oy = topY + (roomBottom - topY - side) / 2;
    if (oy < topY) {
      oy = topY;
    }
    const int inset = circleInsetX(oy, oy + side - 1, pad);
    rightLimit = LCD_WIDTH - inset;
    const int availW = rightLimit - inset - hintW;
    if (availW >= side) {
      // 略靠左，右侧留给 BOOT 提示，但不贴圆边
      ox = inset;
      if (ox + side + hintW > rightLimit) {
        ox = rightLimit - hintW - side;
      }
      if (ox < inset) {
        ox = inset;
      }
      break;
    }
    if (scale <= 2) {
      // 最小 scale 仍超宽：再缩小无 quiet 不可行，只能裁 hint 或强制居中缩小
      side = availW > 0 ? (availW / modules) * modules : modules * 2;
      if (side < modules * 2) {
        side = modules * 2;
      }
      scale = side / modules;
      oy = topY + (roomBottom - topY - side) / 2;
      if (oy < topY) {
        oy = topY;
      }
      const int inset2 = circleInsetX(oy, oy + side - 1, pad);
      rightLimit = LCD_WIDTH - inset2;
      ox = inset2;
      break;
    }
    --scale;
  }

  lcd->fillRect(ox, oy, side, side, TFT_WHITE);
  for (uint8_t row = 0; row < qrcode.size; row++) {
    for (uint8_t col = 0; col < qrcode.size; col++) {
      if (qrcode_getModule(&qrcode, col, row)) {
        lcd->fillRect(ox + (col + quiet) * scale, oy + (row + quiet) * scale,
                      scale, scale, TFT_BLACK);
      }
    }
  }

  drawBootHint(lcd, ox + side, oy, side, rightLimit);
  drawStatusLine(lcd, remainSec);
}

void setupScreenUpdateStatus(LGFX* lcd, int remainSec) {
  if (!lcd) {
    return;
  }
  drawStatusLine(lcd, remainSec);
}
