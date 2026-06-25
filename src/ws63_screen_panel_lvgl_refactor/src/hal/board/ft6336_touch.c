/**
 * @file ft6336_touch.c
 * @brief FT6336U capacitive touch driver with I2C diagnostics.
 */
#include "ft6336_touch.h"
#include "screen_board.h"
#include "screen_config.h"
#include "soc_osal.h"

#include <string.h>

#define FT_REG_NUM_FINGER       0x02
#define FT_TP1_REG              0x03
#define FT_TP2_REG              0x09
#define FT_ID_G_CIPHER_MID      0x9F
#define FT_ID_G_CIPHER_LOW      0xA0
#define FT_ID_G_CIPHER_HIGH     0xA3
#define FT_ID_G_FOCALTECH_ID    0xA8

static errcode_t ft6336_read_reg(uint8_t reg, uint8_t *buf, uint32_t len)
{
#if SCREEN_BOARD_REV_FINAL_HW_I2C || SCREEN_BOARD_REV_FLYWIRE_HW_I2C
    return screen_hw_i2c_read_reg(SCREEN_TOUCH_I2C_ADDR, reg, buf, len);
#else
    return screen_touch_i2c_read(reg, buf, len);
#endif
}

errcode_t ft6336_read_ids(uint8_t *vendor, uint8_t *cipher_mid, uint8_t *cipher_low, uint8_t *cipher_high)
{
    uint8_t tmp[2] = {0};
    errcode_t ret;

    if (vendor != NULL) {
        ret = ft6336_read_reg(FT_ID_G_FOCALTECH_ID, vendor, 1);
        if (ret != ERRCODE_SUCC) {
            return ret;
        }
    }

    ret = ft6336_read_reg(FT_ID_G_CIPHER_MID, tmp, sizeof(tmp));
    if (ret != ERRCODE_SUCC) {
        return ret;
    }
    if (cipher_mid != NULL) {
        *cipher_mid = tmp[0];
    }
    if (cipher_low != NULL) {
        *cipher_low = tmp[1];
    }

    if (cipher_high != NULL) {
        ret = ft6336_read_reg(FT_ID_G_CIPHER_HIGH, cipher_high, 1);
        if (ret != ERRCODE_SUCC) {
            return ret;
        }
    }

    return ERRCODE_SUCC;
}

errcode_t ft6336_init(void)
{
    /* Reset sequence: low 50ms, high 300ms */
    screen_touch_rst(false);
    screen_board_delay_ms(50);
    screen_touch_rst(true);
    screen_board_delay_ms(300);

    errcode_t ret;

#if SCREEN_BOARD_REV_FINAL_HW_I2C || SCREEN_BOARD_REV_FLYWIRE_HW_I2C
    ret = screen_hw_i2c_init();
    if (ret != ERRCODE_SUCC) {
        osal_printk("[TOUCH] hw i2c1 init FAILED (0x%x)\r\n", ret);
        return ret;
    }

    /* Probe by reading register 0x02 (TD_STATUS) */
    uint8_t probe_td = 0xFF;
    ret = screen_hw_i2c_read_reg(SCREEN_TOUCH_I2C_ADDR, FT_REG_NUM_FINGER,
                                 &probe_td, 1);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[TOUCH] probe 0x%02X FAILED (0x%x)\r\n",
                    SCREEN_TOUCH_I2C_ADDR, ret);
        return ERRCODE_FAIL;
    }

#else
    /* I2C bus scan */
    screen_i2c_scan();

    /* Probe target address */
    if (!screen_i2c_probe(SCREEN_TOUCH_I2C_ADDR)) {
        osal_printk("[TOUCH] ERROR: addr 0x%02X not responding!\r\n", SCREEN_TOUCH_I2C_ADDR);
        return ERRCODE_FAIL;
    }
#endif

    /* Read register 0x02 (touch count / TD_STATUS) */
    uint8_t td_status = 0xFF;
    ret = ft6336_read_reg(FT_REG_NUM_FINGER, &td_status, 1);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[TOUCH] reg 0x02 read FAILED (0x%x)\r\n", ret);
    }

    /* Read raw bytes from 0x03 (first touch point) */
    uint8_t raw[6] = {0};
    ret = ft6336_read_reg(FT_TP1_REG, raw, sizeof(raw));
    if (ret != ERRCODE_SUCC) {
        osal_printk("[TOUCH] raw bytes read FAILED (0x%x)\r\n", ret);
    }

    /* Read chip IDs */
    uint8_t vendor = 0;
    uint8_t mid = 0;
    uint8_t low = 0;
    uint8_t high = 0;
    ret = ft6336_read_ids(&vendor, &mid, &low, &high);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[TOUCH] read_ids FAILED (0x%x)\r\n", ret);
        return ret;
    }

    if (vendor != 0x11 || mid != 0x26 || high != 0x64) {
        osal_printk("[TOUCH] ID mismatch: expect vendor=0x11 mid=0x26 high=0x64\r\n");
        return ERRCODE_FAIL;
    }
    if (low != 0x00 && low != 0x01 && low != 0x02) {
        osal_printk("[TOUCH] cipher_low unexpected: 0x%02X\r\n", low);
        return ERRCODE_FAIL;
    }
    return ERRCODE_SUCC;
}

errcode_t ft6336_read_touch(ft6336_touch_data_t *touch)
{
    uint8_t count = 0;
    static const uint8_t point_reg[FT6336_MAX_POINTS] = {FT_TP1_REG, FT_TP2_REG};

    if (touch == NULL) {
        return ERRCODE_FAIL;
    }
    memset(touch, 0, sizeof(*touch));

    errcode_t ret = ft6336_read_reg(FT_REG_NUM_FINGER, &count, 1);
    if (ret != ERRCODE_SUCC) {
        return ret;
    }
    count &= 0x0F;
    if (count > FT6336_MAX_POINTS) {
        count = FT6336_MAX_POINTS;
    }
    touch->count = count;

    for (uint8_t i = 0; i < count; i++) {
        uint8_t buf[4] = {0};
        ret = ft6336_read_reg(point_reg[i], buf, sizeof(buf));
        if (ret != ERRCODE_SUCC) {
            return ret;
        }
        touch->point[i].event = (buf[0] >> 6) & 0x03;
        touch->point[i].x = ((uint16_t)(buf[0] & 0x0F) << 8) | buf[1];
        touch->point[i].id = (buf[2] >> 4) & 0x0F;
        touch->point[i].y = ((uint16_t)(buf[2] & 0x0F) << 8) | buf[3];
    }

    return ERRCODE_SUCC;
}
