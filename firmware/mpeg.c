/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Copyright (C) 2002 by Linus Nielsen Feltzing
 *
 * All files in this archive are subject to the GNU General Public License.
 * See the file COPYING in the source tree root for full license agreement.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/
#include <stdbool.h>
#include "config.h"
#include "debug.h"
#include "panic.h"
#include "id3.h"
#include "mpeg.h"
#include "ata.h"
#include "string.h"
#include <kernel.h>
#include "thread.h"
#include "errno.h"
#include "mp3data.h"
#include "buffer.h"
#include "mp3_playback.h"
#ifndef SIMULATOR
#include "i2c.h"
#include "mas.h"
#include "dac.h"
#include "system.h"
#include "usb.h"
#include "file.h"
#include "hwcompat.h"
#endif

extern void bitswap(unsigned char *data, int length);

#ifdef HAVE_MAS3587F
static void init_recording(void);
static void start_prerecording(void);
static void start_recording(void);
static void stop_recording(void);
static int get_unsaved_space(void);
#endif

#ifndef SIMULATOR
static int get_unplayed_space(void);
static int get_playable_space(void);
static int get_unswapped_space(void);
#endif

#define MPEG_PLAY         1
#define MPEG_STOP         2
#define MPEG_PAUSE        3
#define MPEG_RESUME       4
#define MPEG_NEXT         5
#define MPEG_PREV         6
#define MPEG_FF_REWIND    7
#define MPEG_FLUSH_RELOAD 8
#define MPEG_RECORD 9
#define MPEG_INIT_RECORDING 10
#define MPEG_INIT_PLAYBACK 11
#define MPEG_NEW_FILE    12
#define MPEG_NEED_DATA    100
#define MPEG_TRACK_CHANGE 101
#define MPEG_SAVE_DATA    102
#define MPEG_STOP_DONE    103

#ifdef HAVE_MAS3587F
static enum
{
    MPEG_DECODER,
    MPEG_ENCODER
} mpeg_mode;
#endif

extern char* playlist_peek(int steps);
extern bool playlist_check(int steps);
extern int playlist_next(int steps);
extern int playlist_amount(void);
extern void update_file_pos( int id, int pos );

/* list of tracks in memory */
#define MAX_ID3_TAGS (1<<4) /* Must be power of 2 */
#define MAX_ID3_TAGS_MASK (MAX_ID3_TAGS - 1)

struct id3tag
{
    struct mp3entry id3;
    int mempos;
    bool used;
};

static struct id3tag *id3tags[MAX_ID3_TAGS];
static struct id3tag _id3tags[MAX_ID3_TAGS];

static unsigned int current_track_counter = 0;
static unsigned int last_track_counter = 0;

#ifndef SIMULATOR

static int tag_read_idx = 0;
static int tag_write_idx = 0;

static int num_tracks_in_memory(void)
{
    return (tag_write_idx - tag_read_idx) & MAX_ID3_TAGS_MASK;
}
#endif

#ifndef SIMULATOR
static void debug_tags(void)
{
#ifdef DEBUG_TAGS
    int i;

    for(i = 0;i < MAX_ID3_TAGS;i++)
    {
        DEBUGF("id3tags[%d]: %08x", i, id3tags[i]);
        if(id3tags[i])
            DEBUGF(" - %s", id3tags[i]->id3.path);
        DEBUGF("\n");
    }
    DEBUGF("read: %d, write :%d\n", tag_read_idx, tag_write_idx);
    DEBUGF("num_tracks_in_memory: %d\n", num_tracks_in_memory());
#endif
}

static bool append_tag(struct id3tag *tag)
{
    if(num_tracks_in_memory() < MAX_ID3_TAGS - 1)
    {
        id3tags[tag_write_idx] = tag;
        tag_write_idx = (tag_write_idx+1) & MAX_ID3_TAGS_MASK;
        debug_tags();
        return true;
    }
    else
    {
        DEBUGF("Tag memory is full\n");
        return false;
    }
}

static void remove_current_tag(void)
{
    int oldidx = tag_read_idx;
    
    if(num_tracks_in_memory() > 0)
    {
        /* First move the index, so nobody tries to access the tag */
        tag_read_idx = (tag_read_idx+1) & MAX_ID3_TAGS_MASK;

        /* Now delete it */
        id3tags[oldidx]->used = false;
        id3tags[oldidx] = NULL;
        debug_tags();
    }
}

static void remove_all_non_current_tags(void)
{
    int i = (tag_read_idx+1) & MAX_ID3_TAGS_MASK;

    while (i != tag_write_idx)
    {
        id3tags[i]->used = false;
        id3tags[i] = NULL;

        i = (i+1) & MAX_ID3_TAGS_MASK;
    }

    tag_write_idx = (tag_read_idx+1) & MAX_ID3_TAGS_MASK;
    debug_tags();
}

static void remove_all_tags(void)
{
    int i;
    
    for(i = 0;i < MAX_ID3_TAGS;i++)
        remove_current_tag();

    tag_write_idx = tag_read_idx;

    debug_tags();
}
#endif

static void set_elapsed(struct mp3entry* id3)
{
    if ( id3->vbr ) {
        if ( id3->has_toc ) {
            /* calculate elapsed time using TOC */
            int i;
            unsigned int remainder, plen, relpos, nextpos;

            /* find wich percent we're at */
            for (i=0; i<100; i++ )
            {
                if ( id3->offset < (int)(id3->toc[i] * (id3->filesize / 256)) )
                {
                    break;
                }
            }
            
            i--;
            if (i < 0)
                i = 0;

            relpos = id3->toc[i];

            if (i < 99)
            {
                nextpos = id3->toc[i+1];
            }
            else
            {
                nextpos = 256; 
            }

            remainder = id3->offset - (relpos * (id3->filesize / 256));

            /* set time for this percent (divide before multiply to prevent
               overflow on long files. loss of precision is negligible on
               short files) */
            id3->elapsed = i * (id3->length / 100);

            /* calculate remainder time */
            plen = (nextpos - relpos) * (id3->filesize / 256);
            id3->elapsed += (((remainder * 100) / plen) *
                             (id3->length / 10000));
        }
        else {
            /* no TOC exists. set a rough estimate using average bitrate */
            int tpk = id3->length / (id3->filesize / 1024);
            id3->elapsed = id3->offset / 1024 * tpk;
        }
    }
    else
        /* constant bitrate == simple frame calculation */
        id3->elapsed = id3->offset / id3->bpf * id3->tpf;
}

static bool paused; /* playback is paused */

static unsigned int mpeg_errno;

#ifdef SIMULATOR
static bool is_playing = false;
static bool playing = false;
#else
static int last_dma_tick = 0;

extern unsigned long mas_version_code;

static struct event_queue mpeg_queue;
static char mpeg_stack[DEFAULT_STACK_SIZE + 0x1000];
static char mpeg_thread_name[] = "mpeg";

static int mp3buflen;
static int mp3buf_write;
static int mp3buf_swapwrite;
static int mp3buf_read;

static int last_dma_chunk_size;

static bool playing; /* We are playing an MP3 stream */
static bool play_pending; /* We are about to start playing */
static bool is_playing; /* We are (attempting to) playing MP3 files */
static bool filling; /* We are filling the buffer with data from disk */
static bool dma_underrun; /* True when the DMA has stopped because of
                             slow disk reading (read error, shaking) */
static int low_watermark; /* Dynamic low watermark level */
static int low_watermark_margin; /* Extra time in seconds for watermark */
static int lowest_watermark_level; /* Debug value to observe the buffer
                                      usage */
#ifdef HAVE_MAS3587F
static bool is_recording; /* We are recording */
static bool stop_pending;
unsigned long record_start_time; /* Value of current_tick when recording
                                    was started */
static bool saving; /* We are saving the buffer to disk */
static char recording_filename[MAX_PATH];
static int rec_frequency_index; /* For create_xing_header() calls */
static int rec_version_index;   /* For create_xing_header() calls */
static bool disable_xing_header; /* When splitting files */

static bool prerecording; /* True if prerecording is enabled */
static bool is_prerecording; /* True if we are prerecording */
static int prerecord_buffer[MPEG_MAX_PRERECORD_SECONDS]; /* Array of buffer
                                                            indexes for each
                                                            prerecorded
                                                            second */
static int prerecord_index; /* Current index in the prerecord buffer */
static int prerecording_max_seconds;   /* Max number of seconds to store */
static int prerecord_count; /* Number of seconds in the prerecord buffer */
static int prerecord_timeout; /* The tick count of the next prerecord data store */
#endif

static int mpeg_file;

/* Synchronization variables */
#ifdef HAVE_MAS3587F
static bool init_recording_done;
static bool init_playback_done;
#endif
static bool mpeg_stop_done;

static void recalculate_watermark(int bitrate)
{
    int bytes_per_sec;
    int time = ata_spinup_time;

    /* A bitrate of 0 probably means empty VBR header. We play safe
       and set a high threshold */
    if(bitrate == 0)
        bitrate = 320;
    
    bytes_per_sec = bitrate * 1000 / 8;
    
    if(time)
    {
        /* No drive spins up faster than 3.5s */
        if(time < 350)
            time = 350;

        time = time * 3;
        low_watermark = ((low_watermark_margin * HZ + time) *
                         bytes_per_sec) / HZ;
    }
    else
    {
        low_watermark = MPEG_LOW_WATER;
    }
}

void mpeg_set_buffer_margin(int seconds)
{
    low_watermark_margin = seconds;
}

void mpeg_get_debugdata(struct mpeg_debug *dbgdata)
{
    dbgdata->mp3buflen = mp3buflen;
    dbgdata->mp3buf_write = mp3buf_write;
    dbgdata->mp3buf_swapwrite = mp3buf_swapwrite;
    dbgdata->mp3buf_read = mp3buf_read;

    dbgdata->last_dma_chunk_size = last_dma_chunk_size;

    dbgdata->dma_on = (SCR0 & 0x80) != 0;
    dbgdata->playing = playing;
    dbgdata->play_pending = play_pending;
    dbgdata->is_playing = is_playing;
    dbgdata->filling = filling;
    dbgdata->dma_underrun = dma_underrun;

    dbgdata->unplayed_space = get_unplayed_space();
    dbgdata->playable_space = get_playable_space();
    dbgdata->unswapped_space = get_unswapped_space();

    dbgdata->low_watermark_level = low_watermark;
    dbgdata->lowest_watermark_level = lowest_watermark_level;
}

#ifndef HAVE_MAS3507D
static void postpone_dma_tick(void)
{
    unsigned int count;

    count = FREQ / 1000 / 8;

    /* We are using timer 1 */
    
    TSTR &= ~0x02; /* Stop the timer */
    TSNC &= ~0x02; /* No synchronization */
    TMDR &= ~0x02; /* Operate normally */

    TCNT1 = 0;   /* Start counting at 0 */
    GRA1 = count;
    TCR1 = 0x23; /* Clear at GRA match, sysclock/8 */

    /* Enable interrupt on level 5 */
    IPRC = (IPRC & ~0x000f) | 0x0005;
    
    TSR1 &= ~0x02;
    TIER1 = 0xf9; /* Enable GRA match interrupt */

    TSTR |= 0x02; /* Start timer 1 */
}
#endif

#ifdef DEBUG
static void dbg_timer_start(void)
{
    /* We are using timer 2 */
    
    TSTR &= ~0x04; /* Stop the timer */
    TSNC &= ~0x04; /* No synchronization */
    TMDR &= ~0x44; /* Operate normally */

    TCNT2 = 0;   /* Start counting at 0 */
    TCR2 = 0x03; /* Sysclock/8 */

    TSTR |= 0x04; /* Start timer 2 */
}

static int dbg_cnt2us(unsigned int cnt)
{
    return (cnt * 10000) / (FREQ/800);
}
#endif

static int get_unplayed_space(void)
{
    int space = mp3buf_write - mp3buf_read;
    if (space < 0)
        space += mp3buflen;
    return space;
}

static int get_playable_space(void)
{
    int space = mp3buf_swapwrite - mp3buf_read;
    if (space < 0)
        space += mp3buflen;
    return space;
}

static int get_unplayed_space_current_song(void)
{
    int space;

    if (num_tracks_in_memory() > 1)
    {
        int track_offset = (tag_read_idx+1) & MAX_ID3_TAGS_MASK;

        space = id3tags[track_offset]->mempos - mp3buf_read;
    }
    else
    {
        space = mp3buf_write - mp3buf_read;
    }

    if (space < 0)
        space += mp3buflen;

    return space;
}

static int get_unswapped_space(void)
{
    int space = mp3buf_write - mp3buf_swapwrite;
    if (space < 0)
        space += mp3buflen;
    return space;
}

#ifdef HAVE_MAS3587F
static int get_unsaved_space(void)
{
    int space = mp3buf_write - mp3buf_read;
    if (space < 0)
        space += mp3buflen;
    return space;
}
#endif

#ifdef HAVE_MAS3587F
#ifdef DEBUG
static long timing_info_index = 0;
static long timing_info[1024];
#endif
static bool inverted_pr;
static unsigned long num_rec_bytes;
static unsigned long num_recorded_frames;

static void drain_dma_buffer(void)
{
    if(inverted_pr)
    {
        while((*((volatile unsigned char *)PBDR_ADDR) & 0x40))
        {
            or_b(0x08, &PADRH);
            
            while(*((volatile unsigned char *)PBDR_ADDR) & 0x80);
                    
            /* It must take at least 5 cycles before
               the data is read */
            asm(" nop\n nop\n nop\n");
            asm(" nop\n nop\n nop\n");
            and_b(~0x08, &PADRH);

            while(!(*((volatile unsigned char *)PBDR_ADDR) & 0x80));
        }
    }
    else
    {
        while((*((volatile unsigned char *)PBDR_ADDR) & 0x40))
        {
            and_b(~0x08, &PADRH);
            
            while(*((volatile unsigned char *)PBDR_ADDR) & 0x80);
            
            /* It must take at least 5 cycles before
               the data is read */
            asm(" nop\n nop\n nop\n");
            asm(" nop\n nop\n nop\n");
                    
            or_b(0x08, &PADRH);

            while(!(*((volatile unsigned char *)PBDR_ADDR) & 0x80));
        }
    }
}

#endif /* #ifdef HAVE_MAS3587F */

static void dma_tick (void) __attribute__ ((section (".icode")));
static void dma_tick(void)
{
#ifdef HAVE_MAS3587F
    if(mpeg_mode == MPEG_DECODER)
    {
#endif
        if(playing && !paused)
        {
            /* Start DMA if it is disabled and the DEMAND pin is high */
            if(!(SCR0 & 0x80) && (PBDR & 0x4000))
            {
                mp3_play_pause(true);
            }
            id3tags[tag_read_idx]->id3.elapsed +=
                (current_tick - last_dma_tick) * 1000 / HZ;
            last_dma_tick = current_tick;
        }
#ifdef HAVE_MAS3587F
    }
    else /* MPEG_ENCODER */
    {
        int i;
        int num_bytes;
        if(is_recording && (PBDR & 0x4000))
        {
#ifdef DEBUG
            timing_info[timing_info_index++] = current_tick;
            TCNT2 = 0;
#endif
            /* We read as long as EOD is high, but max 30 bytes.
               This code is optimized, and should probably be
               written in assembler instead. */
            if(inverted_pr)
            {
                i = 0;
                while((*((volatile unsigned char *)PBDR_ADDR) & 0x40)
                      && i < 30)
                {
                    or_b(0x08, &PADRH);

                    while(*((volatile unsigned char *)PBDR_ADDR) & 0x80);
                    
                    /* It must take at least 5 cycles before
                       the data is read */
                    asm(" nop\n nop\n nop\n");
                    mp3buf[mp3buf_write++] = *(unsigned char *)0x4000000;
                    
                    if(mp3buf_write >= mp3buflen)
                        mp3buf_write = 0;

                    i++;
                    
                    and_b(~0x08, &PADRH);

                    /* No wait for /RTW, cause it's not necessary */
                }
            }
            else /* !inverted_pr */
            {
                i = 0;
                while((*((volatile unsigned char *)PBDR_ADDR) & 0x40)
                      && i < 30)
                {
                    and_b(~0x08, &PADRH);
                    
                    while(*((volatile unsigned char *)PBDR_ADDR) & 0x80);
                    
                    /* It must take at least 5 cycles before
                       the data is read */
                    asm(" nop\n nop\n nop\n");
                    mp3buf[mp3buf_write++] = *(unsigned char *)0x4000000;
                    
                    if(mp3buf_write >= mp3buflen)
                        mp3buf_write = 0;

                    i++;
                    
                    or_b(0x08, &PADRH);

                    /* No wait for /RTW, cause it's not necessary */
                }
            }
#ifdef DEBUG
            timing_info[timing_info_index++] = TCNT2 + (i << 16);
            timing_info_index &= 0x3ff;
#endif

            num_rec_bytes += i;
            
            if(is_prerecording)
            {
                if(TIME_AFTER(current_tick, prerecord_timeout))
                {
                    prerecord_timeout = current_tick + HZ;

                    /* Store the write pointer every second */
                    prerecord_buffer[prerecord_index++] = mp3buf_write;

                    /* Wrap if necessary */
                    if(prerecord_index == prerecording_max_seconds)
                        prerecord_index = 0;

                    /* Update the number of seconds recorded */
                    if(prerecord_count < prerecording_max_seconds)
                        prerecord_count++;
                }
            }
            else
            {
                /* Signal to save the data if we are running out of buffer
                   space */
                num_bytes = mp3buf_write - mp3buf_read;
                if(num_bytes < 0)
                    num_bytes += mp3buflen;
                
                if(mp3buflen - num_bytes < MPEG_RECORDING_LOW_WATER && !saving)
                {
                    saving = true;
                    queue_post(&mpeg_queue, MPEG_SAVE_DATA, 0);
                    wake_up_thread();
                }
            }
        }
    }
#endif
}

static void reset_mp3_buffer(void)
{
    mp3buf_read = 0;
    mp3buf_write = 0;
    mp3buf_swapwrite = 0;
    lowest_watermark_level = mp3buflen;
}

 /* DMA transfer end interrupt callback */
static void transfer_end(unsigned char** ppbuf, int* psize)
{
    if(playing && !paused)
    {
        int unplayed_space_left;
        int space_until_end_of_buffer;
        int track_offset = (tag_read_idx+1) & MAX_ID3_TAGS_MASK;

        mp3buf_read += last_dma_chunk_size;
        if(mp3buf_read >= mp3buflen)
            mp3buf_read = 0;
    
        /* First, check if we are on a track boundary */
        if (num_tracks_in_memory() > 0)
        {
            if (mp3buf_read == id3tags[track_offset]->mempos)
            {
                queue_post(&mpeg_queue, MPEG_TRACK_CHANGE, 0);
                track_offset = (track_offset+1) & MAX_ID3_TAGS_MASK;
            }
        }
        
        unplayed_space_left = get_unplayed_space();
        
        space_until_end_of_buffer = mp3buflen - mp3buf_read;
        
        if(!filling && unplayed_space_left < low_watermark)
        {
            filling = true;
            queue_post(&mpeg_queue, MPEG_NEED_DATA, 0);
        }
        
        if(unplayed_space_left)
        {
            last_dma_chunk_size = MIN(0x2000, unplayed_space_left);
            last_dma_chunk_size = MIN(last_dma_chunk_size,
                                      space_until_end_of_buffer);

            /* several tracks loaded? */
            if (num_tracks_in_memory() > 1)
            {
                /* will we move across the track boundary? */
                if (( mp3buf_read < id3tags[track_offset]->mempos ) &&
                    ((mp3buf_read+last_dma_chunk_size) >
                     id3tags[track_offset]->mempos ))
                {
                    /* Make sure that we end exactly on the boundary */
                    last_dma_chunk_size = id3tags[track_offset]->mempos
                        - mp3buf_read;
                }
            }

            *psize = last_dma_chunk_size & 0xffff;
            *ppbuf = mp3buf + mp3buf_read;
            id3tags[tag_read_idx]->id3.offset += last_dma_chunk_size;

            /* Update the watermark debug level */
            if(unplayed_space_left < lowest_watermark_level)
                lowest_watermark_level = unplayed_space_left;
        }
        else
        {
            /* Check if the end of data is because of a hard disk error.
               If there is an open file handle, we are still playing music.
               If not, the last file has been loaded, and the file handle is
               closed. */
            if(mpeg_file >= 0)
            {
                /* Update the watermark debug level */
                if(unplayed_space_left < lowest_watermark_level)
                    lowest_watermark_level = unplayed_space_left;
                
                DEBUGF("DMA underrun.\n");
                dma_underrun = true;
            }
            else
            {
                DEBUGF("No more MP3 data. Stopping.\n");

                queue_post(&mpeg_queue, MPEG_TRACK_CHANGE, 0);
                playing = false;
                is_playing = false;
            }
            *psize = 0; /* no more transfer */
        }
    }

    wake_up_thread();
}

#ifdef HAVE_MAS3587F
static void demand_irq_enable(bool on)
{
    int oldlevel = set_irq_level(15);
    
    if(on)
    {
        IPRA = (IPRA & 0xfff0) | 0x000b;
        ICR &= ~0x0010; /* IRQ3 level sensitive */
    }
    else
        IPRA &= 0xfff0;

    set_irq_level(oldlevel);
}
#endif

#pragma interrupt
void IMIA1(void) /* Timer 1 interrupt */
{
    dma_tick();
    TSR1 &= ~0x01;
#ifdef HAVE_MAS3587F
    /* Disable interrupt */
    IPRC &= ~0x000f;
#endif
}

#ifdef HAVE_MAS3587F
#pragma interrupt
void IRQ3(void) /* PA15: MAS demand IRQ */
{
    /* Begin with setting the IRQ to edge sensitive */
    ICR |= 0x0010;
    
    if(mpeg_mode == MPEG_ENCODER)
        dma_tick();
    else
        postpone_dma_tick();
}
#endif

static int add_track_to_tag_list(char *filename)
{
    struct id3tag *t = NULL;
    int i;

    /* find a free tag */
    for (i=0; i < MAX_ID3_TAGS_MASK; i++ )
        if ( !_id3tags[i].used )
            t = &_id3tags[i];
    if(t)
    {
        /* grab id3 tag of new file and
           remember where in memory it starts */
        if(mp3info(&(t->id3), filename))
        {
            DEBUGF("Bad mp3\n");
            return -1;
        }
        t->mempos = mp3buf_write;
        t->id3.elapsed = 0;
        if(!append_tag(t))
        {
            DEBUGF("Tag list is full\n");
        }
        else
            t->used = true;
    }
    else
    {
        DEBUGF("No memory available for id3 tag");
    }
    return 0;
}

/* If next_track is true, opens the next track, if false, opens prev track */
static int new_file(int steps)
{
    int max_steps = playlist_amount();
    int start = num_tracks_in_memory() - 1;

    if (start < 0)
        start = 0;

    do {
        char *trackname;

        trackname = playlist_peek( start + steps );
        if ( !trackname )
            return -1;
        
        DEBUGF("playing %s\n", trackname);
        
        mpeg_file = open(trackname, O_RDONLY);
        if(mpeg_file < 0) {
            DEBUGF("Couldn't open file: %s\n",trackname);
            steps++;

            /* Bail out if no file could be opened */
            if(steps > max_steps)
                return -1;
        }
        else
        {
            int new_tag_idx = tag_write_idx;

            if(add_track_to_tag_list(trackname))
            {
                /* Bad mp3 file */
                steps++;
                close(mpeg_file);
                mpeg_file = -1;
            }
            else
            {
                /* skip past id3v2 tag */
                lseek(mpeg_file, 
                      id3tags[new_tag_idx]->id3.first_frame_offset,
                      SEEK_SET);
                id3tags[new_tag_idx]->id3.index = steps;
                id3tags[new_tag_idx]->id3.offset = 0;

                if(id3tags[new_tag_idx]->id3.vbr)
                    /* Average bitrate * 1.5 */
                    recalculate_watermark(
                        (id3tags[new_tag_idx]->id3.bitrate * 3) / 2);
                else
                    recalculate_watermark(
                        id3tags[new_tag_idx]->id3.bitrate);
                    
            }
        }
    } while ( mpeg_file < 0 );

    return 0;
}

static void stop_playing(void)
{
    /* Stop the current stream */
#ifdef HAVE_MAS3587F
    demand_irq_enable(false);
#endif
    playing = false;
    filling = false;
    if(mpeg_file >= 0)
        close(mpeg_file);
    mpeg_file = -1;
    mp3_play_pause(false);
    remove_all_tags();
}

static void update_playlist(void)
{
    int index;

    if (num_tracks_in_memory() > 0)
    {
        index = playlist_next(id3tags[tag_read_idx]->id3.index);
        id3tags[tag_read_idx]->id3.index = index;
    }
}

static void track_change(void)
{
    DEBUGF("Track change\n");

#ifdef HAVE_MAS3587F
    /* Reset the AVC */
    mpeg_sound_set(SOUND_AVC, -1);
#endif
    remove_current_tag();

    update_playlist();
    current_track_counter++;
}

#ifdef DEBUG
void hexdump(unsigned char *buf, int len)
{
    int i;

    for(i = 0;i < len;i++)
    {
        if(i && (i & 15) == 0)
        {
            DEBUGF("\n");
        }
        DEBUGF("%02x ", buf[i]);
    }
    DEBUGF("\n");
}
#endif

static void start_playback_if_ready(void)
{
    int playable_space;

    playable_space = mp3buf_swapwrite - mp3buf_read;
    if(playable_space < 0)
        playable_space += mp3buflen;
    
    /* See if we have started playing yet. If not, do it. */
    if(play_pending || dma_underrun)
    {
        /* If the filling has stopped, and we still haven't reached
           the watermark, the file must be smaller than the
           watermark. We must still play it. */
        if((playable_space >= MPEG_PLAY_PENDING_THRESHOLD) ||
           !filling || dma_underrun)
        {
            DEBUGF("P\n");
            play_pending = false;
            playing = true;
			
            last_dma_chunk_size = MIN(0x2000, get_unplayed_space_current_song());
            mp3_play_data(mp3buf + mp3buf_read, last_dma_chunk_size, transfer_end);
            dma_underrun = false;

            if (!paused)
            {
                last_dma_tick = current_tick;
                mp3_play_pause(true);
#ifdef HAVE_MAS3587F
                demand_irq_enable(true);
#endif
            }

            /* Tell ourselves that we need more data */
            queue_post(&mpeg_queue, MPEG_NEED_DATA, 0);
        }
    }
}

static bool swap_one_chunk(void)
{
    int free_space_left;
    int amount_to_swap;

    free_space_left = get_unswapped_space();

    if(free_space_left == 0 && !play_pending)
        return false;

    /* Swap in larger chunks when the user is waiting for the playback
       to start, or when there is dangerously little playable data left */
    if(play_pending)
        amount_to_swap = MIN(MPEG_PLAY_PENDING_SWAPSIZE, free_space_left);
    else
    {
        if(get_playable_space() < low_watermark)
            amount_to_swap = MIN(MPEG_LOW_WATER_SWAP_CHUNKSIZE,
                                 free_space_left);
        else
            amount_to_swap = MIN(MPEG_SWAP_CHUNKSIZE, free_space_left);
    }
    
    if(mp3buf_write < mp3buf_swapwrite)
        amount_to_swap = MIN(mp3buflen - mp3buf_swapwrite,
                             amount_to_swap);
    else
        amount_to_swap = MIN(mp3buf_write - mp3buf_swapwrite,
                             amount_to_swap);

    bitswap(mp3buf + mp3buf_swapwrite, amount_to_swap);

    mp3buf_swapwrite += amount_to_swap;
    if(mp3buf_swapwrite >= mp3buflen)
    {
        mp3buf_swapwrite = 0;
    }

    return true;
}

static const unsigned char empty_id3_header[] =
{
    'I', 'D', '3', 0x04, 0x00, 0x00,
    0x00, 0x00, 0x1f, 0x76 /* Size is 4096 minus 10 bytes for the header */
};

#ifdef HAVE_MAS3587F
static unsigned long get_last_recorded_header(void)
{
    unsigned long tmp[2];

    /* Read the frame data from the MAS and reconstruct it with the
       frame sync and all */
    mas_readmem(MAS_BANK_D0, 0xfd1, tmp, 2);
    return 0xffe00000 | ((tmp[0] & 0x7c00) << 6) | (tmp[1] & 0xffff);
}
#endif

static void mpeg_thread(void)
{
    static int pause_tick = 0;
    static unsigned int pause_track = 0;
    struct event ev;
    int len;
    int free_space_left;
    int unplayed_space_left;
    int amount_to_read;
    int t1, t2;
    int start_offset;
#ifdef HAVE_MAS3587F
    int amount_to_save;
    int writelen;
    int framelen;
    unsigned long saved_header = 0;
    int startpos;
    int rc;
    int offset;
    int countdown;
#endif
    
    is_playing = false;
    play_pending = false;
    playing = false;
    mpeg_file = -1;

    while(1)
    {
#ifdef HAVE_MAS3587F
        if(mpeg_mode == MPEG_DECODER)
        {
#endif
        yield();

        /* Swap if necessary, and don't block on the queue_wait() */
        if(swap_one_chunk())
        {
            queue_wait_w_tmo(&mpeg_queue, &ev, 0);
        }
        else
        {
            DEBUGF("S R:%x W:%x SW:%x\n",
                   mp3buf_read, mp3buf_write, mp3buf_swapwrite);
            queue_wait(&mpeg_queue, &ev);
        }

        start_playback_if_ready();
        
        switch(ev.id)
        {
            case MPEG_PLAY:
                DEBUGF("MPEG_PLAY\n");

#ifdef HAVE_FMRADIO
                /* Silence the A/D input, it may be on because the radio
                   may be playing */
                mas_codec_writereg(6, 0x0000);
#endif

                /* Stop the current stream */
                play_pending = false;
                playing = false;
                paused = false;
                mp3_play_pause(false);

                reset_mp3_buffer();
                remove_all_tags();

                if(mpeg_file >= 0)
                    close(mpeg_file);

                if ( new_file(0) == -1 )
                {
                    is_playing = false;
                    track_change();
                    break;
                }

                start_offset = (int)ev.data;

                /* mid-song resume? */
                if (start_offset) {
                    struct mp3entry* id3 = &id3tags[tag_read_idx]->id3;
                    lseek(mpeg_file, start_offset, SEEK_SET);
                    id3->offset = start_offset;
                    set_elapsed(id3);
                }
                else {
                    /* skip past id3v2 tag */
                    lseek(mpeg_file, 
                          id3tags[tag_read_idx]->id3.first_frame_offset, 
                          SEEK_SET);

                }

                /* Make it read more data */
                filling = true;
                queue_post(&mpeg_queue, MPEG_NEED_DATA, 0);

                /* Tell the file loading code that we want to start playing
                   as soon as we have some data */
                play_pending = true;

                update_playlist();
                current_track_counter++;
                break;

            case MPEG_STOP:
                DEBUGF("MPEG_STOP\n");
                is_playing = false;
                paused = false;
                stop_playing();
                mpeg_stop_done = true;
                break;

            case MPEG_PAUSE:
                DEBUGF("MPEG_PAUSE\n");
                /* Stop the current stream */
                paused = true;
                playing = false;
                pause_tick = current_tick;
                pause_track = current_track_counter;
                mp3_play_pause(false);
                break;

            case MPEG_RESUME:
                DEBUGF("MPEG_RESUME\n");
                /* Continue the current stream */
                paused = false;
                if (!play_pending)
                {
                    playing = true;
                    if ( current_track_counter == pause_track )
                        last_dma_tick += current_tick - pause_tick;
                    else
                        last_dma_tick = current_tick;                        
                    pause_tick = 0;
                    mp3_play_pause(true);
                }
                break;

            case MPEG_NEXT:
                DEBUGF("MPEG_NEXT\n");
                /* is next track in ram? */
                if ( num_tracks_in_memory() > 1 ) {
                    int unplayed_space_left, unswapped_space_left;

                    /* stop the current stream */
                    play_pending = false;
                    playing = false;
                    mp3_play_pause(false);

                    track_change();
                    mp3buf_read = id3tags[tag_read_idx]->mempos;
                    last_dma_chunk_size = MIN(0x2000, get_unplayed_space_current_song());
                    mp3_play_data(mp3buf + mp3buf_read, last_dma_chunk_size, transfer_end);
                    dma_underrun = false;
                    last_dma_tick = current_tick;

                    unplayed_space_left  = get_unplayed_space();
                    unswapped_space_left = get_unswapped_space();

                    /* should we start reading more data? */
                    if(!filling && (unplayed_space_left < low_watermark)) {
                        filling = true;
                        queue_post(&mpeg_queue, MPEG_NEED_DATA, 0);
                        play_pending = true;
                    } else if(unswapped_space_left &&
                              unswapped_space_left > unplayed_space_left) {
                        /* Stop swapping the data from the previous file */
                        mp3buf_swapwrite = mp3buf_read;
                        play_pending = true;
                    } else {
                        playing = true;
                        if (!paused)
                            mp3_play_pause(true);
                    }
                }
                else {
                    if (!playlist_check(1))
                        break;

                    /* stop the current stream */
                    play_pending = false;
                    playing = false;
                    mp3_play_pause(false);

                    reset_mp3_buffer();
                    remove_all_tags();

                    /* Open the next file */
                    if (mpeg_file >= 0)
                        close(mpeg_file);
                    
                    if (new_file(1) < 0) {
                        DEBUGF("No more files to play\n");
                        filling = false;
                    } else {
                        /* Make it read more data */
                        filling = true;
                        queue_post(&mpeg_queue, MPEG_NEED_DATA, 0);
                        
                        /* Tell the file loading code that we want
                           to start playing as soon as we have some data */
                        play_pending = true;

                        update_playlist();
                        current_track_counter++;
                    }
                }
                break;

            case MPEG_PREV: {
                DEBUGF("MPEG_PREV\n");

                if (!playlist_check(-1))
                    break;
                
                /* stop the current stream */
                play_pending = false;
                playing = false;
                mp3_play_pause(false);

                reset_mp3_buffer();
                remove_all_tags();

                /* Open the next file */
                if (mpeg_file >= 0)
                    close(mpeg_file);

                if (new_file(-1) < 0) {
                    DEBUGF("No more files to play\n");
                    filling = false;
                } else {
                    /* Make it read more data */
                    filling = true;
                    queue_post(&mpeg_queue, MPEG_NEED_DATA, 0);

                    /* Tell the file loading code that we want to
                       start playing as soon as we have some data */
                    play_pending = true;

                    update_playlist();
                    current_track_counter++;
                }
                break;
            }

            case MPEG_FF_REWIND: {
                struct mp3entry *id3  = mpeg_current_track();
                unsigned int oldtime  = id3->elapsed;
                unsigned int newtime  = (unsigned int)ev.data;
                int curpos, newpos, diffpos;
                DEBUGF("MPEG_FF_REWIND\n");

                id3->elapsed = newtime;

                if (id3->vbr)
                {
                    if (id3->has_toc)
                    {
                        /* Use the TOC to find the new position */
                        unsigned int percent, remainder;
                        int curtoc, nexttoc, plen;
                        
                        percent = (newtime*100)/id3->length; 
                        if (percent > 99)
                            percent = 99;
                        
                        curtoc = id3->toc[percent];
                        
                        if (percent < 99)
                            nexttoc = id3->toc[percent+1];
                        else
                            nexttoc = 256;
                        
                        newpos = (id3->filesize/256)*curtoc;
                        
                        /* Use the remainder to get a more accurate position */
                        remainder   = (newtime*100)%id3->length;
                        remainder   = (remainder*100)/id3->length;
                        plen        = (nexttoc - curtoc)*(id3->filesize/256);
                        newpos     += (plen/100)*remainder;
                    }
                    else
                    {
                        /* No TOC exists, estimate the new position */
                        newpos = (id3->filesize / (id3->length / 1000)) *
                            (newtime / 1000);
                    }
                }
                else if (id3->bpf && id3->tpf)
                    newpos = (newtime/id3->tpf)*id3->bpf;
                else
                {
                    /* Not enough information to FF/Rewind */
                    id3->elapsed = oldtime;
                    break;
                }

                if (newpos >= (int)(id3->filesize - id3->id3v1len))
                {
                    /* Don't seek right to the end of the file so that we can
                       transition properly to the next song */
                    newpos = id3->filesize - id3->id3v1len - 1;
                }
                else if (newpos < (int)id3->first_frame_offset)
                {
                    /* skip past id3v2 tag and other leading garbage */
                    newpos = id3->first_frame_offset;
                }

                if (mpeg_file >= 0)
                    curpos = lseek(mpeg_file, 0, SEEK_CUR);
                else
                    curpos = id3->filesize;

                if (num_tracks_in_memory() > 1)
                {
                    /* We have started loading other tracks that need to be
                       accounted for */
                    int i = tag_read_idx;
                    int j = tag_write_idx - 1;

                    if (j < 0)
                        j = MAX_ID3_TAGS - 1;

                    while (i != j)
                    {
                        curpos += id3tags[i]->id3.filesize;
                        i = (i+1) & MAX_ID3_TAGS_MASK;
                    }
                }

                diffpos = curpos - newpos;

                if(!filling && diffpos >= 0 && diffpos < mp3buflen)
                {
                    int unplayed_space_left, unswapped_space_left;

                    /* We are changing to a position that's already in
                       memory, so we just move the DMA read pointer. */
                    mp3buf_read = mp3buf_write - diffpos;
                    if (mp3buf_read < 0)
                    {
                        mp3buf_read += mp3buflen;
                    }

                    unplayed_space_left  = get_unplayed_space();
                    unswapped_space_left = get_unswapped_space();

                    /* If unswapped_space_left is larger than
                       unplayed_space_left, it means that the swapwrite pointer
                       hasn't yet advanced up to the new location of the read
                       pointer. We just move it, there is no need to swap
                       data that won't be played anyway. */
                    
                    if (unswapped_space_left > unplayed_space_left)
                    {
                        DEBUGF("Moved swapwrite\n");
                        mp3buf_swapwrite = mp3buf_read;
                        play_pending = true;
                    }

                    if (mpeg_file>=0 && unplayed_space_left < low_watermark)
                    {
                        /* We need to load more data before starting */
                        filling = true;
                        queue_post(&mpeg_queue, MPEG_NEED_DATA, 0);
                        play_pending = true;
                    }
                    else
                    {
                        /* resume will start at new position */
                        last_dma_chunk_size = 
                            MIN(0x2000, get_unplayed_space_current_song());
                        mp3_play_data(mp3buf + mp3buf_read, 
                            last_dma_chunk_size, transfer_end);
                        dma_underrun = false;
                    }
                }
                else
                {
                    /* Move to the new position in the file and start
                       loading data */
                    reset_mp3_buffer();

                    if (num_tracks_in_memory() > 1)
                    {
                        /* We have to reload the current track */
                        close(mpeg_file);
                        remove_all_non_current_tags();
                        mpeg_file = -1;
                    }

                    if (mpeg_file < 0)
                    {
                        mpeg_file = open(id3->path, O_RDONLY);
                        if (mpeg_file < 0)
                        {
                            id3->elapsed = oldtime;
                            break;
                        }
                    }

                    if(-1 == lseek(mpeg_file, newpos, SEEK_SET))
                    {
                        id3->elapsed = oldtime;
                        break;
                    }

                    filling = true;
                    queue_post(&mpeg_queue, MPEG_NEED_DATA, 0);

                    /* Tell the file loading code that we want to start playing
                       as soon as we have some data */
                    play_pending = true;
                }

                id3->offset = newpos;

                break;
            }

            case MPEG_FLUSH_RELOAD: {
                int  numtracks    = num_tracks_in_memory();
                bool reload_track = false;

                if (numtracks > 1)
                {
                    /* Reload songs */
                    int next = (tag_read_idx+1) & MAX_ID3_TAGS_MASK;

                    /* Reset the buffer */
                    mp3buf_write = id3tags[next]->mempos;

                    /* Reset swapwrite unless we're still swapping current
                       track */
                    if (get_unplayed_space() <= get_playable_space())
                        mp3buf_swapwrite = mp3buf_write;

                    close(mpeg_file);
                    remove_all_non_current_tags();
                    mpeg_file = -1;
                    reload_track = true;
                }
                else if (numtracks == 1 && mpeg_file < 0)
                {
                    reload_track = true;
                }

                if(reload_track && new_file(1) >= 0)
                {
                    /* Tell ourselves that we want more data */
                    queue_post(&mpeg_queue, MPEG_NEED_DATA, 0);
                    filling = true;
                }

                break;
            }

            case MPEG_NEED_DATA:
                free_space_left = mp3buf_read - mp3buf_write;

                /* We interpret 0 as "empty buffer" */
                if(free_space_left <= 0)
                    free_space_left = mp3buflen + free_space_left;

                unplayed_space_left = mp3buflen - free_space_left;

                /* Make sure that we don't fill the entire buffer */
                free_space_left -= MPEG_HIGH_WATER;

                /* do we have any more buffer space to fill? */
                if(free_space_left <= MPEG_HIGH_WATER)
                {
                    DEBUGF("0\n");
                    filling = false;
                    ata_sleep();
                    break;
                }

                /* Read small chunks while we are below the low water mark */
                if(unplayed_space_left < low_watermark)
                    amount_to_read = MIN(MPEG_LOW_WATER_CHUNKSIZE,
                                         free_space_left);
                else
                    amount_to_read = free_space_left;

                /* Don't read more than until the end of the buffer */
                amount_to_read = MIN(mp3buflen - mp3buf_write, amount_to_read);
#if MEM == 8    
                amount_to_read = MIN(0x100000, amount_to_read);
#endif

                /* Read as much mpeg data as we can fit in the buffer */
                if(mpeg_file >= 0)
                {
                    DEBUGF("R\n");
                    t1 = current_tick;
                    len = read(mpeg_file, mp3buf+mp3buf_write, amount_to_read);
                    if(len > 0)
                    {
                        t2 = current_tick;
                        DEBUGF("time: %d\n", t2 - t1);
                        DEBUGF("R: %x\n", len);

                        /* Now make sure that we don't feed the MAS with ID3V1
                           data */
                        if (len < amount_to_read)
                        {
                            int tagptr = mp3buf_write + len - 128;
                            int i;
                            char *tag = "TAG";
                            int taglen = 128;
                            
                            for(i = 0;i < 3;i++)
                            {
                                if(tagptr >= mp3buflen)
                                    tagptr -= mp3buflen;

                                if(mp3buf[tagptr] != tag[i])
                                    taglen = 0;

                                tagptr++;
                            }

                            if(taglen)
                            {
                                /* Skip id3v1 tag */
                                DEBUGF("Skipping ID3v1 tag\n");
                                len -= taglen;

                                /* The very rare case when the entire tag
                                   wasn't read in this read() call must be
                                   taken care of */
                                if(len < 0)
                                    len = 0;
                            }
                        }

                        mp3buf_write += len;
                        
                        if(mp3buf_write >= mp3buflen)
                        {
                            mp3buf_write = 0;
                            DEBUGF("W\n");
                        }

                        /* Tell ourselves that we want more data */
                        queue_post(&mpeg_queue, MPEG_NEED_DATA, 0);
                    }
                    else
                    {
                        if(len < 0)
                        {
                            DEBUGF("MPEG read error\n");
                        }
                        
                        close(mpeg_file);
                        mpeg_file = -1;
                    
                        if(new_file(1) < 0)
                        {
                            /* No more data to play */
                            DEBUGF("No more files to play\n");
                            filling = false;
                        }
                        else
                        {
                            /* Tell ourselves that we want more data */
                            queue_post(&mpeg_queue, MPEG_NEED_DATA, 0);
                        }
                    }
                }
                break;
                
            case MPEG_TRACK_CHANGE:
                track_change();
                break;

#ifndef USB_NONE
            case SYS_USB_CONNECTED:
                is_playing = false;
                paused = false;
                stop_playing();
#ifndef SIMULATOR
                
                /* Tell the USB thread that we are safe */
                DEBUGF("mpeg_thread got SYS_USB_CONNECTED\n");
                usb_acknowledge(SYS_USB_CONNECTED_ACK);

                /* Wait until the USB cable is extracted again */
                usb_wait_for_disconnect(&mpeg_queue);
#endif
                break;
#endif /* USB_NONE */
                
#ifdef HAVE_MAS3587F
            case MPEG_INIT_RECORDING:
                init_recording();
                init_recording_done = true;
                break;
#endif
            }
#ifdef HAVE_MAS3587F
        }
        else
        {
            queue_wait(&mpeg_queue, &ev);
            switch(ev.id)
            {
                case MPEG_RECORD:
                    if(is_prerecording)
                    {
                        int startpos, i;
                        
                        /* Go back prerecord_count seconds in the buffer */
                        startpos = prerecord_index - prerecord_count;
                        if(startpos < 0)
                            startpos += prerecording_max_seconds;

                        /* Read the mp3 buffer pointer from the prerecord buffer */
                        startpos = prerecord_buffer[startpos];

                        DEBUGF("Start looking at address %x (%x)\n",
                               mp3buf+startpos, startpos);

                        saved_header = get_last_recorded_header();
                        
                        mem_find_next_frame(startpos, &offset, 5000,
                                            saved_header);

                        mp3buf_read = startpos + offset;
                        
                        DEBUGF("New mp3buf_read address: %x (%x)\n",
                               mp3buf+mp3buf_read, mp3buf_read);

                        /* Make room for headers */
                        mp3buf_read -= MPEG_RESERVED_HEADER_SPACE;
                        if(mp3buf_read < 0)
                        {
                            /* Clear the bottom half */
                            memset(mp3buf, 0,
                                   mp3buf_read + MPEG_RESERVED_HEADER_SPACE);

                            /* And the top half */
                            mp3buf_read += mp3buflen;
                            memset(mp3buf + mp3buf_read, 0,
                                   mp3buflen - mp3buf_read);
                        }
                        else
                        {
                            memset(mp3buf + mp3buf_read, 0,
                                   MPEG_RESERVED_HEADER_SPACE);
                        }

                        /* Copy the empty ID3 header */
                        startpos = mp3buf_read;
                        for(i = 0;i < (int)sizeof(empty_id3_header);i++)
                        {
                            mp3buf[startpos++] = empty_id3_header[i];
                            if(startpos == mp3buflen)
                                startpos = 0;
                        }
                        
                        DEBUGF("New mp3buf_read address (reservation): %x\n",
                               mp3buf+mp3buf_read);
                        
                        DEBUGF("Prerecording...\n");
                    }
                    else
                    {
                        reset_mp3_buffer();

                        num_rec_bytes = 0;
                        
                        /* Advance the write pointer to make
                           room for an ID3 tag plus a VBR header */
                        mp3buf_write = MPEG_RESERVED_HEADER_SPACE;
                        memset(mp3buf, 0, MPEG_RESERVED_HEADER_SPACE);
                        
                        /* Insert the ID3 header */
                        memcpy(mp3buf, empty_id3_header,
                               sizeof(empty_id3_header));

                        DEBUGF("Recording...\n");
                    }

                    start_recording();

                    mpeg_file = open(recording_filename, O_WRONLY|O_CREAT);
                    
                    if(mpeg_file < 0)
                        panicf("recfile: %d", mpeg_file);

                    break;

                case MPEG_STOP:
                    DEBUGF("MPEG_STOP\n");

                    /* Store the last recorded header for later use by the
                       Xing header generation */
                    saved_header = get_last_recorded_header();
                    
                    stop_recording();

                    /* Save the remaining data in the buffer */
                    stop_pending = true;
                    queue_post(&mpeg_queue, MPEG_SAVE_DATA, 0);
                    break;
                    
                case MPEG_STOP_DONE:
                    DEBUGF("MPEG_STOP_DONE\n");
                    
                    if(mpeg_file >= 0)
                        close(mpeg_file);

                    if(!disable_xing_header)
                    {
                        /* Create the Xing header */
                        mpeg_file = open(recording_filename, O_RDWR);
                        if(mpeg_file < 0)
                            panicf("rec upd: %d (%s)", mpeg_file,
                                   recording_filename);

                        /* If the number of recorded frames have
                           reached 0x7ffff, we can no longer trust it */
                        if(num_recorded_frames == 0x7ffff)
                            num_recorded_frames = 0;

                        /* Also, if we have been prerecording, the frame count
                           will be wrong */
                        if(prerecording)
                            num_recorded_frames = 0;
                        
                        /* saved_header is saved right before stopping
                           the MAS */
                        framelen = create_xing_header(mpeg_file, 0,
                                                      num_rec_bytes, mp3buf,
                                                      num_recorded_frames,
                                                      saved_header, NULL,
                                                      false);
                        
                        lseek(mpeg_file, MPEG_RESERVED_HEADER_SPACE-framelen,
                              SEEK_SET);
                        write(mpeg_file, mp3buf, framelen);
                        close(mpeg_file);
                    }
                    mpeg_file = -1;
                    
#ifdef DEBUG1
                    {
                        int i;
                        for(i = 0;i < 512;i++)
                        {
                            DEBUGF("%d - %d us (%d bytes)\n",
                                   timing_info[i*2],
                                   (timing_info[i*2+1] & 0xffff) *
                                   10000 / 13824,
                                   timing_info[i*2+1] >> 16);
                        }
                    }
#endif

                    if(prerecording)
                    {
                        start_prerecording();
                    }
                    mpeg_stop_done = true;
                    break;

                case MPEG_NEW_FILE:
                    /* Make sure we have at least one complete frame
                       in the buffer. If we haven't recorded a single
                       frame within 200ms, the MAS is probably not recording
                       anything, and we bail out. */
                    countdown = 20;
                    amount_to_save = get_unsaved_space();
                    while(countdown-- && amount_to_save < 1800)
                    {
                        sleep(HZ/10);
                        amount_to_save = get_unsaved_space();
                    }

                    if(amount_to_save >= 1800)
                    {
                        /* Now find a frame boundary to split at */
                        startpos = mp3buf_write - 1800;
                        if(startpos < 0)
                            startpos += mp3buflen;

                        saved_header = get_last_recorded_header();
                        
                        rc = mem_find_next_frame(startpos, &offset, 1800,
                                                 saved_header);
                        if(rc) /* Header found? */
                        {
                            /* offset will now contain the number of bytes to
                               add to startpos to find the frame boundary */
                            startpos += offset;
                            if(startpos >= mp3buflen)
                                startpos -= mp3buflen;
                        }
                        else
                        {
                            /* No header found. Let's save the whole buffer. */
                            startpos = mp3buf_write;
                        }
                    }
                    else
                    {
                        /* Too few bytes recorded, timeout */
                        startpos = mp3buf_write;
                    }
                    
                    amount_to_save = startpos - mp3buf_read;
                        if(amount_to_save < 0)
                            amount_to_save += mp3buflen;

                    /* First save up to the end of the buffer */
                    writelen = MIN(amount_to_save,
                                   mp3buflen - mp3buf_read);
                    
                    if(writelen)
                    {
                        rc = write(mpeg_file, mp3buf + mp3buf_read, writelen);
                        if(rc < 0)
                        {
                            if(errno == ENOSPC)
                            {
                                mpeg_errno = MPEGERR_DISK_FULL;
                                demand_irq_enable(false);
                                stop_recording();
                                queue_post(&mpeg_queue, MPEG_STOP_DONE, 0);
                                break;
                            }
                            else
                            {
                                panicf("spt wrt: %d", rc);
                            }
                        }
                    }

                    /* Then save the rest */
                    writelen =  amount_to_save - writelen;
                    if(writelen)
                    {
                        rc = write(mpeg_file, mp3buf, writelen);
                        if(rc < 0)
                        {
                            if(errno == ENOSPC)
                            {
                                mpeg_errno = MPEGERR_DISK_FULL;
                                demand_irq_enable(false);
                                stop_recording();
                                queue_post(&mpeg_queue, MPEG_STOP_DONE, 0);
                                break;
                            }
                            else
                            {
                                panicf("spt wrt: %d", rc);
                            }
                        }
                    }

                    /* Advance the buffer pointers */
                    mp3buf_read += amount_to_save;
                    if(mp3buf_read >= mp3buflen)
                        mp3buf_read -= mp3buflen;

                    /* Close the current file */
                    rc = close(mpeg_file);
                    if(rc < 0)
                        panicf("spt cls: %d", rc);

                    /* Open the new file */
                    mpeg_file = open(recording_filename, O_WRONLY|O_CREAT);
                    if(mpeg_file < 0)
                        panicf("sptfile: %d", mpeg_file);
                    break;
                    
                case MPEG_SAVE_DATA:
                    amount_to_save = get_unsaved_space();
                    
                    /* If the result is negative, the write index has
                       wrapped */
                    if(amount_to_save < 0)
                    {
                        amount_to_save += mp3buflen;
                    }
                    
                    DEBUGF("r: %x w: %x\n", mp3buf_read, mp3buf_write);
                    DEBUGF("ats: %x\n", amount_to_save);
                    /* Save data only if the buffer is getting full,
                       or if we should stop recording */
                    if(amount_to_save)
                    {
                        if(mp3buflen -
                           amount_to_save < MPEG_RECORDING_LOW_WATER ||
                           stop_pending)
                        {
                            /* Only save up to the end of the buffer */
                            writelen = MIN(amount_to_save,
                                           mp3buflen - mp3buf_read);
                            
                            DEBUGF("wrl: %x\n", writelen);
                            
                            rc = write(mpeg_file, mp3buf + mp3buf_read,
                                       writelen);
                            
                            if(rc < 0)
                            {
                                if(errno == ENOSPC)
                                {
                                    mpeg_errno = MPEGERR_DISK_FULL;
                                    stop_recording();
                                    queue_post(&mpeg_queue, MPEG_STOP_DONE, 0);
                                    break;
                                }
                                else
                                {
                                    panicf("rec wrt: %d", rc);
                                }
                            }
                            
                            mp3buf_read += amount_to_save;
                            if(mp3buf_read >= mp3buflen)
                                mp3buf_read = 0;
                            
                            rc = fsync(mpeg_file);
                            if(rc < 0)
                                panicf("rec fls: %d", rc);

                            queue_post(&mpeg_queue, MPEG_SAVE_DATA, 0);
                        }
                        else
                        {
                            saving = false;
                            ata_sleep();
                        }
                    }
                    else
                    {
                        /* We have saved all data,
                           time to stop for real */
                        if(stop_pending)
                            queue_post(&mpeg_queue, MPEG_STOP_DONE, 0);
                        saving = false;
                        ata_sleep();
                    }
                    break;
                    
                case MPEG_INIT_PLAYBACK:
                    mp3_play_init();
                    mpeg_mode = MPEG_DECODER;

                    init_playback_done = true;
                    break;

                case SYS_USB_CONNECTED:
                    /* We can safely go to USB mode if no recording
                       is taking place */
                    if((!is_recording || is_prerecording) && mpeg_stop_done)
                    {
                        /* Even if we aren't recording, we still call this
                           function, to put the MAS in monitoring mode,
                           to save power. */
                        stop_recording();
                
                        /* Tell the USB thread that we are safe */
                        DEBUGF("mpeg_thread got SYS_USB_CONNECTED\n");
                        usb_acknowledge(SYS_USB_CONNECTED_ACK);
                    
                        /* Wait until the USB cable is extracted again */
                        usb_wait_for_disconnect(&mpeg_queue);
                    }
                    break;
            }
        }
#endif
    }
}
#endif /* SIMULATOR */

#ifdef SIMULATOR
static struct mp3entry taginfo;
#endif

struct mp3entry* mpeg_current_track()
{
#ifdef SIMULATOR
    return &taginfo;
#else
    if(num_tracks_in_memory())
        return &(id3tags[tag_read_idx]->id3);
    else
        return NULL;
#endif
}

bool mpeg_has_changed_track(void)
{
    if(last_track_counter != current_track_counter)
    {
        last_track_counter = current_track_counter;
        return true;
    }
    return false;
}

#ifdef HAVE_MAS3587F
void mpeg_init_playback(void)
{
    init_playback_done = false;
    queue_post(&mpeg_queue, MPEG_INIT_PLAYBACK, NULL);

    while(!init_playback_done)
        sleep_thread();
    wake_up_thread();
}


/****************************************************************************
 **
 **
 ** Recording functions
 **
 **
 ***************************************************************************/
void mpeg_init_recording(void)
{
    init_recording_done = false;
    queue_post(&mpeg_queue, MPEG_INIT_RECORDING, NULL);

    while(!init_recording_done)
        sleep_thread();
    wake_up_thread();
}

static void init_recording(void)
{
    unsigned long val;
    int rc;

    /* Disable IRQ6 */
    IPRB &= 0xff0f;

    stop_playing();
    is_playing = false;
    paused = false;

    reset_mp3_buffer();
    remove_all_tags();
    
    if(mpeg_file >= 0)
        close(mpeg_file);
    mpeg_file = -1;

    /* Init the recording variables */
    is_recording = false;
    is_prerecording = false;

    mpeg_stop_done = true;
    
    mas_reset();
    
    /* Enable the audio CODEC and the DSP core, max analog voltage range */
    rc = mas_direct_config_write(MAS_CONTROL, 0x8c00);
    if(rc < 0)
        panicf("mas_ctrl_w: %d", rc);

    /* Stop the current application */
    val = 0;
    mas_writemem(MAS_BANK_D0,0x7f6,&val,1);    
    do
    {
        mas_readmem(MAS_BANK_D0, 0x7f7, &val, 1);
    } while(val);

    /* Perform black magic as described by the data sheet */
    if((mas_version_code & 0xff) == 2)
    {
        DEBUGF("Performing MAS black magic for B2 version\n");
        mas_writereg(0xa3, 0x98);
        mas_writereg(0x94, 0xfffff);
        val = 0;
        mas_writemem(MAS_BANK_D1, 0, &val, 1);
        mas_writereg(0xa3, 0x90);
    }

    /* Enable A/D Converters */
    mas_codec_writereg(0x0, 0xcccd);

    /* Copy left channel to right (mono mode) */
    mas_codec_writereg(8, 0x8000);
    
    /* ADC scale 0%, DSP scale 100%
       We use the DSP output for monitoring, because it works with all
       sources including S/PDIF */
    mas_codec_writereg(6, 0x0000);
    mas_codec_writereg(7, 0x4000);

    /* No mute */
    val = 0;
    mas_writemem(MAS_BANK_D0, 0x7f9, &val, 1);
    
    /* Set Demand mode, no monitoring and validate all settings */
    val = 0x125;
    mas_writemem(MAS_BANK_D0, 0x7f1, &val, 1);

    /* Start the encoder application */
    val = 0x40;
    mas_writemem(MAS_BANK_D0, 0x7f6, &val, 1);
    do
    {
        mas_readmem(MAS_BANK_D0, 0x7f7, &val, 1);
    } while(!(val & 0x40));

    /* We have started the recording application with monitoring OFF.
       This is because we want to record at least one frame to fill the DMA
       buffer, because the silly MAS will not negate EOD until at least one
       DMA transfer has taken place.
       Now let's wait for some data to be encoded. */
    sleep(20);
    
    /* Now set it to Monitoring mode as default, saves power */
    val = 0x525;
    mas_writemem(MAS_BANK_D0, 0x7f1, &val, 1);

    mpeg_mode = MPEG_ENCODER;

    DEBUGF("MAS Recording application started\n");

    /* At this point, all settings are the reset MAS defaults, next thing is to
       call mpeg_set_recording_options(). */
}

void mpeg_record(char *filename)
{
    mpeg_errno = 0;
    
    strncpy(recording_filename, filename, MAX_PATH - 1);
    recording_filename[MAX_PATH - 1] = 0;
    
    disable_xing_header = false;
    queue_post(&mpeg_queue, MPEG_RECORD, NULL);
}

static void start_prerecording(void)
{
    unsigned long val;

    DEBUGF("Starting prerecording\n");
    
    prerecord_index = 0;
    prerecord_count = 0;
    prerecord_timeout = current_tick + HZ;
    memset(prerecord_buffer, 0, sizeof(prerecord_buffer));
    reset_mp3_buffer();
    
    is_prerecording = true;

    /* Stop monitoring and start the encoder */
    mas_readmem(MAS_BANK_D0, 0x7f1, &val, 1);
    val &= ~(1 << 10);
    val |= 1;
    mas_writemem(MAS_BANK_D0, 0x7f1, &val, 1);
    DEBUGF("mas_writemem(MAS_BANK_D0, 0x7f1, %x)\n", val);

    /* Wait until the DSP has accepted the settings */
    do
    {
        mas_readmem(MAS_BANK_D0, 0x7f1, &val,1);
    } while(val & 1);
    
    sleep(20);

    is_recording = true;
    stop_pending = false;
    saving = false;

    demand_irq_enable(true);
}

static void start_recording(void)
{
    unsigned long val;

    num_recorded_frames = 0;

    if(is_prerecording)
    {
        /* This will make the IRQ handler start recording
           for real, i.e send MPEG_SAVE_DATA messages when
           the buffer is full */
        is_prerecording = false;
    }
    else
    {
        /* If prerecording is off, we need to stop the monitoring
           and start the encoder */
        mas_readmem(MAS_BANK_D0, 0x7f1, &val, 1);
        val &= ~(1 << 10);
        val |= 1;
        mas_writemem(MAS_BANK_D0, 0x7f1, &val, 1);
        DEBUGF("mas_writemem(MAS_BANK_D0, 0x7f1, %x)\n", val);
        
        /* Wait until the DSP has accepted the settings */
        do
        {
            mas_readmem(MAS_BANK_D0, 0x7f1, &val,1);
        } while(val & 1);
        
        sleep(20);
    }
    
    is_recording = true;
    stop_pending = false;
    saving = false;

    /* Store the current time */
    if(prerecording)
        record_start_time = current_tick - prerecord_count * HZ;
    else
        record_start_time = current_tick;
        
    demand_irq_enable(true);
}

static void stop_recording(void)
{
    unsigned long val;

    demand_irq_enable(false);

    is_recording = false;
    is_prerecording = false;
        
    /* Read the number of frames recorded */
    mas_readmem(MAS_BANK_D0, 0xfd0, &num_recorded_frames, 1);

    /* Start monitoring */
    mas_readmem(MAS_BANK_D0, 0x7f1, &val, 1);
    val |= (1 << 10) | 1;
    mas_writemem(MAS_BANK_D0, 0x7f1, &val, 1);
    DEBUGF("mas_writemem(MAS_BANK_D0, 0x7f1, %x)\n", val);
    
    /* Wait until the DSP has accepted the settings */
    do
    {
        mas_readmem(MAS_BANK_D0, 0x7f1, &val,1);
    } while(val & 1);
    
    drain_dma_buffer();
}

void mpeg_set_recording_options(int frequency, int quality,
                                int source, int channel_mode,
                                bool editable, int prerecord_time)
{
    bool is_mpeg1;
    unsigned long val;

    is_mpeg1 = (frequency < 3)?true:false;

    rec_version_index = is_mpeg1?3:2;
    rec_frequency_index = frequency % 3;
    
    val = (quality << 17) |
        (rec_frequency_index << 10) |
        ((is_mpeg1?1:0) << 9) |
        (1 << 8) | /* CRC on */
        (((channel_mode * 2 + 1) & 3) << 6) |
        (1 << 5) /* MS-stereo */ |
        (1 << 2) /* Is an original */;
    mas_writemem(MAS_BANK_D0, 0x7f0, &val,1);

    DEBUGF("mas_writemem(MAS_BANK_D0, 0x7f0, %x)\n", val);

    val = editable?4:0;
    mas_writemem(MAS_BANK_D0, 0x7f9, &val,1);

    DEBUGF("mas_writemem(MAS_BANK_D0, 0x7f9, %x)\n", val);

    val = (((source < 2)?1:2) << 8) | /* Input select */
        (1 << 5) | /* SDO strobe invert */
        ((is_mpeg1?0:1) << 3) |
        (1 << 2) | /* Inverted SIBC clock signal */
        1; /* Validate */
    mas_writemem(MAS_BANK_D0, 0x7f1, &val,1);

    DEBUGF("mas_writemem(MAS_BANK_D0, 0x7f1, %x)\n", val);

    drain_dma_buffer();
    
    if(source == 0) /* Mic */
    {
        /* Copy left channel to right (mono mode) */
        mas_codec_writereg(8, 0x8000);
    }
    else
    {
        /* Stereo input mode */
        mas_codec_writereg(8, 0);
    }

    prerecording_max_seconds = prerecord_time;
    if(prerecording_max_seconds)
    {
        prerecording = true;
        start_prerecording();
    }
    else
    {
        prerecording = false;
        is_prerecording = false;
        is_recording = false;
    }
}

/* If use_mic is true, the left gain is used */
void mpeg_set_recording_gain(int left, int right, bool use_mic)
{
    /* Enable both left and right A/D */
    mas_codec_writereg(0x0,
                       (left << 12) |
                       (right << 8) |
                       (left << 4) |
                       (use_mic?0x0008:0) | /* Connect left A/D to mic */
                       0x0007);
}

void mpeg_new_file(char *filename)
{
    mpeg_errno = 0;
    
    strncpy(recording_filename, filename, MAX_PATH - 1);
    recording_filename[MAX_PATH - 1] = 0;

    num_rec_bytes = 0;
    disable_xing_header = true;

    /* Store the current time */
    record_start_time = current_tick;

    queue_post(&mpeg_queue, MPEG_NEW_FILE, NULL);
}

unsigned long mpeg_recorded_time(void)
{
    if(is_prerecording)
        return prerecord_count * HZ;
    
    if(is_recording)
        return current_tick - record_start_time;

    return 0;
}

unsigned long mpeg_num_recorded_bytes(void)
{
    int num_bytes;
    int index;
    
    if(is_recording)
    {
        if(is_prerecording)
        {
            index = prerecord_index - prerecord_count;
            if(index < 0)
                index += prerecording_max_seconds;
            
            num_bytes = mp3buf_write - prerecord_buffer[index];
            if(num_bytes < 0)
                num_bytes += mp3buflen;
            
            return num_bytes;;
        }
        else
            return num_rec_bytes;
    }
    else
        return 0;
}

#endif

void mpeg_play(int offset)
{
#ifdef SIMULATOR
    char* trackname;
    int steps=0;

    is_playing = true;
    
    do {
        trackname = playlist_peek( steps );
        if (!trackname)
            break;
        if(mp3info(&taginfo, trackname)) {
            /* bad mp3, move on */
            if(++steps > playlist_amount())
                break;
            continue;
        }
        playlist_next(steps);
        taginfo.offset = offset;
        set_elapsed(&taginfo);
        is_playing = true;
        playing = true;
        break;
    } while(1);
#else
    is_playing = true;
    
    queue_post(&mpeg_queue, MPEG_PLAY, (void*)offset);
#endif

    mpeg_errno = 0;
}

void mpeg_stop(void)
{
#ifndef SIMULATOR
    mpeg_stop_done = false;
    queue_post(&mpeg_queue, MPEG_STOP, NULL);
    while(!mpeg_stop_done)
        yield();
#else
    is_playing = false;
    playing = false;
#endif
    
}

void mpeg_pause(void)
{
#ifndef SIMULATOR
    queue_post(&mpeg_queue, MPEG_PAUSE, NULL);
#else
    is_playing = true;
    playing = false;
    paused = true;
#endif
}

void mpeg_resume(void)
{
#ifndef SIMULATOR
    queue_post(&mpeg_queue, MPEG_RESUME, NULL);
#else
    is_playing = true;
    playing = true;
    paused = false;
#endif
}

void mpeg_next(void)
{
#ifndef SIMULATOR
    queue_post(&mpeg_queue, MPEG_NEXT, NULL);
#else
    char* file;
    int steps = 1;
    int index;

    do {
        file = playlist_peek(steps);
        if(!file)
            break;
        if(mp3info(&taginfo, file)) {
            if(++steps > playlist_amount())
                break;
            continue;
        }
        index = playlist_next(steps);
        taginfo.index = index;
        current_track_counter++;
        is_playing = true;
        playing = true;
        break;
    } while(1);
#endif
}

void mpeg_prev(void)
{
#ifndef SIMULATOR
    queue_post(&mpeg_queue, MPEG_PREV, NULL);
#else
    char* file;
    int steps = -1;
    int index;

    do {
        file = playlist_peek(steps);
        if(!file)
            break;
        if(mp3info(&taginfo, file)) {
            steps--;
            continue;
        }
        index = playlist_next(steps);
        taginfo.index = index;
        current_track_counter++;
        is_playing = true;
        playing = true;
        break;
    } while(1);
#endif
}

void mpeg_ff_rewind(int newtime)
{
#ifndef SIMULATOR
    queue_post(&mpeg_queue, MPEG_FF_REWIND, (void *)newtime);
#else
    (void)newtime;
#endif
}

void mpeg_flush_and_reload_tracks(void)
{
#ifndef SIMULATOR
    queue_post(&mpeg_queue, MPEG_FLUSH_RELOAD, NULL);
#endif
}

int mpeg_status(void)
{
    int ret = 0;

    if(is_playing)
        ret |= MPEG_STATUS_PLAY;

    if(paused)
        ret |= MPEG_STATUS_PAUSE;
    
#ifdef HAVE_MAS3587F
    if(is_recording && !is_prerecording)
        ret |= MPEG_STATUS_RECORD;

    if(is_prerecording)
        ret |= MPEG_STATUS_PRERECORD;
#endif

    if(mpeg_errno)
        ret |= MPEG_STATUS_ERROR;
    
    return ret;
}

unsigned int mpeg_error(void)
{
    return mpeg_errno;
}

void mpeg_error_clear(void)
{
    mpeg_errno = 0;
}

#ifdef SIMULATOR
static char mpeg_stack[DEFAULT_STACK_SIZE];
static char mpeg_thread_name[] = "mpeg";
static void mpeg_thread(void)
{
    struct mp3entry* id3;
    while ( 1 ) {
        if (is_playing) {
            id3 = mpeg_current_track();
            if (!paused)
            {
                id3->elapsed+=1000;
                id3->offset+=1000;
            }
            if (id3->elapsed>=id3->length)
                mpeg_next();
        }
        sleep(HZ);
    }
}
#endif

void mpeg_init(void)
{
    mpeg_errno = 0;

    mp3buflen = mp3end - mp3buf;

    queue_init(&mpeg_queue);
    create_thread(mpeg_thread, mpeg_stack,
                  sizeof(mpeg_stack), mpeg_thread_name);

    memset(id3tags, sizeof(id3tags), 0);
    memset(_id3tags, sizeof(id3tags), 0);

#ifdef HAVE_MAS3587F
    if(read_hw_mask() & PR_ACTIVE_HIGH)
        inverted_pr = true;
    else
        inverted_pr = false;
#endif
    
#ifdef DEBUG
    dbg_timer_start();
    dbg_cnt2us(0);
#endif
}
