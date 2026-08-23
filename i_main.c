// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// $Id:$
//
// Copyright (C) 1993-1996 by id Software, Inc.
// OS/2 Port by Charlie Dobson
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
//	Main program, simply calls D_DoomMain high level loop.
//	OS/2 version.
//
//-----------------------------------------------------------------------------

static const char
rcsid[] = "$Id: i_main.c,v 1.4 1997/02/03 22:45:10 b1 Exp $";

#include <stdio.h>

#include "os2doom.h"

#include "doomdef.h"

#include "m_argv.h"
#include "d_main.h"

//
// Entry point.
//
// This is still a plain C main(), and the program is still linked as a
// text-mode (VIO) application -- see MKOS2.CMD.  It becomes a Presentation
// Manager application later, when I_InitGraphics calls I_OS2_MorphToPM, and
// not before; everything D_DoomMain prints on the way to that point lands in
// the OS/2 window the game was started from, exactly as DOOM's startup text
// has always done.
//
int
main
( int		argc,
  char**	argv )
{
    // The exception registration record has to live on the stack of the
    // thread it protects.  OS/2 keeps these in a chain rooted in the thread
    // information block and checks that each one lies inside that thread's
    // stack, so a static or a global is refused outright with
    // ERROR_INVALID_ADDRESS.  main()'s frame lasts as long as the program
    // does, which makes this the right place for it.
    EXCEPTIONREGISTRATIONRECORD	xcpt;

    myargc = argc;
    myargv = argv;

    // DOOM writes its startup banner a line at a time and expects to see it
    // as it goes.  A VIO session gives stdout a full buffer, so without this
    // the whole banner arrives in one lump -- or, if a WAD is missing and
    // I_Error runs first, never arrives at all.
    setvbuf (stdout, NULL, _IONBF, 0);

    // Open the transcript before anything has had a chance to print into it.
    // It needs myargc and myargv, which is why it comes after those and not
    // at the very top: -nolog switches it off.
    I_OS2_LogInit ();

    // And something for the player to look at while the WAD is read.  This
    // is deliberately after the log: if opening the window fails, the run is
    // still recorded.
    I_OS2_LoadWindowOpen ();

    // Insurance against leaving the desktop with no mouse pointer at all if
    // the game traps while it has the pointer hidden.  See os2doom.h.
    xcpt.prev_structure   = NULL;
    xcpt.ExceptionHandler = I_OS2_ExceptionHandler;
    DosSetExceptionHandler (&xcpt);

    D_DoomMain ();

    DosUnsetExceptionHandler (&xcpt);

    return 0;
}
