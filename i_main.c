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
//	Main program, simply calls D_DoomMain high level loop.
//	OS/2 version.
//
//-----------------------------------------------------------------------------

static const char
rcsid[] = "$Id: i_main.c,v 1.4 1997/02/03 22:45:10 b1 Exp $";

#include <stdio.h>

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
    myargc = argc;
    myargv = argv;

    // DOOM writes its startup banner a line at a time and expects to see it
    // as it goes.  A VIO session gives stdout a full buffer, so without this
    // the whole banner arrives in one lump -- or, if a WAD is missing and
    // I_Error runs first, never arrives at all.
    setvbuf (stdout, NULL, _IONBF, 0);

    D_DoomMain ();

    return 0;
}
