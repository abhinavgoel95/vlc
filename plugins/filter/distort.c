/*****************************************************************************
 * distort.c : Misc video effects plugin for vlc
 *****************************************************************************
 * Copyright (C) 2000, 2001 VideoLAN
 * $Id: distort.c,v 1.4 2002/01/02 14:37:42 sam Exp $
 *
 * Authors: Samuel Hocevar <sam@zoy.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include <errno.h>
#include <stdlib.h>                                      /* malloc(), free() */
#include <string.h>

#include <math.h>                                            /* sin(), cos() */

#include <videolan/vlc.h>

#include "video.h"
#include "video_output.h"

#include "filter_common.h"

#define DISTORT_MODE_WAVE    1
#define DISTORT_MODE_RIPPLE  2

/*****************************************************************************
 * Capabilities defined in the other files.
 *****************************************************************************/
static void vout_getfunctions( function_list_t * p_function_list );

/*****************************************************************************
 * Build configuration tree.
 *****************************************************************************/
MODULE_CONFIG_START
MODULE_CONFIG_STOP

MODULE_INIT_START
    SET_DESCRIPTION( "miscellaneous video effects module" )
    /* Capability score set to 0 because we don't want to be spawned
     * as a video output unless explicitly requested to */
    ADD_CAPABILITY( VOUT, 0 )
    ADD_SHORTCUT( "distort" )
MODULE_INIT_STOP

MODULE_ACTIVATE_START
    vout_getfunctions( &p_module->p_functions->vout );
MODULE_ACTIVATE_STOP

MODULE_DEACTIVATE_START
MODULE_DEACTIVATE_STOP

/*****************************************************************************
 * vout_sys_t: Distort video output method descriptor
 *****************************************************************************
 * This structure is part of the video output thread descriptor.
 * It describes the Distort specific properties of an output thread.
 *****************************************************************************/
typedef struct vout_sys_s
{
    int i_mode;
    struct vout_thread_s *p_vout;

    /* For the wave mode */
    double  f_angle;
    mtime_t last_date;

} vout_sys_t;

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static int  vout_Probe     ( probedata_t *p_data );
static int  vout_Create    ( struct vout_thread_s * );
static int  vout_Init      ( struct vout_thread_s * );
static void vout_End       ( struct vout_thread_s * );
static void vout_Destroy   ( struct vout_thread_s * );
static int  vout_Manage    ( struct vout_thread_s * );
static void vout_Display   ( struct vout_thread_s *, struct picture_s * );

static void DistortWave    ( struct vout_thread_s *, struct picture_s *,
                                                     struct picture_s * );
static void DistortRipple  ( struct vout_thread_s *, struct picture_s *,
                                                     struct picture_s * );

/*****************************************************************************
 * Functions exported as capabilities. They are declared as static so that
 * we don't pollute the namespace too much.
 *****************************************************************************/
static void vout_getfunctions( function_list_t * p_function_list )
{
    p_function_list->pf_probe = vout_Probe;
    p_function_list->functions.vout.pf_create     = vout_Create;
    p_function_list->functions.vout.pf_init       = vout_Init;
    p_function_list->functions.vout.pf_end        = vout_End;
    p_function_list->functions.vout.pf_destroy    = vout_Destroy;
    p_function_list->functions.vout.pf_manage     = vout_Manage;
    p_function_list->functions.vout.pf_display    = vout_Display;
    p_function_list->functions.vout.pf_setpalette = NULL;
}

/*****************************************************************************
 * intf_Probe: return a score
 *****************************************************************************/
static int vout_Probe( probedata_t *p_data )
{
    return( 0 );
}

/*****************************************************************************
 * vout_Create: allocates Distort video thread output method
 *****************************************************************************
 * This function allocates and initializes a Distort vout method.
 *****************************************************************************/
static int vout_Create( vout_thread_t *p_vout )
{
    char *psz_method;

    /* Allocate structure */
    p_vout->p_sys = malloc( sizeof( vout_sys_t ) );
    if( p_vout->p_sys == NULL )
    {
        intf_ErrMsg("error: %s", strerror(ENOMEM) );
        return( 1 );
    }

    /* Look what method was requested */
    psz_method = main_GetPszVariable( VOUT_FILTER_VAR, "" );

    while( *psz_method && *psz_method != ':' )
    {
        psz_method++;
    }

    if( !strcmp( psz_method, ":wave" ) )
    {
        p_vout->p_sys->i_mode = DISTORT_MODE_WAVE;
    }
    else if( !strcmp( psz_method, ":ripple" ) )
    {
        p_vout->p_sys->i_mode = DISTORT_MODE_RIPPLE;
    }
    else
    {
        intf_ErrMsg( "filter error: no valid distort mode provided, "
                     "using distort:wave" );
        p_vout->p_sys->i_mode = DISTORT_MODE_WAVE;
    }

    return( 0 );
}

/*****************************************************************************
 * vout_Init: initialize Distort video thread output method
 *****************************************************************************/
static int vout_Init( vout_thread_t *p_vout )
{
    int i_index;
    char *psz_filter;
    picture_t *p_pic;

    I_OUTPUTPICTURES = 0;

    /* Initialize the output structure */
    switch( p_vout->render.i_chroma )
    {
        case FOURCC_I420:
        case FOURCC_IYUV:
        case FOURCC_YV12:
        case FOURCC_I422:
        case FOURCC_I444:
            p_vout->output.i_chroma = p_vout->render.i_chroma;
            p_vout->output.i_width  = p_vout->render.i_width;
            p_vout->output.i_height = p_vout->render.i_height;
            p_vout->output.i_aspect = p_vout->render.i_aspect;
            break;

        default:
            return( 0 ); /* unknown chroma */
            break;
    }

    /* Try to open the real video output */
    psz_filter = main_GetPszVariable( VOUT_FILTER_VAR, "" );
    main_PutPszVariable( VOUT_FILTER_VAR, "" );

    intf_WarnMsg( 1, "filter: spawning the real video output" );

    p_vout->p_sys->p_vout =
        vout_CreateThread( NULL,
                           p_vout->render.i_width, p_vout->render.i_height,
                           p_vout->render.i_chroma, p_vout->render.i_aspect );

    /* Everything failed */
    if( p_vout->p_sys->p_vout == NULL )
    {
        intf_ErrMsg( "filter error: can't open vout, aborting" );

        return( 0 );
    }
 
    main_PutPszVariable( VOUT_FILTER_VAR, psz_filter );

    ALLOCATE_DIRECTBUFFERS( VOUT_MAX_PICTURES );

    p_vout->p_sys->f_angle = 0.0;
    p_vout->p_sys->last_date = 0;

    return( 0 );
}

/*****************************************************************************
 * vout_End: terminate Distort video thread output method
 *****************************************************************************/
static void vout_End( vout_thread_t *p_vout )
{
    int i_index;

    /* Free the fake output buffers we allocated */
    for( i_index = I_OUTPUTPICTURES ; i_index ; )
    {
        i_index--;
        free( PP_OUTPUTPICTURE[ i_index ]->planes[ 0 ].p_data );
    }
}

/*****************************************************************************
 * vout_Destroy: destroy Distort video thread output method
 *****************************************************************************
 * Terminate an output method created by DistortCreateOutputMethod
 *****************************************************************************/
static void vout_Destroy( vout_thread_t *p_vout )
{
    vout_DestroyThread( p_vout->p_sys->p_vout, NULL );

    free( p_vout->p_sys );
}

/*****************************************************************************
 * vout_Manage: handle Distort events
 *****************************************************************************
 * This function should be called regularly by video output thread. It manages
 * console events. It returns a non null value on error.
 *****************************************************************************/
static int vout_Manage( vout_thread_t *p_vout )
{
    return( 0 );
}

/*****************************************************************************
 * vout_Display: displays previously rendered output
 *****************************************************************************
 * This function send the currently rendered image to Distort image, waits
 * until it is displayed and switch the two rendering buffers, preparing next
 * frame.
 *****************************************************************************/
static void vout_Display( vout_thread_t *p_vout, picture_t *p_pic )
{
    picture_t *p_outpic;

    /* This is a new frame. Get a structure from the video_output. */
    while( ( p_outpic = vout_CreatePicture( p_vout->p_sys->p_vout, 0, 0, 0 ) )
              == NULL )
    {
        if( p_vout->b_die || p_vout->b_error )
        {
            return;
        }
        msleep( VOUT_OUTMEM_SLEEP );
    }

    vout_DatePicture( p_vout->p_sys->p_vout, p_outpic, mdate() + 50000 );

    switch( p_vout->p_sys->i_mode )
    {
        case DISTORT_MODE_WAVE:
            DistortWave( p_vout, p_pic, p_outpic );
            break;

        case DISTORT_MODE_RIPPLE:
            DistortRipple( p_vout, p_pic, p_outpic );
            break;

        default:
            break;
    }

    vout_DisplayPicture( p_vout->p_sys->p_vout, p_outpic );
}


/*****************************************************************************
 * DistortWave: draw a wave effect on the picture
 *****************************************************************************/
static void DistortWave( vout_thread_t *p_vout, picture_t *p_inpic,
                                                picture_t *p_outpic )
{
    int i_index;
    double f_angle;
    mtime_t new_date = mdate();

    p_vout->p_sys->f_angle += (new_date - p_vout->p_sys->last_date) / 200000.0;
    p_vout->p_sys->last_date = new_date;
    f_angle = p_vout->p_sys->f_angle;

    for( i_index = 0 ; i_index < p_inpic->i_planes ; i_index++ )
    {
        int i_line, i_num_lines, i_offset;
        u8 black_pixel;
        pixel_data_t *p_in, *p_out;

        p_in = p_inpic->planes[ i_index ].p_data;
        p_out = p_outpic->planes[ i_index ].p_data;

        i_num_lines = p_inpic->planes[ i_index ].i_bytes
                          / p_inpic->planes[ i_index ].i_line_bytes;

        black_pixel = ( i_index == Y_PLANE ) ? 0x00 : 0x80;

        /* Ok, we do 3 times the sin() calculation for each line. So what ? */
        for( i_line = 0 ; i_line < i_num_lines ; i_line++ )
        {
            /* Calculate today's offset, don't go above 1/20th of the screen */
            i_offset = (double)(p_inpic->planes[ i_index ].i_line_bytes)
                         * sin( f_angle + 10.0 * (double)i_line
                                               / (double)i_num_lines )
                         / 20.0;

            if( i_offset )
            {
                if( i_offset < 0 )
                {
                    FAST_MEMCPY( p_out, p_in - i_offset,
                         p_inpic->planes[ i_index ].i_line_bytes + i_offset );
                    p_in += p_inpic->planes[ i_index ].i_line_bytes;
                    p_out += p_outpic->planes[ i_index ].i_line_bytes;
                    memset( p_out + i_offset, black_pixel, -i_offset );
                }
                else
                {
                    FAST_MEMCPY( p_out + i_offset, p_in,
                         p_inpic->planes[ i_index ].i_line_bytes - i_offset );
                    memset( p_out, black_pixel, i_offset );
                    p_in += p_inpic->planes[ i_index ].i_line_bytes;
                    p_out += p_outpic->planes[ i_index ].i_line_bytes;
                }
            }
            else
            {
                FAST_MEMCPY( p_out, p_in,
                             p_inpic->planes[ i_index ].i_line_bytes );
                p_in += p_inpic->planes[ i_index ].i_line_bytes;
                p_out += p_outpic->planes[ i_index ].i_line_bytes;
            }

        }
    }
}

/*****************************************************************************
 * DistortRipple: draw a ripple effect at the bottom of the picture
 *****************************************************************************/
static void DistortRipple( vout_thread_t *p_vout, picture_t *p_inpic,
                                                  picture_t *p_outpic )
{
    int i_index;
    double f_angle;
    mtime_t new_date = mdate();

    p_vout->p_sys->f_angle -= (p_vout->p_sys->last_date - new_date) / 100000.0;
    p_vout->p_sys->last_date = new_date;
    f_angle = p_vout->p_sys->f_angle;

    for( i_index = 0 ; i_index < p_inpic->i_planes ; i_index++ )
    {
        int i_line, i_first_line, i_num_lines, i_offset;
        u8 black_pixel;
        pixel_data_t *p_in, *p_out;

        black_pixel = ( i_index == Y_PLANE ) ? 0x00 : 0x80;

        i_num_lines = p_inpic->planes[ i_index ].i_bytes
                          / p_inpic->planes[ i_index ].i_line_bytes;

        i_first_line = i_num_lines * 4 / 5;

        p_in = p_inpic->planes[ i_index ].p_data;
        p_out = p_outpic->planes[ i_index ].p_data;

        FAST_MEMCPY( p_out, p_in,
                     i_first_line * p_inpic->planes[ i_index ].i_line_bytes );

        p_in += i_first_line * p_inpic->planes[ i_index ].i_line_bytes;
        p_out += i_first_line * p_outpic->planes[ i_index ].i_line_bytes;

        /* Ok, we do 3 times the sin() calculation for each line. So what ? */
        for( i_line = i_first_line ; i_line < i_num_lines ; i_line++ )
        {
            /* Calculate today's offset, don't go above 1/20th of the screen */
            i_offset = (double)(p_inpic->planes[ i_index ].i_line_bytes)
                         * sin( f_angle + 2.0 * (double)i_line
                                              / (double)( 1 + i_line
                                                            - i_first_line) )
                         * (double)(i_line - i_first_line)
                         / (double)i_num_lines
                         / 8.0;

            if( i_offset )
            {
                if( i_offset < 0 )
                {
                    FAST_MEMCPY( p_out, p_in - i_offset,
                         p_inpic->planes[ i_index ].i_line_bytes + i_offset );
                    p_in -= p_inpic->planes[ i_index ].i_line_bytes;
                    p_out += p_outpic->planes[ i_index ].i_line_bytes;
                    memset( p_out + i_offset, black_pixel, -i_offset );
                }
                else
                {
                    FAST_MEMCPY( p_out + i_offset, p_in,
                         p_inpic->planes[ i_index ].i_line_bytes - i_offset );
                    memset( p_out, black_pixel, i_offset );
                    p_in -= p_inpic->planes[ i_index ].i_line_bytes;
                    p_out += p_outpic->planes[ i_index ].i_line_bytes;
                }
            }
            else
            {
                FAST_MEMCPY( p_out, p_in,
                             p_inpic->planes[ i_index ].i_line_bytes );
                p_in -= p_inpic->planes[ i_index ].i_line_bytes;
                p_out += p_outpic->planes[ i_index ].i_line_bytes;
            }

        }
    }
}

