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
//	The startup transcript.  OS/2 version; there is no equivalent
//	upstream.
//
//	DOOM says a great deal on its way up -- which WAD it found, how much
//	memory it took, what the sound card agreed to play -- and every word
//	of it is useful exactly once, when something has gone wrong.  On DOS
//	it scrolled past in the session the game was started from and could be
//	read afterwards.  Here the game ends up owning the screen, and a
//	player who starts it from the Workplace Shell has nowhere to read it
//	at all.
//
//	So everything the game prints is written to DOOM.LOG as well as to
//	wherever it was going anyway.  The file is rewritten each run: what is
//	wanted is a transcript of the run that just failed, not an archive.
//
//	This file supplies printf and puts for the whole program, by way of
//	the macros in doomdef.h, which is why doomdef.h is the one DOOM header
//	it does not include -- the definitions below must be the real ones.
//
//	It writes through the file system rather than through the C library.
//	A log exists to describe a run that ended badly, and the worst endings
//	take the machine with them: twice now a trap has locked OS/2 hard
//	enough that the last of the log was still sitting in the disk cache,
//	and CHKDSK handed back an empty file afterwards.  fflush only reaches
//	that cache.  DosWrite on a handle opened WRITE_THROUGH, followed by
//	DosResetBuffer so the directory entry follows the data out, reaches the
//	platter -- so every line is on disk before the line that might take the
//	machine down is even reached.
//
//	That is slow, and deliberately so.  It costs a disk write per line of
//	startup text, which is a few dozen writes across a second or two, and
//	it buys the only thing the file is for.
//
//-----------------------------------------------------------------------------

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

#define INCL_DOSFILEMGR
#include "os2doom.h"

#include "doomtype.h"
#include "m_argv.h"


static HFILE	loghandle	= (HFILE)0;
static boolean	logopen		= false;
static boolean	logtried	= false;


//
// I_OS2_LogInit
//
// Called before anything else has a chance to print.  A log that cannot be
// opened is not an error: the game is perfectly playable without one, and
// refusing to start because a directory is read-only would be absurd.
//
void I_OS2_LogInit (void)
{
    ULONG	action = 0;
    time_t	now;
    char	line[128];

    if (logtried)
	return;

    logtried = true;

    if (M_CheckParm ("-nolog"))
	return;

    //
    // WRITE_THROUGH is the entire reason for opening it this way rather than
    // with fopen.  DENYNONE is so the file can be read from another program
    // while the game is still running, which is how a hang gets looked at at
    // all.
    //
    if (DosOpen ((PSZ)"DOOM.LOG", &loghandle, &action, 0, FILE_NORMAL,
		 OPEN_ACTION_CREATE_IF_NEW | OPEN_ACTION_REPLACE_IF_EXISTS,
		 OPEN_FLAGS_WRITE_THROUGH | OPEN_FLAGS_FAIL_ON_ERROR
		 | OPEN_FLAGS_NOINHERIT | OPEN_SHARE_DENYNONE
		 | OPEN_ACCESS_WRITEONLY,
		 NULL) != NO_ERROR)
	return;

    logopen = true;

    now = time (NULL);
    sprintf (line, "DOOM for OS/2 -- startup transcript, %s", ctime (&now));
    I_OS2_LogWrite (line);

    //
    // When this executable was built, which matters more than it looks.
    //
    // The game is built on one machine and run on another, and a log is read
    // back on the first.  Nothing in the transcript otherwise says which
    // build produced it, so a stale copy on the test machine -- or a log that
    // did not make the return trip -- reads as a genuine result, and a test
    // that never ran gets acted on.  That has happened twice.
    //
    sprintf (line, "Built %s %s\n", __DATE__, __TIME__);
    I_OS2_LogWrite (line);
    I_OS2_LogWrite ("----------------------------------------"
		    "---------------------------------------\n");
}


//
// I_OS2_LogShutdown
//
void I_OS2_LogShutdown (void)
{
    if (logopen)
    {
	logopen = false;
	DosClose (loghandle);
    }
}


//
// I_OS2_LogWrite
//
// One piece of already-formatted text, to the log alone.
//
// Written straight through to the disk, and the file's buffers reset
// afterwards so the directory entry follows the data out.  By the time the
// next line runs, this one is on the platter -- which is the whole value of
// the file, because what happens next may well be a trap.
//
void I_OS2_LogWrite (const char* text)
{
    ULONG	written;
    ULONG	len;

    if (!logopen)
	return;

    len = (ULONG)strlen (text);
    if (!len)
	return;

    if (DosWrite (loghandle, (PVOID)text, len, &written) != NO_ERROR)
    {
	// A log that has begun failing -- a full disk, most likely -- is not
	// worth retrying on every line for the rest of the run.
	logopen = false;
	DosClose (loghandle);
	return;
    }

    DosResetBuffer (loghandle);
}


//
// I_OS2_Printf
//
// What the rest of the program calls when it writes printf.
//
// The fixed buffer is a deliberate limit rather than an oversight.  This runs
// before the heap is worth trusting and during failures, so it allocates
// nothing; 1K is comfortably more than any single line DOOM prints, and a
// line longer than that is truncated rather than allowed to overrun.
//
int I_OS2_Printf (const char* fmt, ...)
{
    char	buf[1024];
    va_list	ap;
    int		n;

    va_start (ap, fmt);
    n = vsprintf (buf, fmt, ap);
    va_end (ap);

    fputs (buf, stdout);
    fflush (stdout);

    I_OS2_LogWrite (buf);

    // The loading window watches the same stream and moves its bar when a
    // line names a stage of the startup.
    I_OS2_LoadNotice (buf);

    return n;
}


//
// I_OS2_Puts
//
int I_OS2_Puts (const char* s)
{
    fputs (s, stdout);
    fputs ("\n", stdout);
    fflush (stdout);

    I_OS2_LogWrite (s);
    I_OS2_LogWrite ("\n");

    I_OS2_LoadNotice (s);

    return 0;
}
