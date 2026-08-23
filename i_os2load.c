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
//	The loading window.  OS/2 version; there is no equivalent upstream.
//
//	DOOM's startup is a page of running commentary aimed at whoever built
//	it -- Z_Init, R_InitTables, R_InitTranslationsTables.  It tells a
//	player nothing except that the machine has not hung, and on a 486 it
//	has to tell them that for a good many seconds.
//
//	So the commentary goes to DOOM.LOG, where it is useful afterwards, and
//	what appears on screen is a bar and a plain sentence saying what is
//	happening.  When the game window opens, this one closes.
//
//	Nothing here is driven from the engine.  The startup lines are simply
//	watched as they go past -- I_OS2LOG.C hands each one over -- and the
//	ones that mark a stage move the bar on.  That means no calls scattered
//	through D_DoomMain, and nothing to keep in step when the engine
//	changes: a line that stops being printed stops being a stage, and the
//	bar is no less honest for it.
//
//-----------------------------------------------------------------------------

#include <stdio.h>
#include <string.h>

#include "os2doom.h"

#include "doomtype.h"
#include "m_argv.h"

#define LOAD_CLASS	"DoomLoading"

#define LOAD_W		380
#define LOAD_H		110

// Margins inside the client area.
#define PAD		16
#define BAR_H		18


//
// The stages, in the order DOOM prints them.
//
// The text is what the player is told; the prefix is what is watched for.
// R_Init is by far the longest of these on a slow machine -- it is where
// every texture, flat and sprite in the WAD is composed -- so it is given
// several stages of its own from the lines it prints while it works.
//
static const struct
{
    char*	prefix;
    char*	text;
}
stages[] =
{
    { "V_Init:",		"Allocating screens"			},
    { "M_LoadDefaults:",	"Reading DEFAULT.CFG"			},
    { "Z_Init:",		"Setting up memory"			},
    { "W_Init:",		"Opening the WAD"			},
    { "M_Init:",		"Starting up"				},
    { "R_Init:",		"Building the renderer"			},
    { "InitTextures",		"Composing wall textures"		},
    { "InitFlats",		"Composing floors and ceilings"		},
    { "InitSprites",		"Composing sprites"			},
    { "InitColormaps",		"Loading the light tables"		},
    { "R_InitData",		"Preparing render data"			},
    { "P_Init:",		"Starting the play loop"		},
    { "I_Init:",		"Setting up sound"			},
    { "D_CheckNetGame:",	"Checking for other players"		},
    { "S_Init:",		"Starting sound and music"		},
    { "HU_Init:",		"Setting up the display"		},
    { "ST_Init:",		"Setting up the status bar"		}
};

#define NUM_STAGES	(int)(sizeof(stages)/sizeof(stages[0]))


static HWND	hwndLoadFrame	= NULLHANDLE;
static HWND	hwndLoad	= NULLHANDLE;
static boolean	loadOpen	= false;
static int	loadStage	= 0;
static char	loadText[80]	= "Starting";


//
// PaintLoad
//
// The whole window, drawn by hand.
//
// Presentation Manager on OS/2 2.x has no progress bar control -- that
// arrived with the Warp toolkit's container and slider classes and cannot be
// relied on here -- so it is two rectangles and a line of text, which is all
// a progress bar has ever been.
//
static void PaintLoad (HPS hps, PRECTL prcl)
{
    RECTL	rcl;
    POINTL	pt;
    LONG	barTop, barBot, barRight;
    LONG	w;

    // Background.
    WinFillRect (hps, prcl, CLR_PALEGRAY);

    w = prcl->xRight - prcl->xLeft;

    barBot	= PAD;
    barTop	= barBot + BAR_H;
    barRight	= prcl->xLeft + PAD
		+ (w - 2*PAD) * loadStage / NUM_STAGES;

    // The filled part.  Drawn before the frame so the frame stays crisp.
    if (barRight > prcl->xLeft + PAD)
    {
	rcl.xLeft   = prcl->xLeft + PAD;
	rcl.yBottom = barBot;
	rcl.xRight  = barRight;
	rcl.yTop    = barTop;

	WinFillRect (hps, &rcl, CLR_DARKBLUE);
    }

    // The frame around it.
    GpiSetColor (hps, CLR_DARKGRAY);

    pt.x = prcl->xLeft + PAD;
    pt.y = barBot;
    GpiMove (hps, &pt);

    pt.x = prcl->xRight - PAD - 1;
    pt.y = barTop;
    GpiBox (hps, DRO_OUTLINE, &pt, 0, 0);

    // And what is happening, above it.
    rcl.xLeft	= prcl->xLeft + PAD;
    rcl.xRight	= prcl->xRight - PAD;
    rcl.yBottom	= barTop + 12;
    rcl.yTop	= prcl->yTop - PAD;

    GpiSetColor (hps, CLR_BLACK);
    WinDrawText (hps, -1, (PSZ)loadText, &rcl, CLR_BLACK, CLR_PALEGRAY,
		 DT_LEFT | DT_VCENTER | DT_TEXTATTRS);
}


//
// LoadWndProc
//
static MRESULT EXPENTRY LoadWndProc (HWND hwnd, ULONG msg,
				     MPARAM mp1, MPARAM mp2)
{
    switch (msg)
    {
      case WM_PAINT:
      {
	RECTL	rcl;
	HPS	hps = WinBeginPaint (hwnd, NULLHANDLE, NULLHANDLE);

	WinQueryWindowRect (hwnd, &rcl);
	PaintLoad (hps, &rcl);

	WinEndPaint (hps);
	return 0;
      }

      case WM_ERASEBACKGROUND:
	// PaintLoad fills every pixel; letting PM erase first only flickers.
	return (MRESULT)FALSE;

      case WM_CLOSE:
	// There is nothing to cancel: the game is coming up either way, and a
	// window that vanishes while the machine is still working would look
	// more like a fault than a courtesy.
	return 0;

      default:
	break;
    }

    return WinDefWindowProc (hwnd, msg, mp1, mp2);
}


//
// I_OS2_LoadWindowOpen
//
// Failure is not an error.  Without this window the game still starts, and
// the startup text still goes to the session and the log; all that is lost is
// something to look at.
//
void I_OS2_LoadWindowOpen (void)
{
    ULONG	flFrame;
    LONG	scr_cx, scr_cy;
    RECTL	rcl;
    LONG	frame_cx, frame_cy;

    if (loadOpen || M_CheckParm ("-noloadwindow"))
	return;

    if (!I_OS2_InitPM ())
	return;

    if (!WinRegisterClass (os2_hab, (PSZ)LOAD_CLASS, LoadWndProc,
			   CS_SIZEREDRAW, 0))
	return;

    // A caption and a thin border, and nothing else.  No system menu, no
    // buttons: there is nothing useful to do to this window while it is up.
    flFrame = FCF_TITLEBAR | FCF_BORDER;

    hwndLoadFrame = WinCreateStdWindow (HWND_DESKTOP, 0, &flFrame,
					(PSZ)LOAD_CLASS,
					(PSZ)"Starting DOOM",
					0, NULLHANDLE, 0, &hwndLoad);

    if (hwndLoadFrame == NULLHANDLE)
	return;

    // Work out the frame that holds a client area of the size wanted, then
    // put it in the middle of the screen.
    rcl.xLeft	= 0;
    rcl.yBottom	= 0;
    rcl.xRight	= LOAD_W;
    rcl.yTop	= LOAD_H;
    WinCalcFrameRect (hwndLoadFrame, &rcl, FALSE);

    frame_cx = rcl.xRight - rcl.xLeft;
    frame_cy = rcl.yTop - rcl.yBottom;

    scr_cx = WinQuerySysValue (HWND_DESKTOP, SV_CXSCREEN);
    scr_cy = WinQuerySysValue (HWND_DESKTOP, SV_CYSCREEN);

    WinSetWindowPos (hwndLoadFrame, HWND_TOP,
		     (scr_cx - frame_cx) / 2, (scr_cy - frame_cy) / 2,
		     frame_cx, frame_cy,
		     SWP_SIZE | SWP_MOVE | SWP_SHOW | SWP_ACTIVATE);

    loadOpen  = true;
    loadStage = 0;

    I_OS2_LoadPump ();
}


//
// I_OS2_LoadPump
//
// Repaint, now, and let Presentation Manager get on with whatever else it
// wants.
//
// Startup runs straight through without a message loop, so a window left to
// its own devices would never paint at all.  Everything worth doing here is
// done between two stages of a startup that is otherwise busy, so a few
// messages dispatched now and then cost nothing that would be noticed.
//
void I_OS2_LoadPump (void)
{
    QMSG	qmsg;

    if (!loadOpen)
	return;

    WinInvalidateRect (hwndLoad, NULL, FALSE);

    while (WinPeekMsg (os2_hab, &qmsg, NULLHANDLE, 0, 0, PM_REMOVE))
	WinDispatchMsg (os2_hab, &qmsg);
}


//
// I_OS2_LoadNotice
//
// One line of DOOM's startup commentary, on its way to the log.  If it names
// a stage, the bar moves.
//
// Matched from the front rather than compared whole: most of these lines have
// something after the colon, and one of them -- R_Init -- has a row of dots
// that grows as it works.
//
void I_OS2_LoadNotice (const char* line)
{
    int		i;

    if (!loadOpen)
	return;

    for (i = 0; i < NUM_STAGES; i++)
    {
	if (strncmp (line, stages[i].prefix, strlen (stages[i].prefix)))
	    continue;

	// Never backwards.  The same prefix can appear more than once and a
	// bar that retreats looks like something has gone wrong.
	if (i + 1 <= loadStage)
	    break;

	loadStage = i + 1;
	strcpy (loadText, stages[i].text);

	I_OS2_LoadPump ();
	break;
    }
}


//
// I_OS2_LoadWindowClose
//
// Called when the game window is up and this one has nothing left to say.
//
void I_OS2_LoadWindowClose (void)
{
    if (!loadOpen)
	return;

    loadOpen = false;

    if (hwndLoadFrame != NULLHANDLE)
    {
	WinDestroyWindow (hwndLoadFrame);
	hwndLoadFrame = NULLHANDLE;
	hwndLoad      = NULLHANDLE;
    }
}
