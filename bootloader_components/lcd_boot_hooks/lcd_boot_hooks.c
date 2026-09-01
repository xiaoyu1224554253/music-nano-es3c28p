#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "esp_rom_sys.h"
#include "soc/soc.h"
#include "soc/gpio_reg.h"

#define PIN_PERI_PWR   22   /* 外设供电: 低=开 */
#define PIN_LCD_RST    33   /* LCD 复位: 低有效 */

void bootloader_hooks_include(void) {}

void bootloader_after_init(void)
{
    esp_rom_gpio_pad_select_gpio(PIN_PERI_PWR);
    REG_SET_BIT(GPIO_ENABLE_REG, BIT(PIN_PERI_PWR));
    REG_SET_BIT(GPIO_OUT_W1TC_REG, BIT(PIN_PERI_PWR));
    esp_rom_delay_us(10 * 1000);

    esp_rom_gpio_pad_select_gpio(PIN_LCD_RST);
    REG_SET_BIT(GPIO_ENABLE1_REG, BIT(PIN_LCD_RST - 32));
    REG_SET_BIT(GPIO_OUT1_W1TC_REG, BIT(PIN_LCD_RST - 32));
    esp_rom_delay_us(10 * 1000);
    REG_SET_BIT(GPIO_OUT1_W1TS_REG, BIT(PIN_LCD_RST - 32));
    ESP_LOGI("HOOK", "RST released");
}
