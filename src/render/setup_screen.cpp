#include "setup_screen.h"

#include <stdio.h>

#include "config.h"
#include "radar_font.h"

static bool isPrintableAscii(const char* text) {
  if (!text || !text[0]) {
    return false;
  }
  for (const uint8_t* p = reinterpret_cast<const uint8_t*>(text); *p; ++p) {
    if (*p < 0x20 || *p > 0x7e) {
      return false;
    }
  }
  return true;
}

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

static void drawBootHint(LGFX* lcd) {
  lcd->setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  lcd->setTextDatum(TC_DATUM);
  lcd->setFont(&radar_fonts::cn12);
  lcd->setTextSize(1.1f);
  lcd->drawString("按下 S 键跳过", LCD_WIDTH / 2, 195);
}

void setupScreenDraw(LGFX* lcd, int remainSec) {
  if (!lcd) {
    return;
  }

  lcd->fillScreen(TFT_BLACK);
  lcd->setTextColor(TFT_WHITE, TFT_BLACK);
  lcd->setTextDatum(TC_DATUM);
  lcd->setFont(&radar_fonts::cn12);
  lcd->setTextColor(TFT_CYAN, TFT_BLACK);
  lcd->setTextSize(1.8f);

  lcd->drawString("设备配置", LCD_WIDTH / 2, 8);

  lcd->setTextColor(TFT_WHITE, TFT_BLACK);
  lcd->setTextSize(1.55f);
  lcd->drawString("1.手机连接热点", LCD_WIDTH / 2, 38);

  char line[48];
  snprintf(line, sizeof(line), "%s", SOFTAP_SSID);
  lcd->drawRoundRect(44, 59, LCD_WIDTH - 88, 28, 5, TFT_DARKGREY);
  lcd->setTextColor(TFT_YELLOW, TFT_BLACK);
  lcd->setTextSize(1.75f);
  lcd->drawString(line, LCD_WIDTH / 2, 62);

  snprintf(line, sizeof(line), "密码: %s", SOFTAP_PASS);
  lcd->setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  lcd->setTextSize(1.35f);
  lcd->drawString(line, LCD_WIDTH / 2, 91);

  lcd->drawFastHLine(28, 113, LCD_WIDTH - 56, TFT_DARKGREY);

  lcd->setTextColor(TFT_WHITE, TFT_BLACK);
  lcd->setTextSize(1.5f);
  lcd->drawString("2.打开手机浏览器", LCD_WIDTH / 2, 121);
  lcd->drawString("输入网址", LCD_WIDTH / 2, 145);

  lcd->drawRoundRect(25, 165, LCD_WIDTH - 50, 27, 5, TFT_DARKGREY);
  lcd->setTextColor(TFT_GREEN, TFT_BLACK);
  lcd->setTextSize(1.3f);
  lcd->drawString(CONFIG_PORTAL_URL, LCD_WIDTH / 2, 168);

  drawBootHint(lcd);
  drawStatusLine(lcd, remainSec);
}

void setupScreenUpdateStatus(LGFX* lcd, int remainSec) {
  if (!lcd) {
    return;
  }
  drawStatusLine(lcd, remainSec);
}

void setupScreenShowSaved(LGFX* lcd, const char* ssid) {
  if (!lcd) {
    return;
  }

  lcd->fillScreen(TFT_BLACK);
  lcd->setTextColor(TFT_WHITE, TFT_BLACK);
  lcd->setTextDatum(MC_DATUM);
  lcd->setFont(&radar_fonts::cn12);
  lcd->setTextSize(1.6f);
  lcd->drawString("配置已保存", LCD_WIDTH / 2, 92);

  lcd->setTextSize(1.25f);
  lcd->drawString("正在连接 WiFi", LCD_WIDTH / 2, 126);

  // 子集只覆盖 ASCII SSID。非 ASCII 名称仍完整保存和联网，但设备屏幕不
  // 尝试用缺字字体显示；上方两行固定中文已足够确认配置成功。
  if (isPrintableAscii(ssid)) {
    lcd->setTextSize(1.0f);
    lcd->drawString(ssid, LCD_WIDTH / 2, 154);
  }
}
