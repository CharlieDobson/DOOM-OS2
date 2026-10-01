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
//	The little that the four OS/2 platform modules -- I_MAIN, I_SYSTEM,
//	I_VIDEO, I_SOUND and I_NET -- have to say to each other.
//
//	Nothing in here is DOOM: it is the handful of handles that Presentation
//	Manager makes process-wide, plus the two services (morphing, and an
//	error box) that more than one of those modules needs.
//
//-----------------------------------------------------------------------------

#ifndef __OS2DOOM__
#define __OS2DOOM__

#define INCL_WIN
#define INCL_GPI
#define INCL_DOSPROCESS
#define INCL_DOSMODULEMGR
#define INCL_DOSSEMAPHORES
#define INCL_DOSERRORS
#define INCL_DOSEXCEPTIONS
#include <os2.h>

#include "doomtype.h"


//
// The Presentation Manager handles.
//
// All of these are NULLHANDLE until I_InitGraphics has run, and code that
// might run before it -- I_Error most of all, which the WAD loader can reach
// long before there is a window -- has to check.  That is the whole reason
// they are shared rather than static to I_VIDEO.C.
//
extern HAB	os2_hab;		// anchor block
extern HMQ	os2_hmq;		// message queue
extern HWND	os2_hwndFrame;		// frame window
extern HWND	os2_hwndClient;		// client window we blit into


//
// I_OS2_MorphToPM
//
// Turns this process from a text-mode (VIO) one into a Presentation Manager
// one, in place, by poking the process type in its own information block.
//
// This is what lets DOOM keep printing its startup banner -- "W_Init: Init
// WADfiles", the whole familiar wall of it -- to the OS/2 command line it was
// launched from, and still open a PM window a moment later.  A program linked
// as a PM application from the start (wlink sys os2v2_pm) has nowhere to put
// that text; a plain VIO application cannot call WinInitialize.  Morphing is
// the documented way to have both.
//
// Returns false if the process could not be morphed, in which case
// WinInitialize will fail and there is no point going on.  Harmless to call
// more than once, and harmless when the program was already linked as PM.
//
boolean I_OS2_MorphToPM (void);


//
// I_OS2_PumpMessages
//
// Drains the PM message queue without blocking.
//
// DOOM's main loop never returns, so there is no WinGetMsg loop anywhere in
// this port: the window procedure is driven from here, and this is called
// from I_StartTic and again from I_FinishUpdate.  That is often enough to
// keep the single input queue happy -- the game touches it at least 35 times
// a second -- which is what stops the desktop deciding the window has hung.
//
void I_OS2_PumpMessages (void);


//
// I_OS2_ErrorBox
//
// Puts text in front of the user when stderr may not be visible: I_Error
// after the window is up, or any failure in a build linked as pure PM.
// Falls back to stderr alone when PM is not initialised yet.
//
void I_OS2_ErrorBox (char *text);


//
// I_OS2_MciEntry
//
// mciSendCommand, the one door into MMPM/2, resolved by name out of MDM.DLL
// on first use and remembered.  Returns NULL on a machine with no multimedia
// support installed, which is a perfectly ordinary thing for an OS/2 machine
// to be.
//
// It is loaded rather than imported for the same reason DIVE is: a DOOM.EXE
// that imported MDM.DLL would refuse to load at all without it, instead of
// simply running quietly.  Both the sound (I_SOUND.C) and the music
// (I_OS2MUS.C) go through here, so the library is loaded once however many
// of them end up being used.
//
typedef ULONG (APIENTRY *PFNMCISENDCOMMAND)(USHORT, USHORT, ULONG,
					    PVOID, USHORT);

PFNMCISENDCOMMAND I_OS2_MciEntry (void);


//
// D_OS2FindIWAD
//
// Finds the game data and works out which game it is, by reading the WAD
// rather than by trusting its file name.  Called from IdentifyVersion.
// Lives in D_OS2IWD.C; declared here because it is part of the port rather
// than part of the engine.
//
// Returns true once an IWAD has been adopted.  Does not return at all if
// there is none -- it reports where it looked and stops.
//
boolean D_OS2FindIWAD (void);


//
// I_OS2_ExceptionHandler
//
// Registered on the main thread by main(), in I_MAIN.C.
//
// Its one job is to put the mouse pointer back.  While the game is running
// the pointer is hidden -- WinShowPointer(FALSE) -- and that is a system
// wide, reference counted state: if DOOM traps while holding it hidden, the
// desktop is left with no pointer at all and the user has to reboot to get
// one back.  That is a rotten thing to do to somebody over a bug in a game.
//
// It never tries to handle the fault.  It always returns
// XCPT_CONTINUE_SEARCH, so OS/2 goes on to produce its usual popup and
// process dump exactly as it would have.
//
ULONG APIENTRY I_OS2_ExceptionHandler (PEXCEPTIONREPORTRECORD	 report,
				       PEXCEPTIONREGISTRATIONRECORD reg,
				       PCONTEXTRECORD		 ctx,
				       PVOID			 dummy);


//
// I_OS2_MusicNotify
//
// Called from the window procedure when MM_MCINOTIFY arrives, carrying the
// status MMPM/2 reported.  Defined in I_OS2MUS.C, which uses it to start a
// looping song again when it reaches the end.
//
void I_OS2_MusicNotify (ULONG status);


//
// I_OS2_PlaylistNotify
//
// Called from the window procedure when MM_MCIPLAYLISTMESSAGE arrives,
// carrying the number of the sound block the device has finished with.
// Defined in I_SOUND.C, which mixes the next one into it.  Only used on
// machines whose audio driver has no DART.
//
void I_OS2_PlaylistNotify (ULONG which);


//
// Choosing between IWADs, in I_OS2WAD.C.
//
// A directory can easily hold several games at once -- DOOM.WAD, DOOM2.WAD,
// TNT.WAD and PLUTONIA.WAD side by side is a perfectly ordinary way to keep
// them -- and the released code simply takes the first name off a list and
// plays that one, with no way to say otherwise except renaming files.
//
// So when the search turns up more than one, the player is asked.  D_OS2IWD.C
// finds and identifies them; this only puts the question.
//
#define MAX_IWAD_CHOICES	12

typedef struct
{
    char	path[CCHMAXPATH];	// as it will be opened
    char	name[64];		// "The Ultimate DOOM", and so on
    int		mode;			// GameMode_t
    int		mission;		// GameMission_t
} os2iwad_t;

//
// The two enumerated types are carried as plain ints on purpose.  Their
// definitions live in doomdef.h and doomstat.h, which this header is included
// alongside but never by -- I_VIDEO.C and I_SOUND.C both take pains to keep
// the DOOM headers and the OS/2 ones apart, because os2medef.h declares a
// type called VERSION and doomdef.h has an enumerator of the same name.  One
// struct is not worth reopening that.
//

//
// I_OS2_ChooseIwad
//
// Puts up the list and blocks until the player picks one.  Returns the index
// chosen, or -1 if they would rather not play after all.
//
// "preferred" is the one selected when the window opens: the caller passes
// whichever the old first-match-wins order would have started, so that simply
// pressing Enter keeps doing what this program has always done.
//
int I_OS2_ChooseIwad (os2iwad_t *list, int count, int preferred);


//
// I_OS2_PlaylistPlayDone
//
// Called from the window procedure when MM_MCINOTIFY arrives, carrying the id
// of the device whose play has finished.  The music sequencer reports to the
// same window, so this is offered the message first and returns true only if
// the device named is the playlist's -- in which case the next pass goes in.
// Returns false for anything else, including on machines that never opened a
// playlist at all, and the message goes to the music instead.
//
boolean I_OS2_PlaylistPlayDone (ULONG deviceID);


//
// The startup transcript, in I_OS2LOG.C.  LogInit opens DOOM.LOG and must be
// called before anything prints; LogWrite puts already-formatted text into it
// without going near stdout, which is what I_Error wants on its way out.
//
void I_OS2_LogInit (void);
void I_OS2_LogShutdown (void);
void I_OS2_LogWrite (const char* text);


//
// I_OS2_InitPM
//
// Become a Presentation Manager process and make the anchor block and message
// queue.  Idempotent: the loading window asks for this early and
// I_InitGraphics asks again later.  Defined in I_SYSTEM.C.
//
boolean I_OS2_InitPM (void);


//
// The loading window, in I_OS2LOAD.C.  LoadNotice is handed every line the
// game prints on its way up and moves the bar when one names a stage;
// LoadWindowClose is called once the game's own window is on the screen.
//
void I_OS2_LoadWindowOpen (void);
void I_OS2_LoadNotice (const char* line);
void I_OS2_LoadPump (void);
void I_OS2_LoadWindowClose (void);


#endif // __OS2DOOM__
