/*****************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _// __ \_/ ___\|  |/ /| __ \ / __ \  \/  /
 *   Jukebox    |    |   ( (__) )  \___|    ( | \_\ ( (__) )    (
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2008 Andrew Mahone
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

#include "plugin.h"
#include "lib/grey.h"
PLUGIN_HEADER

/* different graphics libraries */
#if LCD_DEPTH < 8
#define USEGSLIB
GREY_INFO_STRUCT
#define MYLCD(fn) grey_ub_ ## fn
#define MYLCD_UPDATE()
#define MYXLCD(fn) grey_ub_ ## fn
#define CFORMAT &format_grey
#else
#define MYLCD(fn) rb->lcd_ ## fn
#define MYLCD_UPDATE() rb->lcd_update();
#define MYXLCD(fn) xlcd_ ## fn
#define CFORMAT NULL
#endif

/* this is the plugin entry point */
enum plugin_status plugin_start(const void* parameter)
{
    size_t plugin_buf_len;
    unsigned char * plugin_buf =
        (unsigned char *)rb->plugin_get_buffer(&plugin_buf_len);
    static char filename[MAX_PATH];
    struct bitmap bm = {
        .width = LCD_WIDTH,
        .height = LCD_HEIGHT,
    };
    int ret;

    if(!parameter) return PLUGIN_ERROR;

    rb->strcpy(filename, parameter);

#ifdef USEGSLIB
    long greysize;
    if (!grey_init(plugin_buf, plugin_buf_len, GREY_ON_COP,
                   LCD_WIDTH, LCD_HEIGHT, &greysize))
    {
        rb->splash(HZ, "grey buf error");
        return PLUGIN_ERROR;
    }
    plugin_buf += greysize;
    plugin_buf_len -= greysize;
#endif
    bm.data = plugin_buf;
    ret = rb->read_jpeg_file(filename, &bm, plugin_buf_len,
                   FORMAT_NATIVE|FORMAT_RESIZE|FORMAT_KEEP_ASPECT,
                   CFORMAT);
    if (ret < 1)
        return PLUGIN_ERROR;
#ifdef USEGSLIB
    grey_show(true);
    grey_ub_gray_bitmap((fb_data *)bm.data, (LCD_WIDTH - bm.width) >> 1,
        (LCD_HEIGHT - bm.height) >> 1, bm.width, bm.height);
#else
    rb->lcd_bitmap((fb_data *)bm.data, (LCD_WIDTH - bm.width) >> 1,
        (LCD_HEIGHT - bm.height) >> 1, bm.width, bm.height);
#endif
    MYLCD_UPDATE();
    while (rb->get_action(CONTEXT_STD,1) != ACTION_STD_OK) rb->yield();
#ifdef USEGSLIB
    grey_release();
#endif
    return PLUGIN_OK;
}
