// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
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
//-----------------------------------------------------------------------------

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

#include "os2doom.h"

#include "doomtype.h"
#include "m_argv.h"


static FILE*	logfile		= NULL;
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
    time_t	now;

    if (logtried)
	return;

    logtried = true;

    if (M_CheckParm ("-nolog"))
	return;

    logfile = fopen ("DOOM.LOG", "w");
    if (!logfile)
	return;

    now = time (NULL);
    fprintf (logfile, "DOOM for OS/2 -- startup transcript, %s", ctime (&now));
    fprintf (logfile, "----------------------------------------"
		      "---------------------------------------\n");
    fflush (logfile);
}


//
// I_OS2_LogShutdown
//
void I_OS2_LogShutdown (void)
{
    if (logfile)
    {
	fclose (logfile);
	logfile = NULL;
    }
}


//
// I_OS2_LogWrite
//
// One piece of already-formatted text, to the log alone.  Flushed every time:
// the whole value of the file is that it survives whatever happens next, and
// what happens next may well be a trap.
//
void I_OS2_LogWrite (const char* text)
{
    if (logfile)
    {
	fputs (text, logfile);
	fflush (logfile);
    }
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

    return 0;
}
