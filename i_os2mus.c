// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// Copyright (C) 2026 by Charlie Dobson.
//
// Written for the OS/2 port of DOOM.  Nothing in this file comes from id
// Software: it exists only because the port needed it.
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
// DESCRIPTION:
//	Music for the OS/2 port.
//
//	The released source has no music at all.  The Linux build left every
//	one of these entry points empty, and the DOS build's music code was
//	never released -- it was the part that used the licensed sound library
//	id could not ship.  So this is new.
//
//	Two halves:
//
//	  1. DOOM stores its music as MUS, a compact relative of MIDI written
//	     for the DOS build.  Nothing on OS/2 has heard of it, so it is
//	     converted here to an ordinary type 0 MIDI file.
//
//	  2. MMPM/2's MCI sequencer plays that file.  It is handed over as a
//	     file rather than a stream because that is what the sequencer
//	     device wants, so the converted MIDI goes to a temporary file
//	     which is cleaned up when the song changes or the game exits.
//
//	Looping is done by asking MCI to notify the game window when the song
//	finishes, and starting it again -- see I_OS2_MusicNotify, which
//	I_VIDEO.C calls when MM_MCINOTIFY arrives.
//
//	None of this is essential: every failure here is soft.  A machine with
//	no MIDI device configured, or no MMPM/2 at all, plays no music and
//	says so once.
//
//-----------------------------------------------------------------------------

static const char
rcsid[] = "$Id:$";

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>

#include "os2doom.h"

// See the note in I_SOUND.C: os2medef.h has "typedef WORD VERSION" and
// doomdef.h has "enum { VERSION = 110 }".  Neither is a macro, so the MMPM/2
// type is renamed on the way in.
#define INCL_OS2MM
#define INCL_MMIOOS2
#define VERSION MMPM_VERSION
#include <os2me.h>
#undef VERSION

#include "doomdef.h"
#include "doomstat.h"
#include "i_system.h"
#include "i_sound.h"
#include "m_argv.h"


//=============================================================================
//
//  MUS to MIDI
//
//=============================================================================

//
// The MUS header.  All of it is little endian, which is what the machine is,
// and every field is naturally aligned, so it can be read as a struct.
//
typedef struct
{
    char		ID[4];		// "MUS" 0x1a
    unsigned short	scoreLen;	// bytes of score
    unsigned short	scoreStart;	// offset of the score from the start
    unsigned short	channels;	// primary channels used
    unsigned short	sec_channels;	// secondary channels used
    unsigned short	instrCnt;	// how many instrument patches follow
    unsigned short	dummy;
} musheader_t;


// MUS event types, from the descriptor byte's bits 6-4.
#define MUS_RELEASEo		0
#define MUS_PLAY		1
#define MUS_PITCHBEND		2
#define MUS_SYSTEM		3
#define MUS_CONTROLLER		4
#define MUS_SCOREEND		6

//
// MUS controller number to MIDI controller number.
//
// Entry 0 is a program change rather than a controller and is handled apart;
// it is only in the table to keep the numbering honest.
//
static const byte mus_ctrl_to_midi[10] =
{
      0,	// 0: instrument -- program change, special cased
      0,	// 1: bank select
      1,	// 2: modulation
      7,	// 3: volume
     10,	// 4: pan
     11,	// 5: expression
     91,	// 6: reverb depth
     93,	// 7: chorus depth
     64,	// 8: sustain pedal
     67		// 9: soft pedal
};

//
// MUS system event number (10-14) to MIDI channel mode controller.
//
static const byte mus_sys_to_midi[5] =
{
    120,	// 10: all sounds off
    123,	// 11: all notes off
    126,	// 12: mono
    127,	// 13: poly
    121		// 14: reset all controllers
};


// The MIDI file being built.
static byte*	midibuf		= NULL;
static int	midilen		= 0;
static int	midimax		= 0;
static boolean	midifull	= false;	// ran out of memory

static void MidiByte (byte b)
{
    if (midifull)
	return;

    if (midilen >= midimax)
    {
	int	newmax = midimax ? midimax*2 : 8192;
	byte*	grown  = (byte *) realloc (midibuf, newmax);

	if (!grown)
	{
	    midifull = true;
	    return;
	}

	midibuf = grown;
	midimax = newmax;
    }

    midibuf[midilen++] = b;
}

static void MidiBytes (byte *p, int n)
{
    while (n--)
	MidiByte (*p++);
}

//
// A MIDI variable length quantity: seven bits per byte, high bit set on
// every byte but the last.
//
static void MidiVLQ (unsigned long v)
{
    unsigned long	buffer = v & 0x7f;

    while ((v >>= 7))
    {
	buffer <<= 8;
	buffer |= ((v & 0x7f) | 0x80);
    }

    for (;;)
    {
	MidiByte ((byte)(buffer & 0xff));
	if (buffer & 0x80)
	    buffer >>= 8;
	else
	    break;
    }
}


//
// I_Mus2Midi
//
// Converts a MUS lump into a type 0 MIDI file in midibuf.  Returns false if
// the lump is not MUS, is malformed, or memory ran out.
//
// Timing: MUS events are counted in 140ths of a second.  A MIDI file's tick
// is (tempo / division) microseconds, so a division of 70 ticks per crotchet
// with the standard tempo of 500000 microseconds per crotchet gives exactly
// 140 ticks a second.  No rounding anywhere, which is why those two numbers
// and not some other pair.
//
static boolean I_Mus2Midi (byte *mus, int muslen)
{
    musheader_t*	head = (musheader_t *)mus;
    byte*		score;
    byte*		end;
    unsigned long	delta = 0;
    byte		lastvol[16];
    int			i;
    int			tracklenpos;
    int			trackstart;
    boolean		done = false;

    static const byte	mthd[] =
    {
	'M','T','h','d',  0,0,0,6,	// header, six bytes of it
	0,0,				// format 0: everything in one track
	0,1,				// one track
	0,70				// 70 ticks per crotchet
    };

    static const byte	tempo[] =
    {
	0, 0xff, 0x51, 0x03, 0x07, 0xa1, 0x20	// 500000 us per crotchet
    };

    if (muslen < (int)sizeof(musheader_t))
	return false;

    if (memcmp (head->ID, "MUS\032", 4))
	return false;

    if (head->scoreStart + head->scoreLen > muslen)
	return false;

    score = mus + head->scoreStart;
    end   = score + head->scoreLen;

    for (i = 0; i < 16; i++)
	lastvol[i] = 100;

    midilen  = 0;
    midifull = false;

    MidiBytes ((byte *)mthd, sizeof(mthd));

    MidiBytes ((byte *)"MTrk", 4);
    tracklenpos = midilen;
    MidiByte (0); MidiByte (0); MidiByte (0); MidiByte (0);
    trackstart = midilen;

    MidiBytes ((byte *)tempo, sizeof(tempo));

    while (score < end && !done && !midifull)
    {
	byte	desc = *score++;
	int	type = (desc >> 4) & 7;
	int	ch   = desc & 15;
	int	last = desc & 0x80;
	int	midich;

	// MUS puts percussion on channel 15; MIDI has it on channel 9.
	// Swapping the two is the whole of the channel mapping.
	if (ch == 15)		midich = 9;
	else if (ch == 9)	midich = 15;
	else			midich = ch;

	switch (type)
	{
	  case MUS_RELEASEo:
	  {
	    byte	note;
	    if (score >= end) return false;
	    note = *score++ & 0x7f;
	    MidiVLQ (delta);
	    MidiByte (0x80 | midich);
	    MidiByte (note);
	    MidiByte (0);
	    break;
	  }

	  case MUS_PLAY:
	  {
	    byte	note;
	    if (score >= end) return false;
	    note = *score++;

	    // The top bit says a volume byte follows.  Without one, the
	    // channel keeps whatever volume it last had -- which is how MUS
	    // stays so much smaller than MIDI.
	    if (note & 0x80)
	    {
		if (score >= end) return false;
		lastvol[ch] = *score++ & 0x7f;
	    }
	    note &= 0x7f;

	    MidiVLQ (delta);
	    MidiByte (0x90 | midich);
	    MidiByte (note);
	    MidiByte (lastvol[ch]);
	    break;
	  }

	  case MUS_PITCHBEND:
	  {
	    unsigned	bend;
	    if (score >= end) return false;

	    // One byte in MUS, fourteen bits in MIDI: 0 to 255 becomes 0 to
	    // 16320, with 128 landing on 8192, the centre.
	    bend = (unsigned)(*score++) << 6;

	    MidiVLQ (delta);
	    MidiByte (0xe0 | midich);
	    MidiByte ((byte)(bend & 0x7f));
	    MidiByte ((byte)((bend >> 7) & 0x7f));
	    break;
	  }

	  case MUS_SYSTEM:
	  {
	    byte	ev;
	    if (score >= end) return false;
	    ev = *score++ & 0x7f;
	    if (ev < 10 || ev > 14) return false;

	    MidiVLQ (delta);
	    MidiByte (0xb0 | midich);
	    MidiByte (mus_sys_to_midi[ev - 10]);
	    MidiByte (0);
	    break;
	  }

	  case MUS_CONTROLLER:
	  {
	    byte	ctrl, val;
	    if (score + 1 >= end) return false;
	    ctrl = *score++ & 0x7f;
	    val  = *score++ & 0x7f;

	    if (ctrl == 0)
	    {
		// Change instrument.
		MidiVLQ (delta);
		MidiByte (0xc0 | midich);
		MidiByte (val);
	    }
	    else if (ctrl < 10)
	    {
		MidiVLQ (delta);
		MidiByte (0xb0 | midich);
		MidiByte (mus_ctrl_to_midi[ctrl]);
		MidiByte (val);
	    }
	    else
		return false;
	    break;
	  }

	  case MUS_SCOREEND:
	    done = true;
	    break;

	  default:
	    // Type 5 and type 7 are not defined.  Rather than guess at how
	    // many bytes to skip and desynchronise everything after, give up
	    // and let the caller play nothing.
	    return false;
	}

	delta = 0;

	// The top bit of the descriptor says a delay follows, and it is the
	// gap before the NEXT event.
	if (last && !done)
	{
	    delta = 0;
	    for (;;)
	    {
		byte	b;
		if (score >= end) return false;
		b = *score++;
		delta = (delta << 7) | (b & 0x7f);
		if (!(b & 0x80))
		    break;
	    }
	}
    }

    if (!done || midifull)
	return false;

    // End of track.
    MidiVLQ (0);
    MidiByte (0xff);
    MidiByte (0x2f);
    MidiByte (0x00);

    if (midifull)
	return false;

    // Patch the track length in, big endian as MIDI files are.
    {
	int	tracklen = midilen - trackstart;

	midibuf[tracklenpos+0] = (byte)((tracklen >> 24) & 0xff);
	midibuf[tracklenpos+1] = (byte)((tracklen >> 16) & 0xff);
	midibuf[tracklenpos+2] = (byte)((tracklen >>  8) & 0xff);
	midibuf[tracklenpos+3] = (byte)( tracklen        & 0xff);
    }

    return true;
}


//
// MidiLumpLength
//
// S_ChangeMusic hands over a pointer to the lump and no length at all, so the
// length has to come from the data.  A MIDI file gives it up by walking its
// chunks: each one carries its own size.
//
static int MidiLumpLength (byte *data)
{
    int		len = 0;
    byte*	p = data;

    if (memcmp (p, "MThd", 4))
	return 0;

    for (;;)
    {
	unsigned long	chunklen;

	if (memcmp (p, "MThd", 4) && memcmp (p, "MTrk", 4))
	    break;

	chunklen = ((unsigned long)p[4] << 24) | ((unsigned long)p[5] << 16)
		 | ((unsigned long)p[6] <<  8) |  (unsigned long)p[7];

	// A chunk longer than any plausible song means this is not really a
	// MIDI file and the walk has wandered into rubbish.
	if (chunklen > 0x400000)
	    return 0;

	p   += 8 + chunklen;
	len += 8 + (int)chunklen;

	if (len > 0x400000)
	    return 0;
    }

    return len;
}


//=============================================================================
//
//  MMPM/2 playback
//
//=============================================================================

// The MMPM/2 entry point, shared with the sound (I_SOUND.C) through
// I_OS2_MciEntry so that MDM.DLL is only ever loaded once.
static PFNMCISENDCOMMAND pMci	= NULL;

static USHORT	musDeviceID	= 0;
static boolean	musOpen		= false;
static boolean	musPlaying	= false;
static boolean	musLooping	= false;
static boolean	musAvailable	= true;		// until proved otherwise
static boolean	musWarned	= false;

static char	musFile[CCHMAXPATH];
static boolean	musFileMade	= false;


//
// MusMciError
//
// Says it once.  A machine with no MIDI device set up would otherwise print
// a line every time the level changed.
//
static void MusMciWarn (char *what)
{
    if (musWarned)
	return;

    musWarned = true;
    printf ("I_Music: %s; the game will play without music.\n", what);
}


static void MusClose (void)
{
    MCI_GENERIC_PARMS	gp;

    if (musOpen)
    {
	memset (&gp, 0, sizeof(gp));
	pMci (musDeviceID, MCI_CLOSE, MCI_WAIT, (PVOID)&gp, 0);
	musOpen = false;
	musDeviceID = 0;
    }

    musPlaying = false;

    if (musFileMade)
    {
	remove (musFile);
	musFileMade = false;
    }
}


//
// I_InitMusic
//
void I_InitMusic (void)
{
    char*	tmp;

    if (M_CheckParm ("-nomusic") || M_CheckParm ("-nosound"))
    {
	musAvailable = false;
	printf ("I_InitMusic: disabled.\n");
	return;
    }

    // The one door into MMPM/2.  No multimedia support installed means no
    // music, which is not an error worth stopping over.
    pMci = I_OS2_MciEntry ();
    if (!pMci)
    {
	musAvailable = false;
	printf ("I_InitMusic: no MMPM/2 on this machine; no music.\n");
	return;
    }

    //
    // Somewhere to put the converted MIDI.
    //
    // TMP is only a suggestion.  It is quite normally set to a RAM disk that
    // is not mounted, or to a directory that was deleted years ago, and the
    // first sign of that used to be music silently failing at the first
    // level -- by which point the reason had scrolled away.
    //
    // So the candidates are tried here, at startup, by actually creating a
    // file in each and keeping the first that works.  The game's own
    // directory is last and is nearly always the answer: the configuration
    // and this log were both written there.
    //
    // The process id is in the name so that two copies of DOOM on one
    // machine do not overwrite each other's.
    {
	char*	candidates[3];
	int	c;
	int	handle;

	candidates[0] = getenv ("TMP");
	candidates[1] = getenv ("TEMP");
	candidates[2] = ".";

	musFile[0] = 0;

	for (c = 0; c < 3; c++)
	{
	    tmp = candidates[c];

	    if (!tmp || !*tmp || strlen(tmp) + 16 >= CCHMAXPATH)
		continue;

	    sprintf (musFile, "%s\\DM%05u.MID", tmp, (unsigned)getpid());

	    handle = open (musFile, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY,
			   0666);

	    if (handle >= 0)
	    {
		close (handle);
		remove (musFile);
		break;
	    }

	    printf ("I_InitMusic: cannot write in %s; trying elsewhere.\n",
		    tmp);
	    musFile[0] = 0;
	}

	if (!musFile[0])
	{
	    musAvailable = false;
	    printf ("I_InitMusic: nowhere to write the converted MIDI;"
		    " no music.\n");
	    return;
	}
    }

    printf ("I_InitMusic: MMPM/2 sequencer, MUS converted to MIDI in %s\n",
	    musFile);
}


//
// I_ShutdownMusic
//
void I_ShutdownMusic (void)
{
    MusClose ();

    if (midibuf)
    {
	free (midibuf);
	midibuf = NULL;
	midimax = midilen = 0;
    }
}


//
// I_RegisterSong
//
// Convert, write out, and open.  Returns a handle, which for this port is
// always 1: DOOM only ever has one song registered at a time.
//
int I_RegisterSong (void *data)
{
    MCI_OPEN_PARMS	op;
    byte*		lump = (byte *)data;
    byte*		out;
    int			outlen;
    int			handle;

    if (!musAvailable || !data)
	return 0;

    // Whatever was playing before is finished with.
    MusClose ();

    if (!memcmp (lump, "MUS\032", 4))
    {
	musheader_t*	head = (musheader_t *)lump;

	if (!I_Mus2Midi (lump, head->scoreStart + head->scoreLen))
	{
	    MusMciWarn ("this song could not be converted from MUS");
	    return 0;
	}

	out    = midibuf;
	outlen = midilen;
    }
    else if (!memcmp (lump, "MThd", 4))
    {
	// Some PWADs hold plain MIDI rather than MUS.  Pass it straight
	// through -- the sequencer wants MIDI anyway.
	outlen = MidiLumpLength (lump);
	if (!outlen)
	{
	    MusMciWarn ("this song is not a MIDI file after all");
	    return 0;
	}
	out = lump;
    }
    else
    {
	MusMciWarn ("this song is in neither MUS nor MIDI format");
	return 0;
    }

    // Out to the temporary file.  O_BINARY, of course.
    handle = open (musFile, O_WRONLY | O_CREAT | O_TRUNC | O_BINARY, 0666);
    if (handle == -1)
    {
	MusMciWarn ("the temporary music file could not be written");
	return 0;
    }

    if (write (handle, out, outlen) != outlen)
    {
	close (handle);
	remove (musFile);
	MusMciWarn ("the temporary music file could not be written");
	return 0;
    }

    close (handle);
    musFileMade = true;

    //
    // Hand it to the sequencer.
    //
    memset (&op, 0, sizeof(op));
    op.pszDeviceType  = (PSZ) MAKEULONG (MCI_DEVTYPE_SEQUENCER, 0);
    op.pszElementName = (PSZ) musFile;

    {
	ULONG	rc;

	//
	// Shareable first, so that the game does not take the MIDI device
	// away from the rest of the desktop for as long as it runs.
	//
	// Not every sequencer driver permits that, though, and one that does
	// not refuses the open outright rather than falling back -- so the
	// same request goes in again exclusively before giving up.  An FM
	// synthesiser on a card of this generation is a single voice bank
	// with no notion of two programs using it at once, and is the likely
	// reason for a driver to insist.
	//
	rc = pMci (0, MCI_OPEN,
		   MCI_WAIT | MCI_OPEN_TYPE_ID | MCI_OPEN_ELEMENT
		   | MCI_OPEN_SHAREABLE,
		   (PVOID)&op, 0);

	if (rc != MCIERR_SUCCESS)
	{
	    memset (&op, 0, sizeof(op));
	    op.pszDeviceType  = (PSZ) MAKEULONG (MCI_DEVTYPE_SEQUENCER, 0);
	    op.pszElementName = (PSZ) musFile;

	    rc = pMci (0, MCI_OPEN,
		       MCI_WAIT | MCI_OPEN_TYPE_ID | MCI_OPEN_ELEMENT,
		       (PVOID)&op, 0);
	}

	if (rc != MCIERR_SUCCESS)
	{
	    char	msg[128];

	    remove (musFile);
	    musFileMade = false;

	    // With the number, because "no sequencer" and "the sequencer is
	    // there and would not take this file" are different problems and
	    // the message alone cannot tell them apart.
	    // The low word is the error; the high word is the device it
	    // happened to.  See I_MciErrName in I_SOUND.C -- printing the
	    // packed value whole is what made 70541 out of error 5005.
	    sprintf (msg, "the MMPM/2 sequencer would not open,"
			  " MCI error %u", (unsigned)(rc & 0xFFFF));
	    MusMciWarn (msg);

	    // Do not keep trying on every level change.
	    musAvailable = false;
	    return 0;
	}
    }

    musDeviceID = op.usDeviceID;
    musOpen = true;

    return 1;
}


//
// I_UnRegisterSong
//
void I_UnRegisterSong (int handle)
{
    handle = 0;		// UNUSED: there is only ever the one song
    MusClose ();
}


//
// I_PlaySong
//
void I_PlaySong (int handle, int looping)
{
    MCI_PLAY_PARMS	pp;

    handle = 0;		// UNUSED

    if (!musOpen)
	return;

    musLooping = looping ? true : false;

    memset (&pp, 0, sizeof(pp));

    // MCI_NOTIFY asks for a message when the song ends, which is how looping
    // is done -- see I_OS2_MusicNotify.  It goes to the game's own window,
    // whose message queue is pumped every frame.
    pp.hwndCallback = os2_hwndClient;

    if (pMci (musDeviceID, MCI_PLAY,
			musLooping && os2_hwndClient != NULLHANDLE
			    ? MCI_NOTIFY : 0,
			(PVOID)&pp, 0) != MCIERR_SUCCESS)
    {
	MusMciWarn ("the sequencer would not start the song");
	return;
    }

    musPlaying = true;
}


//
// I_OS2_MusicNotify
//
// Called from the window procedure when MM_MCINOTIFY arrives.  If the song
// that just finished was meant to loop, wind it back and start it again.
//
void I_OS2_MusicNotify (ULONG status)
{
    MCI_SEEK_PARMS	sp;
    MCI_PLAY_PARMS	pp;

    if (!musOpen || !musLooping)
	return;

    // Anything other than a clean finish -- an abort because the song was
    // stopped, say -- is not a reason to start it again.
    if (status != MCI_NOTIFY_SUCCESSFUL)
	return;

    memset (&sp, 0, sizeof(sp));
    if (pMci (musDeviceID, MCI_SEEK, MCI_WAIT | MCI_TO_START,
			(PVOID)&sp, 0) != MCIERR_SUCCESS)
	return;

    memset (&pp, 0, sizeof(pp));
    pp.hwndCallback = os2_hwndClient;

    pMci (musDeviceID, MCI_PLAY, MCI_NOTIFY, (PVOID)&pp, 0);
}


//
// I_PauseSong
//
void I_PauseSong (int handle)
{
    MCI_GENERIC_PARMS	gp;

    handle = 0;		// UNUSED

    if (!musOpen || !musPlaying)
	return;

    memset (&gp, 0, sizeof(gp));
    pMci (musDeviceID, MCI_PAUSE, MCI_WAIT, (PVOID)&gp, 0);
}


//
// I_ResumeSong
//
void I_ResumeSong (int handle)
{
    MCI_GENERIC_PARMS	gp;

    handle = 0;		// UNUSED

    if (!musOpen || !musPlaying)
	return;

    memset (&gp, 0, sizeof(gp));
    pMci (musDeviceID, MCI_RESUME, MCI_WAIT, (PVOID)&gp, 0);
}


//
// I_StopSong
//
void I_StopSong (int handle)
{
    MCI_GENERIC_PARMS	gp;

    handle = 0;		// UNUSED

    if (!musOpen)
	return;

    // Stop the loop first, or the notification from this very stop would
    // start the song again.
    musLooping = false;

    memset (&gp, 0, sizeof(gp));
    pMci (musDeviceID, MCI_STOP, MCI_WAIT, (PVOID)&gp, 0);

    musPlaying = false;
}


//
// I_SetMusicVolume
//
// DOOM's scale is 0 to 15; MCI wants a percentage.
//
void I_SetMusicVolume (int volume)
{
    MCI_SET_PARMS	sp;

    snd_MusicVolume = volume;

    if (!musOpen)
	return;

    memset (&sp, 0, sizeof(sp));
    sp.ulAudio = MCI_SET_AUDIO_ALL;
    sp.ulLevel = (volume * 100) / 15;

    pMci (musDeviceID, MCI_SET,
		    MCI_WAIT | MCI_SET_AUDIO | MCI_SET_VOLUME,
		    (PVOID)&sp, 0);
}


//
// I_QrySongPlaying
//
int I_QrySongPlaying (int handle)
{
    handle = 0;		// UNUSED
    return musPlaying;
}
