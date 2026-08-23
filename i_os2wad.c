// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// $Id:$
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
//	"Which game?" -- the window that appears when a directory holds more
//	than one IWAD.  OS/2 version; there is no equivalent upstream.
//
//	Keeping DOOM, DOOM II, TNT and Plutonia in one directory is the
//	ordinary way to have them, and the released code deals with it by
//	taking the first name off a fixed list and playing that.  Which game
//	starts is then decided by a preference order written in 1993, and the
//	only way to overrule it is to rename files.
//
//	D_OS2IWD.C now collects all of them instead of stopping at the first.
//	This asks which one to play.
//
//	The window is built control by control rather than loaded from a
//	dialog template.  The template would be the conventional way, but the
//	resource script is only bound into the executable when there is an
//	icon to bind with it -- see MKOS2.CMD -- and a dialog that appears only
//	on some builds is worse than no dialog at all.
//
//-----------------------------------------------------------------------------

static const char
rcsid[] = "$Id:$";

#include <stdio.h>
#include <string.h>

#include "os2doom.h"


#define CLASS_NAME	"DoomIwadChoice"

#define CLIENT_W	560

// Everything above the list, and everything below it.  The window is as tall
// as it needs to be for however many WADs were found.
#define TOP_H		56
#define ROW_H		26
#define BOTTOM_H	72

#define ID_FIRST_WAD	100
#define ID_PLAY		200
#define ID_QUIT		201


//
// The answer, and whether there is one yet.
//
// File scope rather than passed about because this is used once, from one
// thread, and blocks until it is finished: a window word or a WM_INITDLG
// parameter would be ceremony around a variable that cannot be reentered.
//
static int	choiceMade	= -1;
static boolean	choiceDone	= false;
static int	choiceCount	= 0;


//
// FitLabel
//
// Builds the text for one radio button: the name of the game, then the file
// it was found in.
//
// A path can be longer than the window, and the useful end of a path is the
// right-hand one -- the directory it is in and the file name, not the drive
// letter.  So an over-long one is trimmed from the left and marked with an
// ellipsis, which is the opposite of what truncation usually does and the
// right way round here.
//
static void FitLabel (os2iwad_t *w, char *out, int max)
{
    int		room;
    int		pathlen;

    strcpy (out, w->name);
    strcat (out, "   ");

    room    = max - (int)strlen (out) - 1;
    pathlen = (int)strlen (w->path);

    if (room < 8)
	return;

    if (pathlen <= room)
	strcat (out, w->path);
    else
    {
	strcat (out, "...");
	strcat (out, w->path + pathlen - (room - 3));
    }
}


//
// MakeControls
//
// Called once the frame is its final size, so that the client these sit on is
// already the size they were laid out for.
//
static void MakeControls (HWND hwnd, os2iwad_t *list, int count, int preferred)
{
    int		clientH = TOP_H + count * ROW_H + BOTTOM_H;
    int		y;
    int		i;

    WinCreateWindow (hwnd, (PSZ)WC_STATIC,
		     (PSZ)"More than one game was found.  Which one would you "
			  "like to play?",
		     WS_VISIBLE | SS_TEXT | DT_LEFT | DT_VCENTER,
		     16, clientH - 36, CLIENT_W - 32, 22,
		     hwnd, HWND_TOP, -1, NULL, NULL);

    y = clientH - TOP_H;

    for (i = 0; i < count; i++)
    {
	char	label[CCHMAXPATH + 80];
	ULONG	style = WS_VISIBLE | BS_AUTORADIOBUTTON;

	FitLabel (&list[i], label, sizeof(label));

	//
	// The first button of a group carries WS_GROUP and WS_TABSTOP, and
	// that is what makes the arrow keys walk the list and Tab jump past
	// it to the buttons.  Without it every radio button is its own group
	// and the arrow keys do nothing at all.
	//
	if (!i)
	    style |= WS_GROUP | WS_TABSTOP;

	y -= ROW_H;

	WinCreateWindow (hwnd, (PSZ)WC_BUTTON, (PSZ)label,
			 style,
			 24, y, CLIENT_W - 48, 22,
			 hwnd, HWND_TOP, ID_FIRST_WAD + i, NULL, NULL);
    }

    WinCreateWindow (hwnd, (PSZ)WC_BUTTON, (PSZ)"~Play",
		     WS_VISIBLE | WS_GROUP | WS_TABSTOP
		     | BS_PUSHBUTTON | BS_DEFAULT,
		     CLIENT_W - 230, 20, 100, 32,
		     hwnd, HWND_TOP, ID_PLAY, NULL, NULL);

    WinCreateWindow (hwnd, (PSZ)WC_BUTTON, (PSZ)"~Quit",
		     WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
		     CLIENT_W - 120, 20, 100, 32,
		     hwnd, HWND_TOP, ID_QUIT, NULL, NULL);

    // Start on whichever one the old order would have played, so that Enter
    // by itself does what this program has always done.
    WinSendDlgItemMsg (hwnd, ID_FIRST_WAD + preferred, BM_SETCHECK,
		       MPFROMSHORT(1), 0);

    WinSetFocus (HWND_DESKTOP,
		 WinWindowFromID (hwnd, ID_FIRST_WAD + preferred));
}


//
// ReadChoice
//
// Which radio button is set.  Asked at the end rather than tracked as the
// player moves, because BS_AUTORADIOBUTTON maintains it for us and reading it
// once is less to get wrong than following every WM_CONTROL.
//
static int ReadChoice (HWND hwnd)
{
    int		i;

    for (i = 0; i < choiceCount; i++)
	if ((ULONG)WinSendDlgItemMsg (hwnd, ID_FIRST_WAD + i,
				      BM_QUERYCHECK, 0, 0))
	    return i;

    return 0;
}


//
// ChoiceWndProc
//
static MRESULT EXPENTRY ChoiceWndProc
( HWND	hwnd,
  ULONG	msg,
  MPARAM mp1,
  MPARAM mp2 )
{
    switch (msg)
    {
      case WM_COMMAND:
	switch (SHORT1FROMMP(mp1))
	{
	  case ID_PLAY:
	    choiceMade = ReadChoice (hwnd);
	    choiceDone = true;
	    return 0;

	  case ID_QUIT:
	    choiceMade = -1;
	    choiceDone = true;
	    return 0;
	}
	break;

      //
      // A double click on one of the names plays it, which is what anyone who
      // has used a file dialog will try first.
      //
      case WM_CONTROL:
	if (SHORT2FROMMP(mp1) == BN_DBLCLICKED)
	{
	    int	id = SHORT1FROMMP(mp1);

	    if (id >= ID_FIRST_WAD && id < ID_FIRST_WAD + choiceCount)
	    {
		choiceMade = id - ID_FIRST_WAD;
		choiceDone = true;
		return 0;
	    }
	}
	break;

      //
      // Escape, and the close box on the title bar, both mean "not this
      // time".  Neither is a crash and neither should start a game the player
      // did not choose.
      //
      case WM_CHAR:
	if ((SHORT1FROMMP(mp1) & KC_VIRTUALKEY)
	    && !(SHORT1FROMMP(mp1) & KC_KEYUP)
	    && SHORT2FROMMP(mp2) == VK_ESC)
	{
	    choiceMade = -1;
	    choiceDone = true;
	    return (MRESULT)TRUE;
	}
	break;

      case WM_CLOSE:
	choiceMade = -1;
	choiceDone = true;
	return 0;
    }

    return WinDefWindowProc (hwnd, msg, mp1, mp2);
}


//
// I_OS2_ChooseIwad
//
int I_OS2_ChooseIwad (os2iwad_t *list, int count, int preferred)
{
    HWND	hwndFrame;
    HWND	hwndClient = NULLHANDLE;
    ULONG	flFrame;
    RECTL	rcl;
    QMSG	qmsg;
    LONG	frame_cx, frame_cy;
    LONG	scr_cx, scr_cy;
    int		clientH;

    if (count < 1)
	return -1;

    if (count == 1)
	return 0;

    if (count > MAX_IWAD_CHOICES)
	count = MAX_IWAD_CHOICES;

    if (preferred < 0 || preferred >= count)
	preferred = 0;

    // PM may not be up yet if the loading window could not be opened.  There
    // is then nowhere to ask, so the old behaviour stands: play the one the
    // preference order picked.
    if (!I_OS2_InitPM () || os2_hab == NULLHANDLE)
	return preferred;

    choiceCount = count;
    choiceMade  = preferred;
    choiceDone  = false;

    if (!WinRegisterClass (os2_hab, (PSZ)CLASS_NAME, ChoiceWndProc, 0, 0))
	return preferred;

    flFrame = FCF_TITLEBAR | FCF_SYSMENU | FCF_DLGBORDER | FCF_TASKLIST;

    //
    // WS_CLIPCHILDREN is not optional: PM does not clip a parent's drawing to
    // exclude its children, so without it the client's own WM_PAINT erases
    // the whole client area straight over the controls and nothing puts them
    // back.  The window comes up correctly sized, correctly titled and
    // completely empty.  See OS2SETUP.C, where that took a while to find.
    //
    hwndFrame = WinCreateStdWindow (HWND_DESKTOP, 0, &flFrame,
				    (PSZ)CLASS_NAME,
				    (PSZ)"DOOM for OS/2",
				    WS_CLIPCHILDREN,
				    NULLHANDLE, 0, &hwndClient);

    if (hwndFrame == NULLHANDLE || hwndClient == NULLHANDLE)
    {
	if (hwndFrame != NULLHANDLE)
	    WinDestroyWindow (hwndFrame);

	return preferred;
    }

    clientH = TOP_H + count * ROW_H + BOTTOM_H;

    rcl.xLeft	= 0;
    rcl.yBottom	= 0;
    rcl.xRight	= CLIENT_W;
    rcl.yTop	= clientH;
    WinCalcFrameRect (hwndFrame, &rcl, FALSE);

    frame_cx = rcl.xRight - rcl.xLeft;
    frame_cy = rcl.yTop - rcl.yBottom;

    scr_cx = WinQuerySysValue (HWND_DESKTOP, SV_CXSCREEN);
    scr_cy = WinQuerySysValue (HWND_DESKTOP, SV_CYSCREEN);

    WinSetWindowPos (hwndFrame, HWND_TOP,
		     (scr_cx - frame_cx) / 2, (scr_cy - frame_cy) / 2,
		     frame_cx, frame_cy,
		     SWP_SIZE | SWP_MOVE);

    MakeControls (hwndClient, list, count, preferred);

    WinSetWindowPos (hwndFrame, HWND_TOP, 0, 0, 0, 0,
		     SWP_SHOW | SWP_ACTIVATE | SWP_ZORDER);

    //
    // A message loop of its own, rather than WinProcessDlg.
    //
    // WinProcessDlg wants a window made by WinLoadDlg or WinCreateDlg, and
    // this one was not.  Running the loop here is no hardship: nothing else
    // in the game has started yet, so there is nothing this can block.  The
    // loading window is on screen behind it and keeps repainting, because its
    // messages come through this same loop.
    //
    while (!choiceDone && WinGetMsg (os2_hab, &qmsg, 0, 0, 0))
	WinDispatchMsg (os2_hab, &qmsg);

    WinDestroyWindow (hwndFrame);

    // Drain whatever the window left behind -- the repaints its going away
    // caused, most of all -- so the loading window is whole again before the
    // WAD starts loading in earnest.
    while (WinPeekMsg (os2_hab, &qmsg, 0, 0, 0, PM_REMOVE))
	WinDispatchMsg (os2_hab, &qmsg);

    return choiceDone ? choiceMade : preferred;
}
