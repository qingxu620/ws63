/**
 * @file ft6336_touch.c
 * @brief FT6336U capacitive touch driver.
 */
#include "ft6336_touch.h"
#include "screen_board.h"

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
    return screen_touch_i2c_read(reg, buf, len);
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
    screen_touch_rst(false);
    screen_board_delay_ms(10);
    screen_touch_rst(true);
    screen_board_delay_ms(500);

    uint8_t vendor = 0;
    uint8_t mid = 0;
    uint8_t low = 0;
    uint8_t high = 0;
    errcode_t ret = ft6336_read_ids(&vendor, &mid, &low, &high);
    if (ret != ERRCODE_SUCC) {
        return ret;
    }

    if (vendor != 0x11 || mid != 0x26 || high != 0x64) {
        return ERRCODE_FAIL;
    }
    if (low != 0x00 && low != 0x01 && low != 0x02) {
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
