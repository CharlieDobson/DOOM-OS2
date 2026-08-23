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
// $Log:$
//
// DESCRIPTION:
//	System specific interface stuff -- OS/2 2.x / Warp version.
//
//	Replaces the Linux original: gettimeofday becomes the OS/2
//	high-resolution timer, usleep becomes DosSleep, and I_Error learns to
//	speak to the user through Presentation Manager once there is a window
//	to put a message box over.
//
//-----------------------------------------------------------------------------

static const char
rcsid[] = "$Id: i_system.c,v 1.4 1997/02/03 22:45:10 b1 Exp $";

#define INCL_DOSPROFILE			// DosTmrQueryFreq / DosTmrQueryTime
#define INCL_DOSMISC			// DosQuerySysInfo, QSV_MS_COUNT
#define INCL_DOSPROCESS			// DosSleep, DosGetInfoBlocks
#define INCL_WIN			// WinMessageBox

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "doomdef.h"
#include "m_misc.h"
#include "m_argv.h"
#include "i_video.h"
#include "i_sound.h"

#include "d_net.h"
#include "g_game.h"

#include "i_system.h"
#include "os2doom.h"


//
// Zone heap size, in megabytes.
//
// The Linux original fixed this at 6 and gave no way to change it, because
// the only thing that ever wrote to it was a sound-server setting in the
// configuration file.  OS/2 boxes vary a great deal more than the Linux
// workstations of 1997 did, so -mb is honoured here the way DOS DOOM honoured
// it.
//
int	mb_used = 6;


void
I_Tactile
( int	on,
  int	off,
  int	total )
{
  // UNUSED.
  on = off = total = 0;
}

ticcmd_t	emptycmd;
ticcmd_t*	I_BaseTiccmd(void)
{
    return &emptycmd;
}


int  I_GetHeapSize (void)
{
    return mb_used*1024*1024;
}

byte* I_ZoneBase (int*	size)
{
    byte*	base;
    int		p;

    // -mb <megabytes>, as DOS DOOM had.
    p = M_CheckParm ("-mb");
    if (p && p < myargc-1)
    {
	mb_used = atoi (myargv[p+1]);
	if (mb_used < 2)
	    mb_used = 2;
    }

    *size = mb_used*1024*1024;
    base = (byte *) malloc (*size);

    // The original just handed the null pointer to Z_Init and let the first
    // allocation fault.  On OS/2, where the swapper can genuinely refuse,
    // say what went wrong instead.
    if (!base)
	I_Error ("I_ZoneBase: failed to allocate %i MB for the zone heap.\n"
		 "Free some memory, or start DOOM with a smaller -mb value.",
		 mb_used);

    return base;
}



//
// I_GetTime
// returns time in 1/70th second tics
//
// Two clocks, in order of preference:
//
//   DosTmrQueryTime  - the 8254 latch, counting at about 1.19 MHz.  This is
//                      the one we want: a DOOM tic is 28.6 ms, and this
//                      resolves to under a microsecond.
//
//   QSV_MS_COUNT     - milliseconds since boot.  Nominally 1 ms, but it is
//                      only advanced by the timer interrupt, so in practice
//                      it moves in steps of about 32 ms -- roughly one whole
//                      tic.  Playable, but the frame pacing visibly wobbles.
//                      It is here only because DosTmrQueryFreq can fail.
//
// Both are unwrapped into a 64-bit count before any arithmetic.  Multiplying
// a 32-bit millisecond count by TICRATE overflows after about two minutes,
// which is the kind of bug that looks like the game "speeding up" much later.
//
static ULONG		tmrfreq = 0;	// 0 => fall back to the ms counter
static long long	tmrbase = 0;

static long long I_HiResCount (void)
{
    QWORD	q;

    if (DosTmrQueryTime (&q) != NO_ERROR)
	return 0;

    return (((long long)q.ulHi) << 32) | (unsigned long)q.ulLo;
}

int  I_GetTime (void)
{
    static int		inited = 0;
    static ULONG	msbase = 0;
    ULONG		ms;

    if (!inited)
    {
	inited = 1;

	if (DosTmrQueryFreq (&tmrfreq) != NO_ERROR || tmrfreq == 0)
	    tmrfreq = 0;
	else
	    tmrbase = I_HiResCount ();

	if (!tmrfreq)
	    DosQuerySysInfo (QSV_MS_COUNT, QSV_MS_COUNT,
			     &msbase, sizeof(msbase));

	return 0;
    }

    if (tmrfreq)
    {
	long long	delta = I_HiResCount() - tmrbase;

	return (int)((delta * TICRATE) / (long long)tmrfreq);
    }

    DosQuerySysInfo (QSV_MS_COUNT, QSV_MS_COUNT, &ms, sizeof(ms));

    // Unsigned subtraction, so the 49-day wrap of the millisecond counter
    // comes out right rather than sending the clock backwards.
    return (int)((((long long)(ULONG)(ms - msbase)) * TICRATE) / 1000);
}



//
// I_Init
//
void I_Init (void)
{
    I_InitSound();

    //
    // And the music, which upstream never had and so nothing ever called.
    //
    // The Linux original's I_InitMusic is an empty function, so leaving it
    // uncalled cost nothing there and the omission carried over unnoticed.
    // Here it is where the temporary file for the converted MIDI is chosen,
    // and without it that path stayed empty -- so every level failed at the
    // write, blaming a file it had never been told the name of.
    //
    I_InitMusic();

    //  I_InitGraphics();
}

//
// I_Quit
//
void I_Quit (void)
{
    D_QuitNetGame ();
    I_ShutdownSound();
    I_ShutdownMusic();
    M_SaveDefaults ();
    I_ShutdownGraphics();

    //
    // DosExit rather than exit().
    //
    // By this point the configuration is written and everything worth
    // flushing has been flushed, and the only thing left to do is stop.
    // exit() will not always manage that here: this process has a
    // Presentation Manager message queue, it may still have an MMPM/2
    // callback thread of its own inside the audio driver, and the run-time's
    // orderly shutdown can sit waiting on either of them for ever.  A
    // program that will not die leaves the session it was started from open
    // behind it, which is exactly what that looks like from the desktop.
    //
    // EXIT_PROCESS stops every thread in the process at once and is not
    // able to block.
    //
    fflush (NULL);
    DosExit (EXIT_PROCESS, 0);
}

void I_WaitVBL(int count)
{
    // There is no retrace to wait for through Presentation Manager, so this
    // is what the Linux build did with usleep: give up the processor for
    // about the right length of time.  DosSleep(0) yields without sleeping,
    // which is not what is wanted, so never round down to nothing.
    ULONG	ms = (ULONG)count * 1000 / 70;

    DosSleep (ms ? ms : 1);
}

void I_BeginRead(void)
{
}

void I_EndRead(void)
{
}

byte*	I_AllocLow(int length)
{
    byte*	mem;

    mem = (byte *)malloc (length);
    if (!mem)
	I_Error ("I_AllocLow: failed on %i bytes", length);
    memset (mem,0,length);
    return mem;
}


//
// I_OS2_MorphToPM
//
// A process's type lives in its own process information block, and OS/2 lets
// it be changed there.  Going from 2 (VIO, a text-mode session) to 3 (PM) is
// what makes WinInitialize legal from a program that was linked as a command
// line tool -- and the reason this port can still write its startup banner to
// the session it was launched from.
//
// Type 3 is already PM, which is the case if MKOS2.CMD was edited to link
// with "sys os2v2_pm"; there is nothing to do then.
//
boolean I_OS2_MorphToPM (void)
{
    PPIB	pib;
    PTIB	tib;

    if (DosGetInfoBlocks (&tib, &pib) != NO_ERROR)
	return false;

    if (pib->pib_ultype == 3)		// already a PM application
	return true;

    if (pib->pib_ultype != 2)		// not a VIO one either: full screen
	return false;

    pib->pib_ultype = 3;
    return true;
}


//
// I_OS2_MciEntry
//
// See os2doom.h.  Loaded once, on first use, and remembered -- including the
// failure, so a machine without MMPM/2 is not asked again every time a level
// changes.
//
PFNMCISENDCOMMAND I_OS2_MciEntry (void)
{
    static boolean		tried = false;
    static HMODULE		hmodMdm = NULLHANDLE;
    static PFNMCISENDCOMMAND	pMci = NULL;

    UCHAR			failed[CCHMAXPATH];

    if (tried)
	return pMci;

    tried = true;

    if (DosLoadModule (failed, sizeof(failed), (PSZ)"MDM", &hmodMdm)
	!= NO_ERROR)
    {
	hmodMdm = NULLHANDLE;
	return NULL;
    }

    if (DosQueryProcAddr (hmodMdm, 0, (PSZ)"mciSendCommand",
			  (PFN *)&pMci) != NO_ERROR)
    {
	DosFreeModule (hmodMdm);
	hmodMdm = NULLHANDLE;
	pMci = NULL;
    }

    return pMci;
}


//
// I_OS2_ErrorBox
//
void I_OS2_ErrorBox (char *text)
{
    // Before I_InitGraphics there is no anchor block and no queue, so there
    // is nothing to put a message box on -- the caller has already written
    // the same text to stderr, which in a VIO session is where the user is
    // looking anyway.
    if (os2_hab == NULLHANDLE)
	return;

    WinMessageBox (HWND_DESKTOP,
		   os2_hwndFrame ? os2_hwndFrame : HWND_DESKTOP,
		   (PSZ)text,
		   (PSZ)"DOOM",
		   0,
		   MB_OK | MB_ERROR | MB_MOVEABLE);
}


//
// I_Error
//
extern boolean demorecording;

void I_Error (char *error, ...)
{
    va_list	argptr;
    char	message[1024];

    // Message first.  Formatted once, into a buffer, so that the same text
    // can go to stderr and to the message box -- a varargs list cannot be
    // walked twice.
    va_start (argptr,error);
    vsprintf (message, error, argptr);
    va_end (argptr);

    fprintf (stderr, "Error: %s\n", message);
    fflush( stderr );

    // Into the transcript too.  This is the single most useful line the file
    // will ever hold, and stderr is about to disappear with the session.
    I_OS2_LogWrite ("Error: ");
    I_OS2_LogWrite (message);
    I_OS2_LogWrite ("\n");

    // Shutdown. Here might be other errors.
    if (demorecording)
	G_CheckDemoStatus();

    D_QuitNetGame ();
    I_ShutdownSound ();
    I_ShutdownGraphics ();

    // After I_ShutdownGraphics the window is gone but the anchor block is
    // still valid, which is exactly what a message box needs -- and with the
    // window gone there is nothing left covering the desktop for it to
    // appear behind.
    I_OS2_ErrorBox (message);

    // See I_Quit: the run-time's exit can hang here, and a program that will
    // not die leaves its session open behind it.
    fflush (NULL);
    DosExit (EXIT_PROCESS, (ULONG)-1);
}
