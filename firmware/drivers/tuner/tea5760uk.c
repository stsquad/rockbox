/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 * Tuner "middleware" for Philips TEA5760UK chip
 *
 * Copyright (C) 2004 Jörg Hohensohn
 *
 * All files in this archive are subject to the GNU General Public License.
 * See the file COPYING in the source tree root for full license agreement.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include "config.h"
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include "kernel.h"
#include "tuner.h" /* tuner abstraction interface */
#include "fmradio.h"
#include "fmradio_i2c.h" /* physical interface driver */

#define I2C_ADR 0xC0
static unsigned char write_bytes[7] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

static void tea5760uk_set_clear(int byte, unsigned char bits, int set)
{
    write_bytes[byte] &= ~bits;
    if (set)
        write_bytes[byte] |= bits;
}

/* tuner abstraction layer: set something to the tuner */
int tea5760uk_set(int setting, int value)
{
    switch(setting)
    {
        case RADIO_SLEEP:
            /* init values */
            write_bytes[0] |= (1<<7); /* mute */
#if CONFIG_TUNER_XTAL == 32768
            /* 32.768kHz, soft mute, stereo noise cancelling */
            write_bytes[3] |= (1<<4) | (1<<3) | (1<<1);
#else
            /* soft mute, stereo noise cancelling */
            write_bytes[3] |= (1<<3) | (1<<1); 
#endif
            /* sleep / standby mode */
            tea5760uk_set_clear(3, (1<<6), value);
            break;

        case RADIO_FREQUENCY:
            {
                int n;
#if CONFIG_TUNER_XTAL == 32768
                n = (4 * (value - 225000) + 16384) / 32768;
#else
                n = (4 * (value - 225000)) / 50000;
#endif
                write_bytes[6] = (write_bytes[6] & 0xC0) | (n >> 8);
                write_bytes[7] = n;
            }
            break;

        case RADIO_SCAN_FREQUENCY:
            tea5760uk_set(RADIO_FREQUENCY, value);
            sleep(HZ/30);
            return tea5760uk_get(RADIO_TUNED);

        case RADIO_MUTE:
            tea5760uk_set_clear(3, (1<<2), value);
            break;

        case RADIO_REGION:
        {
            const struct tea5760uk_region_data *rd =
                &tea5760uk_region_data[value];

            tea5760uk_set_clear(4, (1<<1), rd->deemphasis);
            tea5760uk_set_clear(3, (1<<5), rd->band);
            break;
            }
        case RADIO_FORCE_MONO:
            tea5760uk_set_clear(4, (1<<3), value);
            break;
        default:
            return -1;
    }

    fmradio_i2c_write(I2C_ADR, write_bytes, sizeof(write_bytes));
    return 1;
}

/* tuner abstraction layer: read something from the tuner */
int tea5760uk_get(int setting)
{
    unsigned char read_bytes[16];
    int val = -1; /* default for unsupported query */

    fmradio_i2c_read(I2C_ADR, read_bytes, sizeof(read_bytes));

    switch(setting)
    {
        case RADIO_PRESENT:
            val = 1; /* true */
            break;

        case RADIO_TUNED:
            val = 0;
            if (read_bytes[0] & (1<<4)) /* IF count correct */
            {
                val = read_bytes[8] >> 1; /* IF counter */
                val = (abs(val - 0x36) < 2); /* close match */
            }
            break;

        case RADIO_STEREO:
            val = read_bytes[9] >> 2;
            break;
    }

    return val;
}

void tea5760uk_dbg_info(struct tea5760uk_dbg_info *info)
{
    fmradio_i2c_read(I2C_ADR, info->read_regs, 5);
    memcpy(info->write_regs, write_bytes, 5);
}