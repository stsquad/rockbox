/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2008 by Michael Sevakis
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include "config.h"
#include "system.h"
#include "string.h"
#include "button.h"
#include "lcd.h"
#include "sprintf.h"
#include "font.h"
#include "debug-target.h"
#include "mc13783.h"
#include "adc.h"
#include "ccm-imx31.h"

bool __dbg_hw_info(void)
{
    int line;
    unsigned int pllref;
    unsigned int mcu_pllfreq, ser_pllfreq, usb_pllfreq;
    uint32_t mpctl, spctl, upctl;
    unsigned int freq;
    uint32_t regval;

    lcd_clear_display();
    lcd_setfont(FONT_SYSFIXED);

    while (1)
    {
        line = 0;
        lcd_putsf(0, line++, "Sys Rev Code: 0x%02X", iim_system_rev());
        line++;

        mpctl = CCM_MPCTL;
        spctl = CCM_SPCTL;
        upctl = CCM_UPCTL;

        pllref = ccm_get_pll_ref_clk();

        mcu_pllfreq = ccm_get_pll(PLL_MCU);
        ser_pllfreq = ccm_get_pll(PLL_SERIAL);
        usb_pllfreq = ccm_get_pll(PLL_USB);

        lcd_putsf(0, line++, "pll_ref_clk: %u", pllref);
        line++;

        /* MCU clock domain */
        lcd_putsf(0, line++, "MPCTL: %08lX", mpctl);

        lcd_putsf(0, line++, " mpl_dpdgck_clk: %u", mcu_pllfreq);
        line++;

        regval = CCM_PDR0;
        lcd_putsf(0, line++, "  PDR0: %08lX", regval);

        freq = mcu_pllfreq / (((regval & 0x7) + 1));
        lcd_putsf(0, line++, "   mcu_clk:      %u", freq);

        freq = mcu_pllfreq / (((regval >> 11) & 0x7) + 1);
        lcd_putsf(0, line++, "   hsp_clk:      %u", freq);

        freq = mcu_pllfreq / (((regval >> 3) & 0x7) + 1);
        lcd_putsf(0, line++, "   hclk_clk:     %u", freq);

        lcd_putsf(0, line++, "   ipg_clk:      %u",
            freq / (unsigned)(((regval >> 6) & 0x3) + 1));

        lcd_putsf(0, line++, "   nfc_clk:      %u",
            freq / (unsigned)(((regval >> 8) & 0x7) + 1));

        line++;

        /* Serial clock domain */
        lcd_putsf(0, line++, "SPCTL: %08lX", spctl);
        lcd_putsf(0, line++, " spl_dpdgck_clk: %u", ser_pllfreq);

        line++;

        /* USB clock domain */
        lcd_putsf(0, line++, "UPCTL: %08lX", upctl);

        lcd_putsf(0, line++, " upl_dpdgck_clk: %u", usb_pllfreq);

        regval = CCM_PDR1;
        lcd_putsf(0, line++, "  PDR1: %08lX", regval);

        freq = usb_pllfreq /
            ((((regval >> 30) & 0x3) + 1) * (((regval >> 27) & 0x7) + 1));
        lcd_putsf(0, line++, "   usb_clk:       %u", freq);

        freq = usb_pllfreq / (((CCM_PDR0 >> 16) & 0x1f) + 1);
        lcd_putsf(0, line++, "   ipg_per_baud:  %u", freq);
          
        lcd_update();

        if (button_get(true) == (DEBUG_CANCEL|BUTTON_REL))
            return false;
    }
}

bool __dbg_ports(void)
{
    int line;
    int i;

    static const char pmic_regset[] =
    {
        MC13783_INTERRUPT_STATUS0,
        MC13783_INTERRUPT_SENSE0,
        MC13783_INTERRUPT_STATUS1,
        MC13783_INTERRUPT_SENSE1,
        MC13783_CHARGER,
        MC13783_RTC_TIME,
        MC13783_RTC_ALARM,
        MC13783_RTC_DAY,
        MC13783_RTC_DAY_ALARM,
    };

    static const char *pmic_regnames[ARRAYLEN(pmic_regset)] =
    {
        "Int Stat0 ",
        "Int Sense0",
        "Int Stat1 ",
        "Int Sense1",
        "Charger   ",
        "RTC Time  ",
        "RTC Alarm ",
        "RTC Day   ",
        "RTC Day Al",
    };

    uint32_t pmic_regs[ARRAYLEN(pmic_regset)];

    lcd_clear_display();
    lcd_setfont(FONT_SYSFIXED);

    while(1)
    {
        line = 0;
        lcd_puts(0, line++, "[Ports and Registers]");
        line++;

        /* GPIO1 */
        lcd_putsf(0, line++, "GPIO1: DR:   %08lx GDIR: %08lx", GPIO1_DR, GPIO1_GDIR);

        lcd_putsf(0, line++, "       PSR:  %08lx ICR1: %08lx", GPIO1_PSR, GPIO1_ICR1);

        lcd_putsf(0, line++, "       ICR2: %08lx IMR:  %08lx", GPIO1_ICR2, GPIO1_IMR);

        lcd_putsf(0, line++, "       ISR:  %08lx",             GPIO1_ISR);
        line++;

        /* GPIO2 */
        lcd_putsf(0, line++, "GPIO2: DR:   %08lx GDIR: %08lx", GPIO2_DR, GPIO2_GDIR);

        lcd_putsf(0, line++, "       PSR:  %08lx ICR1: %08lx", GPIO2_PSR, GPIO2_ICR1);

        lcd_putsf(0, line++, "       ICR2: %08lx IMR:  %08lx", GPIO2_ICR2, GPIO2_IMR);

        lcd_putsf(0, line++, "       ISR:  %08lx",             GPIO2_ISR);
        line++;

        /* GPIO3 */
        lcd_putsf(0, line++, "GPIO3: DR:   %08lx GDIR: %08lx", GPIO3_DR, GPIO3_GDIR);

        lcd_putsf(0, line++, "       PSR:  %08lx ICR1: %08lx", GPIO3_PSR, GPIO3_ICR1);

        lcd_putsf(0, line++, "       ICR2: %08lx IMR:  %08lx", GPIO3_ICR2, GPIO3_IMR);

        lcd_putsf(0, line++, "       ISR:  %08lx",             GPIO3_ISR);
        line++;

        lcd_puts(0, line++, "PMIC Registers");
        line++;

        mc13783_read_regset(pmic_regset, pmic_regs, ARRAYLEN(pmic_regs));

        for (i = 0; i < (int)ARRAYLEN(pmic_regs); i++)
        {
            lcd_putsf(0, line++, "%s: %08lx", pmic_regnames[i], pmic_regs[i]);
        }

        line++;

        lcd_puts(0, line++, "ADC");
        line++;

        for (i = 0; i < NUM_ADC_CHANNELS; i += 4)
        {
            lcd_putsf(0, line++,
                     "CH%02d:%04u CH%02d:%04u CH%02d:%04u CH%02d:%04u",
                     i+0, adc_read(i+0),
                     i+1, adc_read(i+1),
                     i+2, adc_read(i+2),
                     i+3, adc_read(i+3));
        }

        lcd_update();
        if (button_get_w_tmo(HZ/10) == (DEBUG_CANCEL|BUTTON_REL))
            return false;
    }
}  
