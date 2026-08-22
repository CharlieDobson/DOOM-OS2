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
static boolean	mouseGrabbed	= false;	// ...and it is grabbed now
static boolean	pointerHidden	= false;
static int	mouseButtons	= 0;

// Which DOOM keys are currently held, so that they can all be released when
// the window loses the focus.  Without this, Alt-Tabbing away while running
// forward leaves the player running forward for ever.
static byte	keyIsDown[256];


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
static MRESULT EXPENTRY DoomWndProc (HWND hwnd, ULONG msg,
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

	if (down)
	{
	    mouseButtons |= bit;

	    // Clicking in the window is how the player asks for the mouse
	    // back after Alt-Tabbing away.
	    WinSetFocus (HWND_DESKTOP, os2_hwndClient);
	    if (grabMouse)
		I_SetGrab (true);
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
      case WM_ACTIVATE:
	if (SHORT1FROMMP(mp1))
	{
	    if (grabMouse)
		I_SetGrab (true);
	}
	else
	{
	    I_SetGrab (false);
	    I_ReleaseAllKeys ();
	}
	break;

      case WM_SIZE:
	client_cx = SHORT1FROMMP(mp2);
	client_cy = SHORT2FROMMP(mp2);
	SetupBlitter ();
	if (mouseGrabbed)
	    I_CentrePointer ();
	break;

      case WM_MOVE:
	SetupBlitter ();
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

      case WM_CLOSE:
	// Never come back: I_Quit saves the configuration and exits.
	I_SetGrab (false);
	I_Quit ();
	return 0;

      default:
	break;
    }

    return WinDefWindowProc (hwnd, msg, mp1, mp2);
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
    LONG		frame_cx, frame_cy;

    if (!firsttime)
	return;
    firsttime = false;

    // -2 and -3 no longer pick a pixel doubling routine -- the blitter
    // scales -- but they still say how big the window should open.
    if (M_CheckParm ("-2"))
	multiply = 2;
    if (M_CheckParm ("-3"))
	multiply = 3;

    if (M_CheckParm ("-nograbmouse"))
	grabMouse = false;

    win_cx = SCREENWIDTH  * multiply;
    win_cy = SCREENHEIGHT * multiply;

    //
    // Become a Presentation Manager process and open the window.
    //
    if (!I_OS2_MorphToPM ())
	I_Error ("I_InitGraphics: could not become a Presentation Manager\n"
		 "process.  Start DOOM from an OS/2 window rather than a\n"
		 "full screen session.");

    os2_hab = WinInitialize (0);
    if (os2_hab == NULLHANDLE)
	I_Error ("I_InitGraphics: WinInitialize failed.");

    os2_hmq = WinCreateMsgQueue (os2_hab, 0);
    if (os2_hmq == NULLHANDLE)
	I_Error ("I_InitGraphics: WinCreateMsgQueue failed.");

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

    WinSetWindowPos (os2_hwndFrame, HWND_TOP,
		     (scr_cx - frame_cx) / 2,
		     (scr_cy - frame_cy) / 2,
		     frame_cx, frame_cy,
		     SWP_SIZE | SWP_MOVE | SWP_ACTIVATE | SWP_SHOW);

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

    if (grabMouse)
	I_SetGrab (true);
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

    if (os2_hwndFrame != NULLHANDLE)
    {
	WinDestroyWindow (os2_hwndFrame);
	os2_hwndFrame  = NULLHANDLE;
	os2_hwndClient = NULLHANDLE;
    }

    if (os2_hmq != NULLHANDLE)
    {
	WinDestroyMsgQueue (os2_hmq);
	os2_hmq = NULLHANDLE;
    }

    // The anchor block is deliberately *not* released here.  I_Error puts up
    // a message box after calling this, and WinMessageBox needs it.  The
    // process is about to end, which releases it anyway.
}
