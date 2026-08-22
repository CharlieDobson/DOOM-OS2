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


#endif // __OS2DOOM__
//-----------------------------------------------------------------------------
//
// $Log:$
//
//-----------------------------------------------------------------------------
