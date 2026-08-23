// Emacs style mode select   -*- C++ -*- 
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
//
// This source is available for distribution and/or modification
// only under the terms of the DOOM Source Code License as
// published by id Software. All rights reserved.
//
// The source is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// FITNESS FOR A PARTICULAR PURPOSE. See the DOOM Source Code License
// for more details.
//
//
// $Log:$
//
// DESCRIPTION:
//	Main loop menu stuff.
//	Default Config File.
//	PCX Screenshots.
//
//-----------------------------------------------------------------------------

static const char
rcsid[] = "$Id: m_misc.c,v 1.6 1997/02/03 22:45:10 b1 Exp $";

#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>

#include <ctype.h>


#include "doomdef.h"

#include "z_zone.h"

#include "m_swap.h"
#include "m_argv.h"

#include "w_wad.h"

#include "i_system.h"
#include "i_video.h"
#include "v_video.h"

#include "hu_stuff.h"

// State.
#include "doomstat.h"

// Data.
#include "dstrings.h"

#include "m_misc.h"

//
// M_DrawText
// Returns the final X coordinate
// HU_Init must have been called to init the font
//
extern patch_t*		hu_font[HU_FONTSIZE];

int
M_DrawText
( int		x,
  int		y,
  boolean	direct,
  char*		string )
{
    int 	c;
    int		w;

    while (*string)
    {
	c = toupper(*string) - HU_FONTSTART;
	string++;
	if (c < 0 || c> HU_FONTSIZE)
	{
	    x += 4;
	    continue;
	}
		
	w = SHORT (hu_font[c]->width);
	if (x+w > SCREENWIDTH)
	    break;
	if (direct)
	    V_DrawPatchDirect(x, y, 0, hu_font[c]);
	else
	    V_DrawPatch(x, y, 0, hu_font[c]);
	x+=w;
    }

    return x;
}




//
// M_WriteFile
//
#ifndef O_BINARY
#define O_BINARY 0
#endif

boolean
M_WriteFile
( char const*	name,
  void*		source,
  int		length )
{
    int		handle;
    int		count;
	
    handle = open ( name, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0666);

    if (handle == -1)
	return false;

    count = write (handle, source, length);
    close (handle);
	
    if (count < length)
	return false;
		
    return true;
}


//
// M_ReadFile
//
int
M_ReadFile
( char const*	name,
  byte**	buffer )
{
    int	handle, count, length;
    struct stat	fileinfo;
    byte		*buf;
	
    handle = open (name, O_RDONLY | O_BINARY, 0666);
    if (handle == -1)
	I_Error ("Couldn't read file %s", name);
    if (fstat (handle,&fileinfo) == -1)
	I_Error ("Couldn't read file %s", name);
    length = fileinfo.st_size;
    buf = Z_Malloc (length, PU_STATIC, NULL);
    count = read (handle, buf, length);
    close (handle);
	
    if (count < length)
	I_Error ("Couldn't read file %s", name);
		
    *buffer = buf;
    return length;
}


//
// DEFAULTS
//
int		usemouse;
int		usejoystick;

extern int	key_right;
extern int	key_left;
extern int	key_up;
extern int	key_down;

extern int	key_strafeleft;
extern int	key_straferight;

extern int	key_fire;
extern int	key_use;
extern int	key_strafe;
extern int	key_speed;

extern int	mousebfire;
extern int	mousebstrafe;
extern int	mousebforward;

extern int	joybfire;
extern int	joybstrafe;
extern int	joybuse;
extern int	joybspeed;

extern int	viewwidth;
extern int	viewheight;

extern int	mouseSensitivity;
extern int	showMessages;

extern int	detailLevel;

extern int	screenblocks;

extern int	showMessages;

// machine-independent sound params
extern	int	numChannels;


// UNIX hack, to be removed.
#ifdef SNDSERV
extern char*	sndserver_filename;
extern int	mb_used;
#endif

#ifdef LINUX
char*		mousetype;
char*		mousedev;
#endif

#ifdef __OS2__
// Defined in I_VIDEO.C; saved and restored through the defaults table below.
extern int	os2_window_x;
extern int	os2_window_y;
extern int	os2_window_w;
extern int	os2_window_h;

// Defined in I_VIDEO.C.  Declared here rather than by including os2doom.h,
// which would drag os2.h into an engine source file for one function.
int	I_OS2_KeyForScancode (int scan);

//
// What wrote this configuration file.
//
// 0, or absent, means "not this port" -- a DEFAULT.CFG from the DOS game,
// whose key bindings are raw scan codes rather than DOOM's own key codes.
// 1 means the bindings are already in our alphabet and must be left alone.
//
// The first attempt at this sniffed for settings only the DOS setup program
// writes -- comport, snd_sbport, snd_musicdevice.  That was wrong, and
// wrong in a way worth remembering: M_SaveDefaults writes out the table
// below and nothing else, so the very first run of this port deleted all
// three of them while leaving the scan codes it had failed to convert.  From
// then on the file was a DOS configuration with no evidence left that it was
// one, and every later run read the bindings as nonsense and said nothing.
//
// A stamp we write ourselves cannot be destroyed by our own saving.
//
#define OS2_CFGVERSION	1

int	os2_cfgversion = 0;

// Which of the ten bindings the file actually supplied.  Only those are
// translated: a binding that came from the table below is already a DOOM key
// code, and passing, say, the default use key -- a space, 0x20 -- through
// the scan code table would silently turn it into the letter D.
static boolean	os2_keyseen[10];
#endif

extern char*	chat_macros[];



typedef struct
{
    char*	name;
    int*	location;
    int		defaultvalue;
    int		scantranslate;		// PC scan code hack
    int		untranslated;		// lousy hack
} default_t;

default_t	defaults[] =
{
    {"mouse_sensitivity",&mouseSensitivity, 5},
    {"sfx_volume",&snd_SfxVolume, 8},
    {"music_volume",&snd_MusicVolume, 8},
    {"show_messages",&showMessages, 1},
    

#ifdef NORMALUNIX
    {"key_right",&key_right, KEY_RIGHTARROW},
    {"key_left",&key_left, KEY_LEFTARROW},
    {"key_up",&key_up, KEY_UPARROW},
    {"key_down",&key_down, KEY_DOWNARROW},
    {"key_strafeleft",&key_strafeleft, ','},
    {"key_straferight",&key_straferight, '.'},

    {"key_fire",&key_fire, KEY_RCTRL},
    {"key_use",&key_use, ' '},
    {"key_strafe",&key_strafe, KEY_RALT},
    {"key_speed",&key_speed, KEY_RSHIFT},

// UNIX hack, to be removed. 
#ifdef SNDSERV
    {"sndserver", (int *) &sndserver_filename, (int) "sndserver"},
    {"mb_used", &mb_used, 2},
#endif
    
#endif

#ifdef LINUX
    {"mousedev", (int*)&mousedev, (int)"/dev/ttyS0"},
    {"mousetype", (int*)&mousetype, (int)"microsoft"},
#endif

    {"use_mouse",&usemouse, 1},
    {"mouseb_fire",&mousebfire,0},
    {"mouseb_strafe",&mousebstrafe,1},
    {"mouseb_forward",&mousebforward,2},

    {"use_joystick",&usejoystick, 0},
    {"joyb_fire",&joybfire,0},
    {"joyb_strafe",&joybstrafe,1},
    {"joyb_use",&joybuse,3},
    {"joyb_speed",&joybspeed,2},

    {"screenblocks",&screenblocks, 9},
    {"detaillevel",&detailLevel, 0},

    {"snd_channels",&numChannels, 3},



    {"usegamma",&usegamma, 0},

    {"chatmacro0", (int *) &chat_macros[0], (int) HUSTR_CHATMACRO0 },
    {"chatmacro1", (int *) &chat_macros[1], (int) HUSTR_CHATMACRO1 },
    {"chatmacro2", (int *) &chat_macros[2], (int) HUSTR_CHATMACRO2 },
    {"chatmacro3", (int *) &chat_macros[3], (int) HUSTR_CHATMACRO3 },
    {"chatmacro4", (int *) &chat_macros[4], (int) HUSTR_CHATMACRO4 },
    {"chatmacro5", (int *) &chat_macros[5], (int) HUSTR_CHATMACRO5 },
    {"chatmacro6", (int *) &chat_macros[6], (int) HUSTR_CHATMACRO6 },
    {"chatmacro7", (int *) &chat_macros[7], (int) HUSTR_CHATMACRO7 },
    {"chatmacro8", (int *) &chat_macros[8], (int) HUSTR_CHATMACRO8 },
    {"chatmacro9", (int *) &chat_macros[9], (int) HUSTR_CHATMACRO9 },

#ifdef __OS2__
    // Where the window was when the game was last shut down.  Kept up to
    // date from WM_SIZE and WM_MOVE in I_VIDEO.C, so what is saved is
    // whatever the window looked like at the end.  A width of zero means
    // nothing has been saved yet and the built-in size is used.
    {"os2_window_x", &os2_window_x, 0},
    {"os2_window_y", &os2_window_y, 0},
    {"os2_window_w", &os2_window_w, 0},
    {"os2_window_h", &os2_window_h, 0},

    // Written last so that it is the last line of the file: a configuration
    // that ends with this really was written by this port and completely.
    {"os2_cfgversion", &os2_cfgversion, 0}
#endif


};

int	numdefaults;
char*	defaultfile;


//
// M_SaveDefaults
//
void M_SaveDefaults (void)
{
    int		i;
    int		v;
    FILE*	f;
	
    f = fopen (defaultfile, "w");
    if (!f)
	return; // can't write the file, but don't complain
		
    for (i=0 ; i<numdefaults ; i++)
    {
	if (defaults[i].defaultvalue > -0xfff
	    && defaults[i].defaultvalue < 0xfff)
	{
	    v = *defaults[i].location;
	    fprintf (f,"%s\t\t%i\n",defaults[i].name,v);
	} else {
	    fprintf (f,"%s\t\t\"%s\"\n",defaults[i].name,
		     * (char **) (defaults[i].location));
	}
    }
	
    fclose (f);
}


//
// M_LoadDefaults
//
extern byte	scantokey[128];

#ifdef __OS2__
//
// M_OS2TranslateDosKeys
//
// A DEFAULT.CFG written by DOOM's DOS setup program stores its key bindings
// as raw PC scan codes -- turn right is 77, fire is 29, use is 57 -- because
// that is what the DOS keyboard handler dealt in.
//
// This build, like the Linux one it came from, uses DOOM's own key codes
// instead: turn right is KEY_RIGHTARROW, 0xae.  Loading a DOS file as it
// stands therefore binds turn-right to 77, which this port reads as the
// letter M, and the arrow keys do nothing at all.  The player is left with a
// game that only answers the mouse -- and no way to tell why.
//
// So the scan codes are put back through the same table the keyboard handler
// uses, which is exactly the mapping that was lost.  The bindings the player
// chose are kept; only their spelling changes.  M_SaveDefaults then writes
// them in the new alphabet, so this happens once.
//

// The ten bindings the DOS setup program writes, in the order os2_keyseen
// records them.
static char*	os2_keynames[10] =
{
    "key_right", "key_left", "key_up", "key_down",
    "key_strafeleft", "key_straferight",
    "key_fire", "key_use", "key_strafe", "key_speed"
};

//
// M_OS2KeyIndex
//
// Which of the ten, or -1.
//
static int M_OS2KeyIndex (char* name)
{
    int		k;

    for (k = 0; k < 10; k++)
	if (!strcmp (name, os2_keynames[k]))
	    return k;

    return -1;
}


//
// M_OS2LooksLikeDosConfig
//
// A second opinion, for the case the stamp cannot cover.
//
// A configuration written by an early build of this port carries no stamp and
// may already have been translated, and translating twice would be worse than
// not translating at all: DOOM's use key is 0x20, which is also the scan code
// for D, so a second pass would quietly rebind it.
//
// The four movement keys settle it.  DOOM's arrow codes are 0xac to 0xaf and
// the DOS scan codes for the same keys are 0x48 to 0x50, so which side of
// 0x80 they fall on says which alphabet the file is written in.  If the file
// supplied none of them there is nothing to go on, and the stamp stands
// alone.
//
static boolean M_OS2LooksLikeDosConfig (void)
{
    static char*	arrows[4] =
	{ "key_right", "key_left", "key_up", "key_down" };

    int		i;
    int		a;
    int		seen = 0;
    int		low  = 0;

    for (a = 0; a < 4; a++)
    {
	int	k = M_OS2KeyIndex (arrows[a]);

	if (k < 0 || !os2_keyseen[k])
	    continue;

	for (i = 0; i < numdefaults; i++)
	    if (!strcmp (defaults[i].name, arrows[a]))
	    {
		seen++;
		if (*defaults[i].location < 0x80)
		    low++;
		break;
	    }
    }

    // No evidence either way: trust the missing stamp.
    if (!seen)
	return true;

    // Every one of them below 0x80 is a scan code file.  Anything else --
    // including a mixture -- is left alone, because a player who has bound
    // movement to letters is better served by nothing happening than by
    // having those bindings rewritten underneath them.
    return (low == seen) ? true : false;
}


static void M_OS2TranslateDosKeys (void)
{
    int		i;
    int		k;
    int		changed = 0;

    if (!M_OS2LooksLikeDosConfig ())
    {
	printf ("M_LoadDefaults: %s carries no version stamp, but its\n"
		"                bindings are already DOOM key codes"
		" -- left alone.\n", defaultfile);
	return;
    }

    for (k = 0; k < 10; k++)
    {
	// Not in the file, so it is whatever the table above says -- already
	// a DOOM key code, and not to be touched.
	if (!os2_keyseen[k])
	    continue;

	for (i = 0; i < numdefaults; i++)
	    if (!strcmp (defaults[i].name, os2_keynames[k]))
	    {
		int	key = I_OS2_KeyForScancode (*defaults[i].location);

		if (key)
		{
		    *defaults[i].location = key;
		    changed++;
		}
		break;
	    }
    }

    printf ("M_LoadDefaults: %s was not written by this port;\n"
	    "                %i key bindings translated from DOS scan codes.\n",
	    defaultfile, changed);
}


//
// M_OS2ShowKeys
//
// The bindings actually in force, in hexadecimal, because that is how DOOM's
// key codes are written down.  Cheap, and it turns "the keyboard does not
// work" into a question with an answer.
//
static void M_OS2ShowKeys (void)
{
    int		i;
    int		k;

    printf ("M_LoadDefaults: bindings");

    for (k = 0; k < 10; k++)
	for (i = 0; i < numdefaults; i++)
	    if (!strcmp (defaults[i].name, os2_keynames[k]))
	    {
		printf (" %s=%02x", os2_keynames[k] + 4,
			(unsigned)(*defaults[i].location & 0xff));
		break;
	    }

    printf ("\n");
}
#endif


void M_LoadDefaults (void)
{
    int		i;
    int		len;
    FILE*	f;
    char	def[80];
    char	strparm[100];
    char*	newstring;
    int		parm;
    boolean	isstring;
#ifdef __OS2__
    boolean	hadfile = false;
#endif
    
    // set everything to base values
    numdefaults = sizeof(defaults)/sizeof(defaults[0]);
    for (i=0 ; i<numdefaults ; i++)
	*defaults[i].location = defaults[i].defaultvalue;
    
    // check for a custom default file
    i = M_CheckParm ("-config");
    if (i && i<myargc-1)
    {
	defaultfile = myargv[i+1];
	printf ("	default file: %s\n",defaultfile);
    }
    else
	defaultfile = basedefault;
    
    // read the file in, overriding any set defaults
    f = fopen (defaultfile, "r");
    if (f)
    {
#ifdef __OS2__
	hadfile = true;
#endif
	while (!feof(f))
	{
	    isstring = false;
	    if (fscanf (f, "%79s %[^\n]\n", def, strparm) == 2)
	    {
		if (strparm[0] == '"')
		{
		    // get a string default
		    isstring = true;
		    len = strlen(strparm);
		    newstring = (char *) malloc(len);
		    strparm[len-1] = 0;
		    strcpy(newstring, strparm+1);
		}
		else if (strparm[0] == '0' && strparm[1] == 'x')
		    sscanf(strparm+2, "%x", &parm);
		else
		    sscanf(strparm, "%i", &parm);
#ifdef __OS2__
		// Remember which of the ten bindings the file supplied, so
		// that only those are put through the scan code table.
		{
		    int	k = M_OS2KeyIndex (def);

		    if (k >= 0)
			os2_keyseen[k] = true;
		}
#endif
		for (i=0 ; i<numdefaults ; i++)
		    if (!strcmp(def, defaults[i].name))
		    {
			if (!isstring)
			    *defaults[i].location = parm;
			else
			    *defaults[i].location =
				(int) newstring;
			break;
		    }
	    }
	}
		
	fclose (f);
    }

#ifdef __OS2__
    //
    // A file that does not carry our stamp was not written by this port, so
    // whatever bindings it supplied are DOS scan codes and have to be
    // converted.  A file that does carry it is left exactly as it is.
    //
    // Note that this is asked of the file, not of the values: 0x20 is both
    // DOOM's use key and the scan code for D, so the numbers themselves
    // cannot say which alphabet they are written in.  Only the stamp can.
    //
    if (hadfile && os2_cfgversion < OS2_CFGVERSION)
	M_OS2TranslateDosKeys ();

    // From here on the file is ours, whether it was before or not.
    os2_cfgversion = OS2_CFGVERSION;

    M_OS2ShowKeys ();
#endif
}


//
// SCREEN SHOTS
//


typedef struct
{
    char		manufacturer;
    char		version;
    char		encoding;
    char		bits_per_pixel;

    unsigned short	xmin;
    unsigned short	ymin;
    unsigned short	xmax;
    unsigned short	ymax;
    
    unsigned short	hres;
    unsigned short	vres;

    unsigned char	palette[48];
    
    char		reserved;
    char		color_planes;
    unsigned short	bytes_per_line;
    unsigned short	palette_type;
    
    char		filler[58];
    unsigned char	data;		// unbounded
} pcx_t;


//
// WritePCXfile
//
void
WritePCXfile
( char*		filename,
  byte*		data,
  int		width,
  int		height,
  byte*		palette )
{
    int		i;
    int		length;
    pcx_t*	pcx;
    byte*	pack;
	
    pcx = Z_Malloc (width*height*2+1000, PU_STATIC, NULL);

    pcx->manufacturer = 0x0a;		// PCX id
    pcx->version = 5;			// 256 color
    pcx->encoding = 1;			// uncompressed
    pcx->bits_per_pixel = 8;		// 256 color
    pcx->xmin = 0;
    pcx->ymin = 0;
    pcx->xmax = SHORT(width-1);
    pcx->ymax = SHORT(height-1);
    pcx->hres = SHORT(width);
    pcx->vres = SHORT(height);
    memset (pcx->palette,0,sizeof(pcx->palette));
    pcx->color_planes = 1;		// chunky image
    pcx->bytes_per_line = SHORT(width);
    pcx->palette_type = SHORT(2);	// not a grey scale
    memset (pcx->filler,0,sizeof(pcx->filler));


    // pack the image
    pack = &pcx->data;
	
    for (i=0 ; i<width*height ; i++)
    {
	if ( (*data & 0xc0) != 0xc0)
	    *pack++ = *data++;
	else
	{
	    *pack++ = 0xc1;
	    *pack++ = *data++;
	}
    }
    
    // write the palette
    *pack++ = 0x0c;	// palette ID byte
    for (i=0 ; i<768 ; i++)
	*pack++ = *palette++;
    
    // write output file
    length = pack - (byte *)pcx;
    M_WriteFile (filename, pcx, length);

    Z_Free (pcx);
}


//
// M_ScreenShot
//
void M_ScreenShot (void)
{
    int		i;
    byte*	linear;
    char	lbmname[12];
    
    // munge planar buffer to linear
    linear = screens[2];
    I_ReadScreen (linear);
    
    // find a file name to save it to
    strcpy(lbmname,"DOOM00.pcx");
		
    for (i=0 ; i<=99 ; i++)
    {
	lbmname[4] = i/10 + '0';
	lbmname[5] = i%10 + '0';
	if (access(lbmname,0) == -1)
	    break;	// file doesn't exist
    }
    if (i==100)
	I_Error ("M_ScreenShot: Couldn't create a PCX");
    
    // save the pcx file
    WritePCXfile (lbmname, linear,
		  SCREENWIDTH, SCREENHEIGHT,
		  W_CacheLumpName ("PLAYPAL",PU_CACHE));
	
    players[consoleplayer].message = "screen shot";
}


