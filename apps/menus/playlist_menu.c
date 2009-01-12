/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2007 Jonathan Gordon
 * Copyright (C) 2009 Alex Bennee
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

#include <stdbool.h>
#include <string.h>
#include "config.h"
#include "lang.h"
#include "action.h"
#include "settings.h"
#include "menu.h"
#include "playlist_menu.h"

#include "file.h"
#include "keyboard.h"
#include "playlist.h"
#include "tree.h"
#include "playlist_viewer.h"
#include "talk.h"
#include "playlist_catalog.h"
#include "sprintf.h"

#include "debug.h"

extern struct playlist_info current_playlist;

/*
  playlist_menu_getname

  Fetch the current playlist name and then ensure:

  a) Ends in m3u8 (as we encode as UTF8)
  b) Contains a proper path
  c) Failing that is the default path
*/

static void playlist_menu_getname(struct playlist_info* playlist, char *str, int maxlen)
{
    int len;

    /*
      Fetch the playlist name, if playlist is NULL it will
      fetch the current playing playlist name
    */
    playlist_get_name(playlist, str, maxlen);
    len = strlen(str);

    /* Make sure playlist always ends in m3u8 */
    if (len > 4 && !strcasecmp(&str[len-4], ".m3u"))
        strcat(str, "8");
    
    /* If the playlist is shorter than the extension then it's
     * probably a dud filename so replace it with the default
     * filename
     */
    if (len<=5)
    {
        /* directory config is of the format: "dir: /path/to/dir" */
        if (global_settings.playlist_catalog_dir[0])
        {
            snprintf(str, maxlen, "%s%s", global_settings.playlist_catalog_dir, DEFAULT_DYNAMIC_PLAYLIST_NAME);
        } else {
            strlcpy(str, DEFAULT_DYNAMIC_PLAYLIST_NAME, sizeof(DEFAULT_DYNAMIC_PLAYLIST_NAME));
        }
    }
}

/*
  save_playlist_screen

  Called when we want to save the current playlist. If the current
  playlist is already named then sets the starting string as that.

  Returns 0 if we didn't anything, 1 once the playlist is saved
*/
  
int save_playlist_screen(struct playlist_info* playlist)
{
    char temp[MAX_PATH+1];

    playlist_menu_getname(playlist, temp, MAX_PATH);
    
    if (!kbd_input(temp, MAX_PATH))
    {
        playlist_save(playlist, temp);

        /* reload in case playlist was saved to cwd */
        reload_directory();

	/* can exit menu now */
	return 1;
    }

    return 0;
}

/*
  Main Menu -> Playlists ->
    + Create Playlist
    + View Current Playlist
    + Save Current Playlist
    + View Catalog
*/

MENUITEM_FUNCTION(create_playlist_item, 0, ID2P(LANG_CREATE_PLAYLIST), 
                  (int(*)(void))create_playlist, NULL, NULL, Icon_NOICON);
MENUITEM_FUNCTION(view_playlist, 0, ID2P(LANG_VIEW_DYNAMIC_PLAYLIST), 
                  (int(*)(void))playlist_viewer, NULL, NULL, Icon_NOICON);
MENUITEM_FUNCTION(save_playlist, MENU_FUNC_USEPARAM, ID2P(LANG_SAVE_DYNAMIC_PLAYLIST), 
                         (int(*)(void*))save_playlist_screen, 
                        NULL, NULL, Icon_NOICON);
MENUITEM_FUNCTION(catalog, 0, ID2P(LANG_CATALOG_VIEW), 
                  (int(*)(void))catalog_view_playlists,
                   NULL, NULL, Icon_NOICON);
MAKE_MENU(playlist_options, ID2P(LANG_PLAYLISTS), NULL,
          Icon_Playlist,
          &create_playlist_item, &view_playlist, &save_playlist, &catalog);

/*
  Settings -> General Settings -> Playlists ->
    + Recursively Insert Directories
    + Warn When Erasing Modified Playlist
*/
  
MENUITEM_SETTING(recursive_dir_insert, &global_settings.recursive_dir_insert, NULL);
MENUITEM_SETTING(warn_on_erase, &global_settings.warnon_erase_dynplaylist, NULL);

MAKE_MENU(playlist_settings, ID2P(LANG_PLAYLISTS), NULL,
          Icon_Playlist,
          &recursive_dir_insert, &warn_on_erase);

/* Returns "Save current as <current name>" */
static char* save_playlist_as_string(int selected_item, void * data, char *buffer)
{
    /* Unused */
    (void) selected_item;
    (void) data;

    char temp[MAX_PATH];
    
    playlist_menu_getname(NULL, temp, MAX_PATH);
    snprintf(buffer, MAX_PATH, str(LANG_SAVE_CURRENT_PLAYLIST_AS), temp);
    return buffer;
}

/* Just save the playlist and exit */
int save_playlist_now(void)
{
    char temp[MAX_PATH];

    playlist_menu_getname(NULL, temp, MAX_PATH);
    playlist_save(NULL, temp);
    /* reload in case playlist was saved to cwd */
    reload_directory();
    return 1;
}

/*
  Menu:
  
  (Potential playlist erasing action) ->
  "Save current playlist?
    + as "current name"
    + as ....
    + No (Changes will be lost)
*/

MENUITEM_FUNCTION_DYNTEXT(pmsc_save_playlist_as,
			  MENU_FUNC_CHECK_RETVAL,
                          (int(*)(void))save_playlist_now, /* func */
                          NULL,
                          save_playlist_as_string,
			  NULL, /* voice callback */
                          NULL,
			  NULL, Icon_NOICON);

MENUITEM_FUNCTION(pmsc_save_playlist,
		  MENU_FUNC_USEPARAM|MENU_FUNC_CHECK_RETVAL,
		  ID2P(LANG_SAVE_CURRENT_PLAYLIST_AS_PROMPT), 
		  (int(*)(void*))save_playlist_screen, 
		  &current_playlist,
		  NULL, Icon_NOICON);

MENUITEM_RETURNVALUE(pmsc_continue,
		     ID2P(LANG_DONT_SAVE_CURRENT_PLAYLIST),     /* Continue */
		     0,                                         /* Returns 0 */
		     NULL, Icon_NOICON);

MAKE_MENU(maybe_save_menu, ID2P(LANG_SAVE_CURRENT_PLAYLIST_Q), NULL,
	  Icon_Playlist,
	  &pmsc_save_playlist_as, &pmsc_save_playlist, &pmsc_continue);

/*
 * Potentially save current dynamic playlist.
 *
 * Originally we used to warn if the dynamic playlist was going to get
 * over-written. Now we offer multiple options for quick save
 *
 * "Save current playlist?
 *   - as "current name"
 *   - as ....
 *   - No (Changes will be lost)
 *
 * If the user aborts the menu (e.g. going "back") we return false up
 * the chain and abort whatever action we were about to do
 *
 */
bool playlist_maybe_save_current(void)
{
    bool r=true;
    
    DEBUGF("playlist_maybe_save_current: c:%s pm:%s mod:%s\n",
	   global_settings.warnon_erase_dynplaylist?"true":"false",
	   global_settings.party_mode?"true":"false",
	   playlist_modified(NULL)?"true":"false");
    
    /* We only care if the playlist has been modified */
    if (global_settings.warnon_erase_dynplaylist &&
        !global_settings.party_mode &&
        playlist_modified(NULL))
    {
	int menu_selection = 0;
	int menu_r;
	menu_r = do_menu(&maybe_save_menu, &menu_selection, NULL, false);


	DEBUGF("playlist_maybe_save_current: menu_selection=%d menu_r=%d\n",
	       menu_selection, menu_r);

	/* Did we abort? */
	if (menu_r == GO_TO_PREVIOUS) {
	    r = false;
	}		
    }

    DEBUGF("playlist_maybe_save_current: %s\n", r?"true":"false");
    return r;
}
 
