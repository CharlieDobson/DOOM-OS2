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
//	DOOM graphics stuff for OS/2 2.x and Warp, Presentation Manager.
//
//	This replaces the Linux X11 module.  Two things about it are worth
//	knowing before reading on.
//
//	1. There are two ways out to the screen, chosen at startup:
//
//	   DIVE  - Direct Interface Video Extensions.  The fast path.  DIVE
//	           takes the 320x200 8-bit image and blits it to the visible
//	           part of the window, scaling and colour-converting in the
//	           display driver, which is as close to the hardware as a PM
//	           program is allowed to get.  It lives in DIVE.DLL, part of
//	           MMPM/2, and is therefore not on every machine.
//
//	   GPI   - GpiDrawBits.  The fallback, and it works everywhere back to
//	           OS/2 2.0 with no multimedia installed at all.  It goes
//	           through the whole graphics engine for every frame, so it is
//	           a good deal slower, but it is correct.
//
//	   DIVE.DLL is therefore loaded by name at run time rather than
//	   imported.  Importing it would make DOOM.EXE refuse to load at all
//	   on a machine without MMPM/2, which is the one case the fallback
//	   exists to serve.
//
//	2. There is no WinGetMsg loop.  D_DoomLoop never returns, so the
//	   window procedure is driven by I_OS2_PumpMessages out of I_StartTic
//	   and I_FinishUpdate.  See os2doom.h.
//
//	Scaling is DIVE's and GPI's job, not ours: both stretch the source
//	image to whatever the window currently is.  The -2 and -3 switches the
//	Linux version used to pick a pixel-doubling routine now just choose
//	the size the window opens at, and the window can be resized freely
//	afterwards.
//
//-----------------------------------------------------------------------------

static const char
rcsid[] = "$Id: i_x.c,v 1.6 1997/02/03 22:45:10 b1 Exp $";

#define INCL_DOSMEMMGR			// DosAllocMem

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "os2doom.h"			// brings in os2.h with INCL_WIN/INCL_GPI

#include <mmioos2.h>			// mmioFOURCC, needed by fourcc.h
#include <fourcc.h>			// FOURCC_LUT8, FOURCC_SCRN
#include <dive.h>

#include "doomstat.h"
#include "i_system.h"
#include "v_video.h"
#include "m_argv.h"
#include "d_main.h"

#include "doomdef.h"

#include "i_video.h"


#define DOOM_WINDOW_CLASS	"DoomWindowClass"

// Resource id of the window icon.  Only meaningful if RESDOOM.RC was
// compiled and bound into the executable -- see MKOS2.CMD.
#define ID_DOOM_ICON		1

//
// The Presentation Manager handles, shared with the rest of the port.
//
HAB	os2_hab		= NULLHANDLE;
HMQ	os2_hmq		= NULLHANDLE;
HWND	os2_hwndFrame	= NULLHANDLE;
HWND	os2_hwndClient	= NULLHANDLE;


// Initial window size, in multiples of 320x200.  Only the startup size:
// the blitter stretches to whatever the window is at the time.
static int	multiply = 1;

// The frame buffer DOOM draws into.  screens[0] is pointed at this in
// I_InitGraphics, exactly as the X11 version pointed screens[0] at the
// XImage's own data -- it saves a 64000 byte copy every frame.
static byte*	blitbuf = NULL;

// Current client size, kept up to date from WM_SIZE so that the mouse code
// does not have to ask PM for it on every motion event.
static LONG	client_cx = SCREENWIDTH;
static LONG	client_cy = SCREENHEIGHT;


//
//  DIVE, loaded by name.
//
typedef ULONG (APIENTRY *PFNDIVEOPEN)             (HDIVE *, BOOL, PVOID);
typedef ULONG (APIENTRY *PFNDIVECLOSE)            (HDIVE);
typedef ULONG (APIENTRY *PFNDIVESETUPBLITTER)     (HDIVE, PSETUP_BLITTER);
typedef ULONG (APIENTRY *PFNDIVEBLITIMAGE)        (HDIVE, ULONG, ULONG);
typedef ULONG (APIENTRY *PFNDIVEALLOCIMAGEBUFFER) (HDIVE, PULONG, FOURCC,
						   ULONG, ULONG, ULONG, PBYTE);
typedef ULONG (APIENTRY *PFNDIVEFREEIMAGEBUFFER)  (HDIVE, ULONG);
typedef ULONG (APIENTRY *PFNDIVESETSOURCEPALETTE) (HDIVE, ULONG, ULONG, PBYTE);

static HMODULE			hmodDive	= NULLHANDLE;
static PFNDIVEOPEN		pDiveOpen;
static PFNDIVECLOSE		pDiveClose;
static PFNDIVESETUPBLITTER	pDiveSetupBlitter;
static PFNDIVEBLITIMAGE		pDiveBlitImage;
static PFNDIVEALLOCIMAGEBUFFER	pDiveAllocImageBuffer;
static PFNDIVEFREEIMAGEBUFFER	pDiveFreeImageBuffer;
static PFNDIVESETSOURCEPALETTE	pDiveSetSourcePalette;

static HDIVE	hDive		= 0;
static ULONG	diveBufNum	= 0;

static boolean	useDive		= false;	// DIVE opened and has a buffer
static boolean	diveBlitterOK	= false;	// ...and the blitter is current


//
//  GPI fallback state.
//
// GpiDrawBits reads its source bottom-up, and DOOM's frame buffer is
// top-down, so the fallback keeps a flipped copy.  The DIVE path needs no
// such thing.
//
static byte*	gpibuf = NULL;

static struct
{
    BITMAPINFOHEADER2	hdr;
    RGB2		argb[256];
} gpibmi;


//
//  Palette.
//
// DIVE wants 256 entries of blue, green, red, reserved.  GPI's RGB2 happens
// to be the same four bytes in the same order, but they are kept separately
// rather than aliased: nothing in either interface promises that, and the
// cost is one kilobyte.
//
static byte	divepal[256*4];


//
//  Input state.
//
static boolean	grabMouse	= true;		// -nograbmouse turns it off

//
// use_mouse from DEFAULT.CFG.
//
// This is the only place it can possibly be honoured.  Nothing in the engine
// consults it: G_Responder feeds every ev_mouse it is given straight into
// mousex/mousey, and G_BuildTiccmd uses those unconditionally.  On DOS the
// setting decided whether the mouse driver was initialised at all, so the
// events simply never existed.  Here the platform layer has to be the one
// that does not send them.
//
extern int	usemouse;
static boolean	mouseGrabbed	= false;	// ...and it is grabbed now
static boolean	pointerHidden	= false;
static int	mouseButtons	= 0;

// Which DOOM keys are currently held, so that they can all be released when
// the window loses the focus.  Without this, Alt-Tabbing away while running
// forward leaves the player running forward for ever.
static byte	keyIsDown[256];

// Whether the window currently has the focus.  I_FinishUpdate throttles the
// game right down when it does not: DOOM's loop renders as fast as the
// machine allows, and there is no reason to spend a whole processor drawing
// frames into a window nobody is looking at.
static boolean	windowActive	= true;

// -keydebug: print every keystroke this window is handed.  Keyboard trouble
// on OS/2 is nearly always a question of which window holds the input focus,
// and that question cannot be answered from inside the game -- either the
// messages arrive or they do not.  This makes the difference visible.
static boolean	keydebug	= false;

// Set while the window procedure is running, so that the shutdown path can
// tell whether it is being called from inside a dispatched message.  Tearing
// the message queue down from in there wedges the process.
static boolean	inWndProc	= false;

// The close box was clicked.  Acted on by the pump once the dispatch that
// set it has unwound -- see I_OS2_PumpMessages.
static boolean	os2_quitRequested = false;

//
// Window shape.
//
// DOOM's 320x200 picture is meant to be seen as 4:3 -- the pixels on the
// hardware it was written for are not square, they are half again as tall as
// they are wide.  Blitting it into a window of 320x200 square pixels makes
// everything a fifth too short, which is why the window opens at 320x240
// (times the -2 or -3 multiplier) and why the frame is held to 4:3 while it
// is being resized.
//
// -stretch turns the constraint off and lets the picture take whatever shape
// the window is given.
//
static boolean	stretchToWindow	= false;
static PFNWP	pfnFrameProc	= NULL;		// the frame's own procedure

// Remembered across runs through DEFAULT.CFG.  M_MISC.C has these in its
// defaults table; they are updated from WM_SIZE and WM_MOVE, so whatever the
// window looks like when the game exits is what it looks like next time.
// A width of zero means "nothing saved yet", and the defaults apply.
int		os2_window_x	= 0;
int		os2_window_y	= 0;
int		os2_window_w	= 0;
int		os2_window_h	= 0;


//
// RememberWindowPos
//
// Records where the frame is, for DEFAULT.CFG.  Deliberately ignores a
// minimised or maximised window: restoring DOOM to a zero-sized icon next
// time would be a poor trick.
//
static void RememberWindowPos (void)
{
    SWP		swp;

    if (os2_hwndFrame == NULLHANDLE)
	return;

    if (!WinQueryWindowPos (os2_hwndFrame, &swp))
	return;

    if ((swp.fl & (SWP_MINIMIZE | SWP_MAXIMIZE)) || swp.cx <= 0 || swp.cy <= 0)
	return;

    os2_window_x = swp.x;
    os2_window_y = swp.y;
    os2_window_w = swp.cx;
    os2_window_h = swp.cy;
}


//
// Scan code (PC set 1) to DOOM key.
//
// Scan codes are used in preference to PM's character translation because
// they are the only thing that reports the shift, control and alt keys going
// up and down -- which DOOM needs, since they are its strafe, fire and use
// keys -- and because they do not change with the keyboard layout.  Note how
// many of DOOM's own key codes are simply 0x80 plus the scan code: that is
// not a coincidence, it is where they came from.
//
// 0 means "no DOOM key"; those fall through to the virtual key and then the
// character code in I_TranslateKey.
//
static const byte scantokey[128] =
{
    /* 00 */  0,		KEY_ESCAPE,	'1',		'2',
    /* 04 */  '3',		'4',		'5',		'6',
    /* 08 */  '7',		'8',		'9',		'0',
    /* 0c */  KEY_MINUS,	KEY_EQUALS,	KEY_BACKSPACE,	KEY_TAB,
    /* 10 */  'q',		'w',		'e',		'r',
    /* 14 */  't',		'y',		'u',		'i',
    /* 18 */  'o',		'p',		'[',		']',
    /* 1c */  KEY_ENTER,	KEY_RCTRL,	'a',		's',
    /* 20 */  'd',		'f',		'g',		'h',
    /* 24 */  'j',		'k',		'l',		';',
    /* 28 */  '\'',		'`',		KEY_RSHIFT,	'\\',
    /* 2c */  'z',		'x',		'c',		'v',
    /* 30 */  'b',		'n',		'm',		',',
    /* 34 */  '.',		'/',		KEY_RSHIFT,	'*',
    /* 38 */  KEY_RALT,		' ',		0 /*caps*/,	KEY_F1,
    /* 3c */  KEY_F2,		KEY_F3,		KEY_F4,		KEY_F5,
    /* 40 */  KEY_F6,		KEY_F7,		KEY_F8,		KEY_F9,
    /* 44 */  KEY_F10,		KEY_PAUSE,	0 /*scroll*/,	0 /*home*/,
    /* 48 */  KEY_UPARROW,	0 /*pgup*/,	KEY_MINUS,	KEY_LEFTARROW,
    /* 4c */  0 /*kp 5*/,	KEY_RIGHTARROW,	KEY_EQUALS,	0 /*end*/,
    /* 50 */  KEY_DOWNARROW,	0 /*pgdn*/,	0 /*ins*/,	KEY_BACKSPACE,
    /* 54 */  0,		0,		0,		KEY_F11,
    /* 58 */  KEY_F12,		0,		0,		0,
    /* 5c */  0,		0,		0,		0,
    /* 60 */  0,0,0,0, 0,0,0,0,
    /* 68 */  0,0,0,0, 0,0,0,0,
    /* 70 */  0,0,0,0, 0,0,0,0,
    /* 78 */  0,0,0,0, 0,0,0,0
};


//
// I_OS2_KeyForScancode
//
// The same table, for M_MISC.C.
//
// A DEFAULT.CFG written by the DOS setup program stores its key bindings as
// raw PC scan codes, because that is what the DOS keyboard handler dealt in.
// This port's keys are DOOM's own codes, so such a file binds turn-right to
// scan code 77 -- which this build reads as the letter M.  The config loader
// puts those back through here.  Returns 0 for a scan code with no DOOM key.
//
int I_OS2_KeyForScancode (int scan)
{
    if (scan < 0 || scan > 127)
	return 0;

    return scantokey[scan];
}


//
// I_TranslateKey
//
// Scan code first, virtual key second, character last.
//
// The virtual key step is not redundant.  The arrow keys and the grey
// navigation block send an 0xe0 prefix on the wire, and how much of that
// survives into WM_CHAR's scan code byte has varied between OS/2 keyboard
// drivers.  When the scan code arrives unusable, the virtual key still says
// exactly which key it was.
//
static int I_TranslateKey (USHORT fsflags, USHORT usch, USHORT usvk,
			   UCHAR scan)
{
    int		rc = 0;

    if ((fsflags & KC_SCANCODE) && scan < 128)
	rc = scantokey[scan];

    if (!rc && (fsflags & KC_VIRTUALKEY))
    {
	switch (usvk)
	{
	  case VK_LEFT:		rc = KEY_LEFTARROW;	break;
	  case VK_RIGHT:	rc = KEY_RIGHTARROW;	break;
	  case VK_UP:		rc = KEY_UPARROW;	break;
	  case VK_DOWN:		rc = KEY_DOWNARROW;	break;
	  case VK_ESC:		rc = KEY_ESCAPE;	break;
	  case VK_ENTER:
	  case VK_NEWLINE:	rc = KEY_ENTER;		break;
	  case VK_TAB:		rc = KEY_TAB;		break;
	  case VK_SPACE:	rc = ' ';		break;
	  case VK_BACKSPACE:
	  case VK_DELETE:	rc = KEY_BACKSPACE;	break;
	  case VK_PAUSE:	rc = KEY_PAUSE;		break;
	  case VK_SHIFT:	rc = KEY_RSHIFT;	break;
	  case VK_CTRL:		rc = KEY_RCTRL;		break;
	  case VK_ALT:
	  case VK_ALTGRAF:	rc = KEY_RALT;		break;
	  case VK_F1:		rc = KEY_F1;		break;
	  case VK_F2:		rc = KEY_F2;		break;
	  case VK_F3:		rc = KEY_F3;		break;
	  case VK_F4:		rc = KEY_F4;		break;
	  case VK_F5:		rc = KEY_F5;		break;
	  case VK_F6:		rc = KEY_F6;		break;
	  case VK_F7:		rc = KEY_F7;		break;
	  case VK_F8:		rc = KEY_F8;		break;
	  case VK_F9:		rc = KEY_F9;		break;
	  case VK_F10:		rc = KEY_F10;		break;
	  case VK_F11:		rc = KEY_F11;		break;
	  case VK_F12:		rc = KEY_F12;		break;
	  default:					break;
	}
    }

    // Last resort: whatever character PM made of it.  DOOM compares menu and
    // chat input against lower case.
    if (!rc && (fsflags & KC_CHAR) && usch > 0 && usch < 128)
    {
	rc = usch;
	if (rc >= 'A' && rc <= 'Z')
	    rc = rc - 'A' + 'a';
    }

    return rc;
}


//
// Mouse grabbing.
//
// The pointer is parked in the middle of the client area and hidden; every
// motion event is read as a displacement from that centre and the pointer is
// put back.  This is the same trick the X11 version played with XWarpPointer,
// and it is what turns an absolute pointing device into the relative one DOOM
// wants.  No capture is taken, so Alt-Tab and Ctrl-Esc still work normally.
//
static void I_CentrePointer (void)
{
    POINTL	pt;

    pt.x = client_cx / 2;
    pt.y = client_cy / 2;
    WinMapWindowPoints (os2_hwndClient, HWND_DESKTOP, &pt, 1);
    WinSetPointerPos (HWND_DESKTOP, pt.x, pt.y);
}

//
// I_WantGrab
//
// Whether the pointer should be held captive right now.
//
// The menu test is what gives the player a way out.  Grabbing the pointer
// hides it and pins it to the middle of the window, and without some
// reliable way to get it back the only escape is to guess that Alt-Tab still
// works.  Pressing Escape brings up the menu and the pointer comes straight
// back, which is the behaviour anyone would try first.
//
static boolean I_WantGrab (void)
{
    return (grabMouse && usemouse && windowActive && !menuactive)
	   ? true : false;
}


static void I_SetGrab (boolean on)
{
    if (on == mouseGrabbed)
	return;

    mouseGrabbed = on;

    if (on)
    {
	I_CentrePointer ();
	if (!pointerHidden)
	{
	    WinShowPointer (HWND_DESKTOP, FALSE);
	    pointerHidden = true;
	}
    }
    else if (pointerHidden)
    {
	// Always put the pointer back.  Leaving the desktop without one
	// because DOOM exited badly is unforgivable.
	WinShowPointer (HWND_DESKTOP, TRUE);
	pointerHidden = false;
    }
}


//
// I_OS2_ExceptionHandler
//
// See os2doom.h.  Put the pointer back, then get out of the way.
//
ULONG APIENTRY I_OS2_ExceptionHandler (PEXCEPTIONREPORTRECORD	 report,
				       PEXCEPTIONREGISTRATIONRECORD reg,
				       PCONTEXTRECORD		 ctx,
				       PVOID			 dummy)
{
    // UNUSED.
    reg = 0;
    ctx = 0;
    dummy = 0;

    // Unwinding is not a fault -- it is the tidying up afterwards, and it
    // runs this handler again on the way past.  Only act on the real thing.
    if (report
	&& !(report->fHandlerFlags & (EH_UNWINDING | EH_EXIT_UNWIND
				      | EH_NESTED_CALL)))
    {
	if (pointerHidden)
	{
	    WinShowPointer (HWND_DESKTOP, TRUE);
	    pointerHidden = false;
	}
    }

    // Never pretend to have handled it.  OS/2 goes on to do whatever it
    // would have done -- popup, dump, terminate -- exactly as before.
    return XCPT_CONTINUE_SEARCH;
}


//
// I_ReleaseAllKeys
//
// Post a key-up for everything still held.  Called when the window loses the
// focus, so that a key held at that moment does not stay held for ever.
//
static void I_ReleaseAllKeys (void)
{
    event_t	event;
    int		i;

    for (i = 0; i < 256; i++)
    {
	if (keyIsDown[i])
	{
	    keyIsDown[i] = 0;
	    event.type  = ev_keyup;
	    event.data1 = i;
	    event.data2 = event.data3 = 0;
	    D_PostEvent (&event);
	}
    }

    mouseButtons = 0;
}


//
// I_OS2_WindowActivated
//
// The window has come to the front, or gone from it.
//
// Presentation Manager sends WM_ACTIVATE to the frame window rather than to
// the client, which is why this is a function and not simply a case in the
// client's window procedure: the frame's subclass is the only place that
// sees it.  Two things hang on knowing: the pointer must not stay captured
// by a window the player has switched away from, and a key that was held
// down at the moment of switching must be let go, or the player comes back
// to find themselves still walking forward.
//
static void I_OS2_WindowActivated (boolean active)
{
    windowActive = active;

    if (active)
	I_SetGrab (I_WantGrab ());
    else
    {
	I_SetGrab (false);
	I_ReleaseAllKeys ();
    }
}


//
// SetupBlitter
//
// Tells DIVE where the window is on the screen, how big it is, and which
// parts of it are actually visible.  Every one of those can change without
// the game doing anything, which is why this is called again from WM_SIZE,
// WM_MOVE and -- the one that matters -- WM_VRNENABLED, the message PM sends
// when the visible region of the window changes because something moved in
// front of it.
//
static void SetupBlitter (void)
{
    SETUP_BLITTER	sb;
    SWP			swp;
    POINTL		pt;
    HPS			hps;
    HRGN		hrgn;
    RGNRECT		ctl;
    static RECTL	rcls[64];

    diveBlitterOK = false;

    if (!useDive || os2_hwndClient == NULLHANDLE)
	return;

    if (!WinQueryWindowPos (os2_hwndClient, &swp))
	return;
    if ((swp.fl & SWP_MINIMIZE) || swp.cx <= 0 || swp.cy <= 0)
	return;

    // Where the client area's bottom left corner sits on the desktop.  DIVE
    // blits to the screen, not to the window, so it has to be told.
    pt.x = 0;
    pt.y = 0;
    WinMapWindowPoints (os2_hwndClient, HWND_DESKTOP, &pt, 1);

    hps = WinGetPS (os2_hwndClient);
    if (hps == NULLHANDLE)
	return;

    hrgn = GpiCreateRegion (hps, 0, NULL);
    if (hrgn == NULLHANDLE)
    {
	WinReleasePS (hps);
	return;
    }

    WinQueryVisibleRegion (os2_hwndClient, hrgn);

    ctl.ircStart	= 1;
    ctl.crc		= sizeof(rcls) / sizeof(rcls[0]);
    ctl.crcReturned	= 0;
    ctl.ulDirection	= RECTDIR_LFRT_TOPBOT;

    if (!GpiQueryRegionRects (hps, hrgn, NULL, &ctl, rcls))
	ctl.crcReturned = 0;

    GpiDestroyRegion (hps, hrgn);
    WinReleasePS (hps);

    // Nothing of the window is visible.  Not an error -- it happens every
    // time the window is fully covered -- but there is nothing to blit to,
    // and asking DIVE to blit anyway is how you paint over other people's
    // windows.
    if (ctl.crcReturned == 0)
	return;

    memset (&sb, 0, sizeof(sb));
    sb.ulStructLen		= sizeof(sb);

    // FALSE: our source image is stored top-down, first byte is the top left
    // pixel, which is how DOOM has always held it.  If the picture ever comes
    // out upside down on some display driver, this is the flag to turn over.
    sb.fInvert			= FALSE;

    sb.fccSrcColorFormat	= FOURCC_LUT8;	// 8-bit palettised
    sb.ulSrcWidth		= SCREENWIDTH;
    sb.ulSrcHeight		= SCREENHEIGHT;
    sb.ulSrcPosX		= 0;
    sb.ulSrcPosY		= 0;
    sb.ulDitherType		= 0;

    sb.fccDstColorFormat	= FOURCC_SCRN;	// whatever the screen is
    sb.ulDstWidth		= swp.cx;	// stretch to fill the window
    sb.ulDstHeight		= swp.cy;
    sb.lDstPosX			= 0;
    sb.lDstPosY			= 0;

    sb.lScreenPosX		= pt.x;
    sb.lScreenPosY		= pt.y;
    sb.ulNumDstRects		= ctl.crcReturned;
    sb.pVisDstRects		= rcls;

    diveBlitterOK = (pDiveSetupBlitter (hDive, &sb) == DIVE_SUCCESS);
}


//
// GpiUpdate
//
// The fallback blit.  Flip the frame buffer into bottom-up order, then let
// the graphics engine stretch it into the client area.
//
static void GpiUpdate (void)
{
    HPS		hps;
    POINTL	pts[4];
    int		y;

    if (os2_hwndClient == NULLHANDLE || !gpibuf)
	return;
    if (client_cx <= 0 || client_cy <= 0)
	return;

    for (y = 0; y < SCREENHEIGHT; y++)
	memcpy (gpibuf + y*SCREENWIDTH,
		blitbuf + (SCREENHEIGHT-1-y)*SCREENWIDTH,
		SCREENWIDTH);

    hps = WinGetPS (os2_hwndClient);
    if (hps == NULLHANDLE)
	return;

    // Target rectangle, then source rectangle; GPI's origin is bottom left
    // for both, and the far corner is exclusive.
    pts[0].x = 0;		pts[0].y = 0;
    pts[1].x = client_cx;	pts[1].y = client_cy;
    pts[2].x = 0;		pts[2].y = 0;
    pts[3].x = SCREENWIDTH;	pts[3].y = SCREENHEIGHT;

    GpiDrawBits (hps, (PVOID)gpibuf, (PBITMAPINFO2)&gpibmi,
		 4, pts, ROP_SRCCOPY, BBO_IGNORE);

    WinReleasePS (hps);
}


//
// The window procedure.
//
static MRESULT EXPENTRY DoomWndProcInner (HWND hwnd, ULONG msg,
					  MPARAM mp1, MPARAM mp2)
{
    event_t	event;

    switch (msg)
    {
      case WM_CREATE:
	return (MRESULT)FALSE;

      //
      // Keyboard.
      //
      case WM_CHAR:
      {
	USHORT	fsflags = SHORT1FROMMP(mp1);
	UCHAR	scan    = CHAR4FROMMP(mp1);
	USHORT	usch    = SHORT1FROMMP(mp2);
	USHORT	usvk    = SHORT2FROMMP(mp2);
	int	key;

	// -keydebug: what PM said, and what it became.  If this prints
	// nothing while keys are being pressed, the keystrokes are not
	// reaching this window at all and no amount of translation will
	// help -- go and look at the focus.
	if (keydebug)
	    printf ("WM_CHAR flags=%04x scan=%02x vk=%02x ch=%02x -> key=%02x\n",
		    (unsigned)fsflags, (unsigned)scan, (unsigned)usvk,
		    (unsigned)usch,
		    (unsigned)I_TranslateKey (fsflags, usch, usvk, scan));

	// A key held down repeats.  DOOM wants one keydown per press for the
	// menus, and the game itself only cares whether a key is down at all,
	// so repeats are dropped.
	if (!(fsflags & KC_KEYUP) && (fsflags & KC_PREVDOWN))
	    return (MRESULT)TRUE;

	key = I_TranslateKey (fsflags, usch, usvk, scan);
	if (!key)
	    break;

	event.type  = (fsflags & KC_KEYUP) ? ev_keyup : ev_keydown;
	event.data1 = key;
	event.data2 = event.data3 = 0;

	keyIsDown[key & 0xff] = (fsflags & KC_KEYUP) ? 0 : 1;

	D_PostEvent (&event);
	return (MRESULT)TRUE;
      }

      //
      // Mouse.
      //
      // DOOM's button mask is bit 0 left, bit 1 middle, bit 2 right -- the
      // order X11 reported them in, and the order the default bindings in
      // the configuration file assume.  PM numbers its buttons 1 left,
      // 2 right, 3 middle, so the middle and right bits cross over here.
      //
      case WM_BUTTON1DOWN:
      case WM_BUTTON2DOWN:
      case WM_BUTTON3DOWN:
      case WM_BUTTON1UP:
      case WM_BUTTON2UP:
      case WM_BUTTON3UP:
      {
	int	bit  = (msg == WM_BUTTON1DOWN || msg == WM_BUTTON1UP) ? 1
		     : (msg == WM_BUTTON3DOWN || msg == WM_BUTTON3UP) ? 2
		     : 4;
	boolean	down = (msg == WM_BUTTON1DOWN ||
			msg == WM_BUTTON2DOWN ||
			msg == WM_BUTTON3DOWN);

	// Clicking in the window is how the player asks for the keyboard --
	// and, when the mouse is in use, the pointer -- back.
	//
	// This must happen BEFORE use_mouse is consulted.  Mouse messages
	// arrive at whatever window the pointer is over, but keystrokes only
	// go to the window that holds the focus, so a player using the
	// keyboard alone still has to click on the window once to start
	// playing.  Dropping that click here because the mouse is switched
	// off is what left the keyboard dead: the one thing that ever gave
	// this window the focus was behind the very setting that says the
	// player intends to use the keyboard for everything.
	if (down)
	    WinSetFocus (HWND_DESKTOP, os2_hwndClient);

	// use_mouse off means the mouse does not drive the game at all, so
	// the buttons are not the game's business either -- a player who
	// turned the mouse off would otherwise still be firing with it.
	if (!usemouse)
	    break;

	if (down)
	{
	    mouseButtons |= bit;
	    I_SetGrab (I_WantGrab ());
	}
	else
	    mouseButtons &= ~bit;

	event.type  = ev_mouse;
	event.data1 = mouseButtons;
	event.data2 = event.data3 = 0;
	D_PostEvent (&event);
	return (MRESULT)TRUE;
      }

      case WM_MOUSEMOVE:
	if (mouseGrabbed)
	{
	    int	x  = (SHORT)SHORT1FROMMP(mp1);
	    int	y  = (SHORT)SHORT2FROMMP(mp1);
	    int	dx = x - (int)(client_cx / 2);
	    int	dy = y - (int)(client_cy / 2);

	    // The move we caused ourselves by re-centring: ignore it, or the
	    // view drifts by whatever rounding the centring introduced.
	    if (dx || dy)
	    {
		event.type  = ev_mouse;
		event.data1 = mouseButtons;

		// PM's y grows upward, which is the direction DOOM wants for
		// looking up -- no sign flip here, unlike the X11 version.
		event.data2 = dx << 2;
		event.data3 = dy << 2;
		D_PostEvent (&event);

		I_CentrePointer ();
	    }

	    return (MRESULT)TRUE;	// no pointer to set: it is hidden
	}
	break;

      //
      // Focus and size.
      //
      // WM_ACTIVATE is delivered to the FRAME, not to the client, so in
      // practice this case never runs -- the frame's subclass calls
      // I_OS2_WindowActivated directly.  It is kept because a client window
      // is entitled to the message and being handed it twice does no harm.
      //
      case WM_ACTIVATE:
	I_OS2_WindowActivated (SHORT1FROMMP(mp1) ? true : false);
	break;

      case WM_SIZE:
	client_cx = SHORT1FROMMP(mp2);
	client_cy = SHORT2FROMMP(mp2);
	SetupBlitter ();
	RememberWindowPos ();
	if (mouseGrabbed)
	    I_CentrePointer ();
	break;

      case WM_MOVE:
	SetupBlitter ();
	RememberWindowPos ();
	break;

      //
      // Visible region notification.  This is DIVE's lifeline: PM sends
      // WM_VRNDISABLED before the visible region changes and WM_VRNENABLED
      // after, and blitting to the screen between the two would paint over
      // whatever is now in front of us.
      //
      case WM_VRNDISABLED:
	diveBlitterOK = false;
	break;

      case WM_VRNENABLED:
	SetupBlitter ();
	break;

      case WM_PAINT:
      {
	// DIVE repaints from the next I_FinishUpdate, a thirty-fifth of a
	// second away, so this only has to validate the region.  The GPI
	// path has to actually draw.
	RECTL	rcl;
	HPS	hps = WinBeginPaint (hwnd, NULLHANDLE, &rcl);
	WinEndPaint (hps);

	if (!useDive)
	    GpiUpdate ();
	else
	    SetupBlitter ();
	return 0;
      }

      case WM_ERASEBACKGROUND:
	// Every pixel of the client area is overwritten every frame; letting
	// PM erase it first only makes the picture flicker.
	return (MRESULT)FALSE;

      //
      // MMPM/2 has finished playing something.
      //
      // Two devices report here: the music sequencer, and -- on a card with
      // no DART, being fed one playlist pass at a time -- the waveaudio
      // device.  Nothing distinguishes the messages except the device id they
      // carry, which is the low half of mp2.  The playlist is offered it
      // first and takes it only if it is genuinely its own.
      //
      // The value is spelled out rather than pulled in from <os2me.h>: that
      // header brings os2medef.h with it, which declares a type called
      // VERSION and collides with doomdef.h's enumerator of the same name.
      // I_SOUND.C and I_OS2MUS.C have to deal with that; there is no reason
      // for this file to as well, for one constant.
      case 0x0500:				// MM_MCINOTIFY
	if (!I_OS2_PlaylistPlayDone ((ULONG)SHORT1FROMMP(mp2)))
	    I_OS2_MusicNotify (SHORT1FROMMP(mp1));
	return 0;

      //
      // A sound block has been played, on a machine whose audio driver has no
      // DART and is being fed by a playlist instead.  Spelled out for the
      // same reason as MM_MCINOTIFY above.
      //
      case 0x0504:				// MM_MCIPLAYLISTMESSAGE
	I_OS2_PlaylistNotify ((ULONG)LONGFROMMP(mp2));
	return 0;

      case WM_CLOSE:
	//
	// Quitting cannot be done from here.
	//
	// I_Quit ends in I_ShutdownGraphics, which destroys this window and
	// then the message queue -- and this code is running inside a message
	// dispatched from that very queue.  Pulling both out from under the
	// dispatch leaves the process wedged: it never reaches its exit, so
	// the session it was started from never closes either.
	//
	// So the click is only recorded.  The pump acts on it once the
	// dispatch has unwound and the stack is the game's own again.
	//
	I_SetGrab (false);
	os2_quitRequested = true;
	return 0;

      default:
	break;
    }

    return WinDefWindowProc (hwnd, msg, mp1, mp2);
}


//
// The window procedure proper: nothing but a marker around the real one.
//
// I_Error can be reached from inside a dispatched message -- a DIVE failure
// during a repaint, say -- and it shuts the graphics down on its way out.
// Destroying this window, or the queue this message came from, while that
// message is still being dispatched is what wedges the process.  The flag
// lets I_ShutdownGraphics see that it is in that position and leave those
// two steps to the exit, which is moments away in any case.
//
static MRESULT EXPENTRY DoomWndProc (HWND hwnd, ULONG msg,
				     MPARAM mp1, MPARAM mp2)
{
    MRESULT	mr;
    boolean	wasIn = inWndProc;

    inWndProc = true;
    mr = DoomWndProcInner (hwnd, msg, mp1, mp2);
    inWndProc = wasIn;

    return mr;
}


//
// I_OS2_PumpMessages
//
void I_OS2_PumpMessages (void)
{
    QMSG	qmsg;

    if (os2_hab == NULLHANDLE)
	return;

    while (WinPeekMsg (os2_hab, &qmsg, NULLHANDLE, 0, 0, PM_REMOVE))
	WinDispatchMsg (os2_hab, &qmsg);

    // The close box, acted on now that the dispatch has unwound.  I_Quit
    // does not come back.
    if (os2_quitRequested)
    {
	os2_quitRequested = false;
	I_Quit ();
    }
}


//
// I_StartFrame
//
void I_StartFrame (void)
{
    // er?
}


//
// I_StartTic
//
void I_StartTic (void)
{
    I_OS2_PumpMessages ();

    // Re-decide the grab every frame rather than only when a message
    // happens to arrive.  menuactive changes inside the game, not inside the
    // window procedure, so this is the only place that notices the player
    // pressing Escape and gives the pointer back.
    I_SetGrab (I_WantGrab ());
}


//
// I_UpdateNoBlit
//
void I_UpdateNoBlit (void)
{
    // what is this?
}


//
// I_FinishUpdate
//
void I_FinishUpdate (void)
{
    static int	lasttic;
    int		tics;
    int		i;

    // draws little dots on the bottom of the screen
    if (devparm)
    {
	i = I_GetTime();
	tics = i - lasttic;
	lasttic = i;
	if (tics > 20) tics = 20;

	for (i=0 ; i<tics*2 ; i+=2)
	    screens[0][ (SCREENHEIGHT-1)*SCREENWIDTH + i] = 0xff;
	for ( ; i<20*2 ; i+=2)
	    screens[0][ (SCREENHEIGHT-1)*SCREENWIDTH + i] = 0x0;
    }

    // Keep the window answering while the game is busy.  Doing this here as
    // well as in I_StartTic matters when a single tic takes a long time --
    // loading a level, say -- because it is the only thing between DOOM and
    // the desktop deciding the window has stopped responding.
    I_OS2_PumpMessages ();

    // Nobody is looking: stop burning a processor on it.
    //
    // DOOM renders as fast as the machine will let it, so a window that has
    // been Alt-Tabbed away from goes on consuming everything the scheduler
    // will give it.  Sleeping here caps the loop at about twenty frames a
    // second and hands the time back to whatever the user actually switched
    // to.
    //
    // The game clock is deliberately left alone.  I_GetTime keeps running,
    // so the tics still pass at the right rate and a network game stays in
    // step -- which is also why a network game is never slowed at all.
    if (!windowActive && !netgame)
	DosSleep (50);

    if (useDive)
    {
	if (diveBlitterOK)
	    pDiveBlitImage (hDive, diveBufNum, DIVE_BUFFER_SCREEN);
    }
    else
	GpiUpdate ();
}


//
// I_ReadScreen
//
void I_ReadScreen (byte* scr)
{
    memcpy (scr, screens[0], SCREENWIDTH*SCREENHEIGHT);
}


//
// I_SetPalette
//
// Takes full 8 bit values.  The gamma correction happens here, as it did in
// the X11 version -- the engine hands over the raw PLAYPAL entries and
// expects the platform to apply gammatable[usegamma] on the way to the
// hardware.
//
void I_SetPalette (byte* palette)
{
    int		i;

    for (i = 0; i < 256; i++)
    {
	byte	r = gammatable[usegamma][*palette++];
	byte	g = gammatable[usegamma][*palette++];
	byte	b = gammatable[usegamma][*palette++];

	divepal[i*4+0] = b;
	divepal[i*4+1] = g;
	divepal[i*4+2] = r;
	divepal[i*4+3] = 0;

	gpibmi.argb[i].bBlue  = b;
	gpibmi.argb[i].bGreen = g;
	gpibmi.argb[i].bRed   = r;
	gpibmi.argb[i].fcOptions = 0;
    }

    if (useDive)
	pDiveSetSourcePalette (hDive, 0, 256, divepal);
}


//
// LoadDive
//
// Bring in DIVE.DLL by name.  Everything here is allowed to fail: a machine
// without MMPM/2 has no DIVE.DLL, and that is exactly the machine the GPI
// fallback exists for.
//
static boolean LoadDive (void)
{
    UCHAR	failed[CCHMAXPATH];

    if (DosLoadModule (failed, sizeof(failed), (PSZ)"DIVE", &hmodDive)
	!= NO_ERROR)
    {
	hmodDive = NULLHANDLE;
	return false;
    }

    if (DosQueryProcAddr (hmodDive, 0, (PSZ)"DiveOpen",
			  (PFN *)&pDiveOpen) != NO_ERROR ||
	DosQueryProcAddr (hmodDive, 0, (PSZ)"DiveClose",
			  (PFN *)&pDiveClose) != NO_ERROR ||
	DosQueryProcAddr (hmodDive, 0, (PSZ)"DiveSetupBlitter",
			  (PFN *)&pDiveSetupBlitter) != NO_ERROR ||
	DosQueryProcAddr (hmodDive, 0, (PSZ)"DiveBlitImage",
			  (PFN *)&pDiveBlitImage) != NO_ERROR ||
	DosQueryProcAddr (hmodDive, 0, (PSZ)"DiveAllocImageBuffer",
			  (PFN *)&pDiveAllocImageBuffer) != NO_ERROR ||
	DosQueryProcAddr (hmodDive, 0, (PSZ)"DiveFreeImageBuffer",
			  (PFN *)&pDiveFreeImageBuffer) != NO_ERROR ||
	DosQueryProcAddr (hmodDive, 0, (PSZ)"DiveSetSourcePalette",
			  (PFN *)&pDiveSetSourcePalette) != NO_ERROR)
    {
	DosFreeModule (hmodDive);
	hmodDive = NULLHANDLE;
	return false;
    }

    return true;
}


//
// FrameWndProc
//
// The frame's own window procedure, with one message intercepted.
//
// WM_ADJUSTWINDOWPOS arrives while a border is being dragged, carrying the
// size PM is about to apply.  Correcting it here is what holds the picture
// to 4:3 while the window is resized.  Note that it changes nothing about
// the blitter, which goes on filling whatever the client area turns out to
// be -- which is exactly why this is the safe place to do it.
//
static MRESULT EXPENTRY FrameWndProc (HWND hwnd, ULONG msg,
				      MPARAM mp1, MPARAM mp2)
{
    // Activation is a frame message; the client is never sent it.
    if (msg == WM_ACTIVATE)
	I_OS2_WindowActivated (SHORT1FROMMP(mp1) ? true : false);

    if (msg == WM_ADJUSTWINDOWPOS && !stretchToWindow)
    {
	PSWP	pswp = (PSWP)PVOIDFROMMP(mp1);

	if (pswp
	    && (pswp->fl & SWP_SIZE)
	    && !(pswp->fl & (SWP_MINIMIZE | SWP_MAXIMIZE | SWP_RESTORE)))
	{
	    RECTL	rcl;
	    LONG	chrome_cx, chrome_cy;
	    LONG	client_w;

	    // How much of the frame is title bar and border: ask what frame
	    // would be needed to hold a client area of nothing at all.
	    rcl.xLeft = rcl.yBottom = rcl.xRight = rcl.yTop = 0;
	    WinCalcFrameRect (hwnd, &rcl, FALSE);
	    chrome_cx = rcl.xRight - rcl.xLeft;
	    chrome_cy = rcl.yTop   - rcl.yBottom;

	    client_w = pswp->cx - chrome_cx;

	    // Height follows width.  Dragging a corner then feels like
	    // setting how wide the picture is, which is the dimension people
	    // reach for.
	    if (client_w > 0)
		pswp->cy = client_w * 3 / 4 + chrome_cy;
	}
    }

    return pfnFrameProc (hwnd, msg, mp1, mp2);
}


//
// I_InitGraphics
//
void I_InitGraphics (void)
{
    static boolean	firsttime = true;
    ULONG		flFrame;
    RECTL		rcl;
    PVOID		mem;
    LONG		scr_cx, scr_cy;
    LONG		win_cx, win_cy;
    LONG		win_x, win_y;
    LONG		frame_cx, frame_cy;
    boolean		explicitsize = false;

    if (!firsttime)
	return;
    firsttime = false;

    // -2 and -3 no longer pick a pixel doubling routine -- the blitter
    // scales -- but they still say how big the window should open.
    if (M_CheckParm ("-2"))
    {
	multiply = 2;
	explicitsize = true;
    }
    if (M_CheckParm ("-3"))
    {
	multiply = 3;
	explicitsize = true;
    }

    if (M_CheckParm ("-nograbmouse"))
	grabMouse = false;

    if (M_CheckParm ("-stretch"))
	stretchToWindow = true;

    if (M_CheckParm ("-keydebug"))
	keydebug = true;

    //
    // The window opens 4:3, not 320x200.
    //
    // DOOM's picture is 320x200, but it was drawn to be seen on a 4:3
    // screen, where the pixels are half again as tall as they are wide.
    // Shown at 320x200 on square pixels everything comes out a fifth too
    // short -- most obvious on the faces in the status bar and on anything
    // round.  320x240 is the same picture in the proportions it was drawn
    // for, and the blitter stretches it there for nothing.
    //
    // -stretch gives the old behaviour back and lets the window be any
    // shape at all.
    //
    win_cx = SCREENWIDTH * multiply;
    win_cy = stretchToWindow ? SCREENHEIGHT * multiply
			     : SCREENWIDTH * multiply * 3 / 4;

    //
    // Become a Presentation Manager process and open the window.
    //
    // Usually already done: the loading window needs Presentation Manager
    // long before this, and I_OS2_InitPM only ever does the work once.
    if (!I_OS2_InitPM ())
	I_Error ("I_InitGraphics: could not become a Presentation Manager\n"
		 "process.  Start DOOM from an OS/2 window rather than a\n"
		 "full screen session.");

    if (!WinRegisterClass (os2_hab, (PSZ)DOOM_WINDOW_CLASS, DoomWndProc,
			   CS_SIZEREDRAW | CS_MOVENOTIFY, 0))
	I_Error ("I_InitGraphics: WinRegisterClass failed.");

    flFrame = FCF_TITLEBAR | FCF_SYSMENU | FCF_MINBUTTON | FCF_MAXBUTTON
	    | FCF_SIZEBORDER | FCF_TASKLIST;

    os2_hwndFrame = WinCreateStdWindow (HWND_DESKTOP,
					0,		// not visible yet
					&flFrame,
					(PSZ)DOOM_WINDOW_CLASS,
					(PSZ)"DOOM",
					0,
					NULLHANDLE,	// resources: none
					0,
					&os2_hwndClient);
    if (os2_hwndFrame == NULLHANDLE)
	I_Error ("I_InitGraphics: WinCreateStdWindow failed.");

    // Take over the frame's messages, for the 4:3 constraint above.
    pfnFrameProc = WinSubclassWindow (os2_hwndFrame, FrameWndProc);

    //
    // The window icon, if this DOOM.EXE was built with one.
    //
    // Loaded and applied by hand rather than asked for with FCF_ICON in the
    // frame flags.  PM validates an icon resource strictly, and if it does
    // not like it, FCF_ICON fails -- and that fails the whole
    // WinCreateStdWindow, with PMERR_INVALID_RESOURCE_FORMAT and no window
    // at all.  Done this way the worst a bad or missing icon can do is leave
    // the default one in place, which is what happens when no resource was
    // bound in.
    //
    {
	HPOINTER	hptr = WinLoadPointer (HWND_DESKTOP, NULLHANDLE,
					       ID_DOOM_ICON);
	if (hptr != NULLHANDLE)
	    WinSendMsg (os2_hwndFrame, WM_SETICON,
			MPFROMLONG(hptr), (MPARAM)0);
    }

    // Size the frame so that the *client* comes out at the size asked for.
    // Sizing the frame directly would lose the title bar and border out of
    // the picture, and the image would be squashed by however many pixels
    // the user's border happens to be.
    rcl.xLeft   = 0;
    rcl.yBottom = 0;
    rcl.xRight  = win_cx;
    rcl.yTop    = win_cy;
    WinCalcFrameRect (os2_hwndFrame, &rcl, FALSE);

    frame_cx = rcl.xRight - rcl.xLeft;
    frame_cy = rcl.yTop   - rcl.yBottom;

    scr_cx = WinQuerySysValue (HWND_DESKTOP, SV_CXSCREEN);
    scr_cy = WinQuerySysValue (HWND_DESKTOP, SV_CYSCREEN);

    win_x = (scr_cx - frame_cx) / 2;
    win_y = (scr_cy - frame_cy) / 2;

    //
    // Where it was last time, if it has been here before.
    //
    // Clamped to the screen, because a position saved on a 1024x768 desktop
    // would otherwise put the window somewhere unreachable when the same
    // DEFAULT.CFG is used at 640x480 -- and a window whose title bar is off
    // the screen cannot be dragged back.
    //
    // ...unless the size was asked for on the command line.  A saved
    // geometry that silently overrode -2 and -3 would make them look broken:
    // they would work once, and then never again once the window had been
    // saved at some other size.
    if (!explicitsize && os2_window_w > 0 && os2_window_h > 0)
    {
	frame_cx = os2_window_w;
	frame_cy = os2_window_h;
	win_x    = os2_window_x;
	win_y    = os2_window_y;

	if (frame_cx > scr_cx)	frame_cx = scr_cx;
	if (frame_cy > scr_cy)	frame_cy = scr_cy;

	if (win_x + frame_cx > scr_cx)	win_x = scr_cx - frame_cx;
	if (win_y + frame_cy > scr_cy)	win_y = scr_cy - frame_cy;
	if (win_x < 0)			win_x = 0;
	if (win_y < 0)			win_y = 0;
    }

    WinSetWindowPos (os2_hwndFrame, HWND_TOP,
		     win_x, win_y,
		     frame_cx, frame_cy,
		     SWP_SIZE | SWP_MOVE | SWP_ACTIVATE | SWP_SHOW);

    //
    // And say so twice.
    //
    // SWP_ACTIVATE above raises the window, but this process began life as a
    // text mode program and morphed into a Presentation Manager one on the
    // way here (see I_OS2_MorphToPM).  The session it was started from is
    // still there, still holding a window of its own, and the keyboard
    // follows whichever of the two the desktop thinks is in front.  Asking
    // outright settles it: the frame becomes the active window, and the
    // client -- which is what the window procedure below belongs to -- takes
    // the input focus.  Without the focus, mouse messages still arrive,
    // because those go to whatever the pointer is over, but not one
    // keystroke does.
    //
    WinSetActiveWindow (HWND_DESKTOP, os2_hwndFrame);
    WinSetFocus (HWND_DESKTOP, os2_hwndClient);

    if (keydebug)
	printf ("I_InitGraphics: focus is %s the game window.\n",
		(WinQueryFocus (HWND_DESKTOP) == os2_hwndClient)
		    ? "on" : "NOT on");

    client_cx = win_cx;
    client_cy = win_cy;

    //
    // The frame buffer.
    //
    // DosAllocMem rather than malloc: DIVE takes the address of this buffer
    // and hands it to the display driver, which wants whole committed pages
    // it can address, not something in the middle of the C run-time heap.
    //
    if (DosAllocMem (&mem, SCREENWIDTH*SCREENHEIGHT,
		     PAG_READ | PAG_WRITE | PAG_COMMIT) != NO_ERROR)
	I_Error ("I_InitGraphics: could not allocate the frame buffer.");

    blitbuf = (byte *)mem;
    memset (blitbuf, 0, SCREENWIDTH*SCREENHEIGHT);

    // Point the engine's first screen at it, so that everything DOOM draws
    // lands in the buffer the blitter reads -- no copy in between.  This is
    // what the X11 version did with the XImage's data, and V_Init's own
    // allocation for screens[0] is simply left behind.
    screens[0] = blitbuf;

    //
    // Try DIVE, fall back to GPI.
    //
    if (!M_CheckParm ("-nodive") && LoadDive ())
    {
	if (pDiveOpen (&hDive, FALSE, NULL) == DIVE_SUCCESS)
	{
	    if (pDiveAllocImageBuffer (hDive, &diveBufNum, FOURCC_LUT8,
				       SCREENWIDTH, SCREENHEIGHT,
				       SCREENWIDTH, blitbuf) == DIVE_SUCCESS)
	    {
		useDive = true;
	    }
	    else
	    {
		pDiveClose (hDive);
		hDive = 0;
	    }
	}
    }

    if (useDive)
    {
	printf ("I_InitGraphics: DIVE, %ix%i stretched to %ix%i.\n",
		SCREENWIDTH, SCREENHEIGHT, (int)win_cx, (int)win_cy);

	// Ask to be told when the visible region changes, and set the
	// blitter up for the first time.
	WinSetVisibleRegionNotify (os2_hwndClient, TRUE);
	SetupBlitter ();
    }
    else
    {
	printf ("I_InitGraphics: DIVE unavailable, using GpiDrawBits.\n");

	gpibuf = (byte *) malloc (SCREENWIDTH*SCREENHEIGHT);
	if (!gpibuf)
	    I_Error ("I_InitGraphics: could not allocate the blit buffer.");
	memset (gpibuf, 0, SCREENWIDTH*SCREENHEIGHT);

	memset (&gpibmi, 0, sizeof(gpibmi));
	gpibmi.hdr.cbFix     = sizeof(BITMAPINFOHEADER2);
	gpibmi.hdr.cx        = SCREENWIDTH;
	gpibmi.hdr.cy        = SCREENHEIGHT;
	gpibmi.hdr.cPlanes   = 1;
	gpibmi.hdr.cBitCount = 8;
    }

    // The game's own window is up and about to be drawn into, so the loading
    // window has nothing left to say.  This is the last thing it sees.
    I_OS2_LoadWindowClose ();

    I_SetGrab (I_WantGrab ());
}


//
// I_ShutdownGraphics
//
void I_ShutdownGraphics (void)
{
    // Before anything else: the pointer must come back, and it must come
    // back even if this is being called from I_Error half way through a
    // failed startup.
    I_SetGrab (false);

    if (useDive)
    {
	if (os2_hwndClient != NULLHANDLE)
	    WinSetVisibleRegionNotify (os2_hwndClient, FALSE);

	pDiveFreeImageBuffer (hDive, diveBufNum);
	pDiveClose (hDive);

	useDive = false;
	diveBlitterOK = false;
	hDive = 0;
    }

    if (hmodDive != NULLHANDLE)
    {
	DosFreeModule (hmodDive);
	hmodDive = NULLHANDLE;
    }

    // Only when this is not being called from inside a dispatched message:
    // see the note on DoomWndProc.  Skipping them costs nothing, because
    // ending the process releases both anyway.
    if (os2_hwndFrame != NULLHANDLE && !inWndProc)
    {
	WinDestroyWindow (os2_hwndFrame);
	os2_hwndFrame  = NULLHANDLE;
	os2_hwndClient = NULLHANDLE;
    }

    //
    // The message queue is deliberately NOT destroyed, any more than the
    // anchor block is.
    //
    // I_Error calls this and then puts its message in a message box, and
    // WinMessageBox needs a message queue on the calling thread just as much
    // as it needs an anchor block -- it has nowhere to dispatch to otherwise.
    // Destroying the queue here left it beeping and returning without ever
    // showing anything, which went unnoticed for as long as the error was
    // also being printed to a session.  With the port linked as a real PM
    // application there is no session, and the box is the only way an error
    // can be reported at all.
    //
    // The process is about to end, which releases both.
    //

    // The anchor block is deliberately *not* released here.  I_Error puts up
    // a message box after calling this, and WinMessageBox needs it.  The
    // process is about to end, which releases it anyway.
}
