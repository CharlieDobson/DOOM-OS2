// Emacs style mode select   -*- C++ -*-
//-----------------------------------------------------------------------------
//
// DESCRIPTION:
//	OS2SETUP.EXE -- the settings program for DOOM for OS/2.
//
//	DOOM's own menus cannot rebind a key.  On DOS that did not matter,
//	because SETUP.EXE did it before the game ever ran; this port had
//	nothing equivalent, so the only way to change a binding was to edit
//	DEFAULT.CFG by hand in DOOM's own key codes.  This is that program.
//
//	Deliberately NOT called SETUP.EXE.  The DOS game's own SETUP.EXE sits
//	in the same directory and still works for the DOS build; overwriting
//	somebody's original files to install a port would be unforgivable.
//
//	It edits DEFAULT.CFG in place and in full: every line that was in the
//	file is written back, in the order it arrived, whether this program
//	understands it or not.  A configuration that came from the DOS setup
//	program keeps its comport and snd_sbport settings -- they mean nothing
//	here, but they mean something to DOOM.EXE on DOS, and the two share a
//	directory.
//
//	Built by BUILD\MKSETUP.CMD.  A pure Presentation Manager program: no
//	DOOM sources, no engine, nothing but the file format.
//
//-----------------------------------------------------------------------------

#define INCL_WIN
#define INCL_GPI
#define INCL_DOSPROCESS
#define INCL_DOSERRORS
#include <os2.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define CLASS_NAME	"DoomSetup"

#define CLIENT_W	470
#define CLIENT_H	400

#define IDL_KEYS	100
#define IDB_CHANGE	101
#define IDB_DEFAULTS	102
#define IDC_MOUSE	103
#define IDE_SENS	104
#define IDR_1X		105
#define IDR_2X		106
#define IDR_3X		107
#define IDB_SAVE	108
#define IDB_CANCEL	109

// DOOM's own key codes, from DOOMDEF.H.  Only the ones bindable to an action.
#define KEY_RIGHTARROW	0xae
#define KEY_LEFTARROW	0xac
#define KEY_UPARROW	0xad
#define KEY_DOWNARROW	0xaf
#define KEY_RCTRL	0x9d
#define KEY_RSHIFT	0xb6
#define KEY_RALT	0xb8
#define KEY_TAB		9
#define KEY_ENTER	13


//
// The actions, in the order they are shown.
//
static const struct
{
    char*	name;			// as it appears in DEFAULT.CFG
    char*	label;			// as it appears to a person
    int		fallback;		// DOOM's own default
}
actions[] =
{
    { "key_up",		"Move forward",		KEY_UPARROW	},
    { "key_down",	"Move backward",	KEY_DOWNARROW	},
    { "key_left",	"Turn left",		KEY_LEFTARROW	},
    { "key_right",	"Turn right",		KEY_RIGHTARROW	},
    { "key_strafeleft",	"Sidestep left",	','		},
    { "key_straferight","Sidestep right",	'.'		},
    { "key_fire",	"Fire",			KEY_RCTRL	},
    { "key_use",	"Open / use",		' '		},
    { "key_strafe",	"Sidestep modifier",	KEY_RALT	},
    { "key_speed",	"Run modifier",		KEY_RSHIFT	}
};

#define NUM_ACTIONS	(int)(sizeof(actions)/sizeof(actions[0]))


//
// DEFAULT.CFG, held as it was read.
//
// Order is preserved and unknown settings are kept untouched, so that saving
// from here never silently discards something -- which is precisely the
// mistake that made the port unable to recognise its own configuration file
// earlier in its life.
//
#define MAX_ENTRIES	128

static struct
{
    char	name[48];
    char	value[160];
}
cfg[MAX_ENTRIES];

static int	cfgCount = 0;
static char	cfgPath[CCHMAXPATH] = "DEFAULT.CFG";

static HWND	hwndClient = NULLHANDLE;
static HWND	hwndFrame  = NULLHANDLE;

// Which action is waiting for a key press, or -1.
static int	capturing = -1;


//
// CfgFind / CfgGet / CfgSet
//
static int CfgFind (char* name)
{
    int		i;

    for (i = 0; i < cfgCount; i++)
	if (!stricmp (cfg[i].name, name))
	    return i;

    return -1;
}

static int CfgGetInt (char* name, int fallback)
{
    int		i = CfgFind (name);

    if (i < 0)
	return fallback;

    return atoi (cfg[i].value);
}

static void CfgSetInt (char* name, int value)
{
    int		i = CfgFind (name);

    if (i < 0)
    {
	if (cfgCount >= MAX_ENTRIES)
	    return;

	i = cfgCount++;
	strcpy (cfg[i].name, name);
    }

    sprintf (cfg[i].value, "%i", value);
}


//
// CfgLoad
//
// DOOM's own format: a name, whitespace, then a value which may be a number
// or a quoted string.  The string case is kept exactly as it was read,
// quotes included, because nothing here needs to look inside one.
//
static void CfgLoad (void)
{
    FILE*	f;
    char	line[256];

    cfgCount = 0;

    f = fopen (cfgPath, "r");
    if (!f)
	return;

    while (cfgCount < MAX_ENTRIES && fgets (line, sizeof(line), f))
    {
	char*	p = line;
	char*	name;
	int	n = 0;

	while (*p == ' ' || *p == '\t')
	    p++;

	name = p;
	while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r')
	    p++;

	if (p == name)
	    continue;			// blank line

	n = (int)(p - name);
	if (n >= (int)sizeof(cfg[0].name))
	    n = sizeof(cfg[0].name) - 1;

	memcpy (cfg[cfgCount].name, name, n);
	cfg[cfgCount].name[n] = 0;

	while (*p == ' ' || *p == '\t')
	    p++;

	// The rest of the line, less the newline.
	n = strlen (p);
	while (n > 0 && (p[n-1] == '\n' || p[n-1] == '\r'))
	    n--;

	if (n <= 0)
	    continue;			// a name with no value: not a setting

	if (n >= (int)sizeof(cfg[0].value))
	    n = sizeof(cfg[0].value) - 1;

	memcpy (cfg[cfgCount].value, p, n);
	cfg[cfgCount].value[n] = 0;

	cfgCount++;
    }

    fclose (f);
}


//
// CfgSave
//
static BOOL CfgSave (void)
{
    FILE*	f;
    int		i;

    f = fopen (cfgPath, "w");
    if (!f)
	return FALSE;

    for (i = 0; i < cfgCount; i++)
	fprintf (f, "%s\t\t%s\n", cfg[i].name, cfg[i].value);

    fclose (f);
    return TRUE;
}


//
// KeyName
//
// A DOOM key code, as something a person can read.
//
static char* KeyName (int key)
{
    static char	buf[32];

    switch (key)
    {
      case KEY_UPARROW:		return "Up Arrow";
      case KEY_DOWNARROW:	return "Down Arrow";
      case KEY_LEFTARROW:	return "Left Arrow";
      case KEY_RIGHTARROW:	return "Right Arrow";
      case KEY_RCTRL:		return "Ctrl";
      case KEY_RSHIFT:		return "Shift";
      case KEY_RALT:		return "Alt";
      case KEY_TAB:		return "Tab";
      case KEY_ENTER:		return "Enter";
      case ' ':			return "Space";
      case ',':			return "Comma";
      case '.':			return "Period";
      case '/':			return "Slash";
      case ';':			return "Semicolon";
      case '\'':		return "Quote";
      case '[':			return "[";
      case ']':			return "]";
      case '\\':		return "Backslash";
      case '`':			return "Backquote";
      case '-':			return "Minus";
      case '=':			return "Equals";
      default:			break;
    }

    if (key >= 0xbb && key <= 0xc4)	// KEY_F1 .. KEY_F10
    {
	sprintf (buf, "F%i", key - 0xbb + 1);
	return buf;
    }

    if (key > 32 && key < 127)
    {
	sprintf (buf, "%c", toupper (key));
	return buf;
    }

    sprintf (buf, "code %02x", key);
    return buf;
}


//
// KeyFromMessage
//
// A WM_CHAR, as a DOOM key code, or 0 for one that cannot be bound.
//
// Only the virtual key and the character are consulted, never the scan code.
// The game translates scan codes because it must cope with a DEFAULT.CFG
// written by the DOS setup program; here there is nothing historical to
// respect, and a virtual key says what a key IS rather than where it sits.
//
static int KeyFromMessage (USHORT fsflags, USHORT usch, USHORT usvk)
{
    if (fsflags & KC_VIRTUALKEY)
    {
	switch (usvk)
	{
	  case VK_LEFT:		return KEY_LEFTARROW;
	  case VK_RIGHT:	return KEY_RIGHTARROW;
	  case VK_UP:		return KEY_UPARROW;
	  case VK_DOWN:		return KEY_DOWNARROW;
	  case VK_CTRL:		return KEY_RCTRL;
	  case VK_SHIFT:	return KEY_RSHIFT;
	  case VK_ALT:
	  case VK_ALTGRAF:	return KEY_RALT;
	  case VK_SPACE:	return ' ';
	  case VK_TAB:		return KEY_TAB;
	  case VK_ENTER:
	  case VK_NEWLINE:	return KEY_ENTER;

	  // Escape is how the player backs out of being asked, so it can
	  // never be a binding.
	  case VK_ESC:		return 0;

	  case VK_F1: case VK_F2: case VK_F3: case VK_F4:
	  case VK_F5: case VK_F6: case VK_F7: case VK_F8:
	  case VK_F9: case VK_F10:
	    // The function keys already do things -- save, load, volume --
	    // and DOOM checks those before it looks at any binding, so one
	    // bound here would simply never be seen.
	    return 0;

	  default:		break;
	}
    }

    if ((fsflags & KC_CHAR) && usch > 32 && usch < 127)
    {
	int	c = usch;

	if (c >= 'A' && c <= 'Z')
	    c = c - 'A' + 'a';

	return c;
    }

    return 0;
}


//
// FillKeyList
//
static void FillKeyList (void)
{
    HWND	hwndList = WinWindowFromID (hwndClient, IDL_KEYS);
    int		sel;
    int		i;

    sel = (LONG)WinSendMsg (hwndList, LM_QUERYSELECTION,
			    MPFROMSHORT (LIT_FIRST), 0);

    WinSendMsg (hwndList, LM_DELETEALL, 0, 0);

    for (i = 0; i < NUM_ACTIONS; i++)
    {
	char	row[80];
	int	key = CfgGetInt (actions[i].name, actions[i].fallback);

	// Padded so the key names line up under each other.
	sprintf (row, "%-22s%s", actions[i].label, KeyName (key));

	WinSendMsg (hwndList, LM_INSERTITEM,
		    MPFROMSHORT (LIT_END), MPFROMP (row));
    }

    if (sel != LIT_NONE && sel >= 0 && sel < NUM_ACTIONS)
	WinSendMsg (hwndList, LM_SELECTITEM, MPFROMSHORT (sel),
		    MPFROMSHORT (TRUE));
}


//
// LoadToControls / ControlsToCfg
//
static void LoadToControls (void)
{
    char	buf[32];
    int		w;
    int		id;

    FillKeyList ();

    WinSendMsg (WinWindowFromID (hwndClient, IDC_MOUSE), BM_SETCHECK,
		MPFROMSHORT (CfgGetInt ("use_mouse", 0) ? 1 : 0), 0);

    sprintf (buf, "%i", CfgGetInt ("mouse_sensitivity", 5));
    WinSetWindowText (WinWindowFromID (hwndClient, IDE_SENS), (PSZ)buf);

    // Window width, as a multiple of 320.  Anything unrecognised -- including
    // a window the player dragged to an odd size -- shows as 1x, and is only
    // written back if they actually choose one.
    w = CfgGetInt ("os2_window_w", 0);

    id = (w >= 960) ? IDR_3X : (w >= 640) ? IDR_2X : IDR_1X;

    WinSendMsg (WinWindowFromID (hwndClient, id), BM_SETCHECK,
		MPFROMSHORT (1), 0);
}


static void ControlsToCfg (void)
{
    char	buf[32];
    int		mult = 1;

    CfgSetInt ("use_mouse",
	       (LONG)WinSendMsg (WinWindowFromID (hwndClient, IDC_MOUSE),
				 BM_QUERYCHECK, 0, 0) ? 1 : 0);

    WinQueryWindowText (WinWindowFromID (hwndClient, IDE_SENS),
			sizeof(buf), (PSZ)buf);
    {
	int	s = atoi (buf);

	if (s < 0)  s = 0;
	if (s > 20) s = 20;

	CfgSetInt ("mouse_sensitivity", s);
    }

    if ((LONG)WinSendMsg (WinWindowFromID (hwndClient, IDR_2X),
			  BM_QUERYCHECK, 0, 0))
	mult = 2;
    else if ((LONG)WinSendMsg (WinWindowFromID (hwndClient, IDR_3X),
			       BM_QUERYCHECK, 0, 0))
	mult = 3;

    // 4:3, matching what the game opens at -- 320x200 is meant to be seen as
    // 320x240, which is why the height is not simply 200 times the multiple.
    CfgSetInt ("os2_window_w", 320 * mult);
    CfgSetInt ("os2_window_h", 240 * mult);

    // Written by us, in DOOM's own key codes: the stamp that stops the game
    // trying to translate these as though they were DOS scan codes.
    CfgSetInt ("os2_cfgversion", 1);
}


//
// StartCapture
//
static void StartCapture (void)
{
    HWND	hwndList = WinWindowFromID (hwndClient, IDL_KEYS);
    int		sel = (LONG)WinSendMsg (hwndList, LM_QUERYSELECTION,
					MPFROMSHORT (LIT_FIRST), 0);
    char	msg[80];

    if (sel == LIT_NONE || sel < 0 || sel >= NUM_ACTIONS)
    {
	WinMessageBox (HWND_DESKTOP, hwndFrame,
		       (PSZ)"Choose an action from the list first.",
		       (PSZ)"DOOM Setup", 0, MB_OK | MB_INFORMATION);
	return;
    }

    capturing = sel;

    sprintf (msg, "Press the key for \"%s\"  (Esc to cancel)",
	     actions[sel].label);
    WinSetWindowText (WinWindowFromID (hwndClient, IDB_CHANGE),
		      (PSZ)"Waiting...");
    WinSetWindowText (hwndFrame, (PSZ)msg);

    // The keystroke has to come here, not to the button that was just
    // clicked.
    WinSetFocus (HWND_DESKTOP, hwndClient);
}


static void EndCapture (void)
{
    capturing = -1;

    WinSetWindowText (WinWindowFromID (hwndClient, IDB_CHANGE),
		      (PSZ)"~Change key...");
    WinSetWindowText (hwndFrame, (PSZ)"DOOM for OS/2 -- Setup");
}


//
// MakeControls
//
// Built in code rather than from a dialog template.  One file, no resource
// compiler, and nothing that can fail to be bound at run time.
//
static void MakeControls (HWND hwnd)
{
    LONG	y;

    WinCreateWindow (hwnd, (PSZ)WC_STATIC, (PSZ)"Controls",
		     WS_VISIBLE | SS_TEXT | DT_LEFT | DT_VCENTER,
		     14, CLIENT_H - 30, 300, 20,
		     hwnd, HWND_TOP, -1, NULL, NULL);

    y = CLIENT_H - 30 - 180;

    WinCreateWindow (hwnd, (PSZ)WC_LISTBOX, NULL,
		     WS_VISIBLE | LS_NOADJUSTPOS,
		     14, y, CLIENT_W - 28, 175,
		     hwnd, HWND_TOP, IDL_KEYS, NULL, NULL);

    y -= 40;

    WinCreateWindow (hwnd, (PSZ)WC_BUTTON, (PSZ)"~Change key...",
		     WS_VISIBLE | BS_PUSHBUTTON,
		     14, y, 130, 30,
		     hwnd, HWND_TOP, IDB_CHANGE, NULL, NULL);

    WinCreateWindow (hwnd, (PSZ)WC_BUTTON, (PSZ)"~Defaults",
		     WS_VISIBLE | BS_PUSHBUTTON,
		     154, y, 110, 30,
		     hwnd, HWND_TOP, IDB_DEFAULTS, NULL, NULL);

    y -= 36;

    WinCreateWindow (hwnd, (PSZ)WC_BUTTON, (PSZ)"Use the ~mouse to play",
		     WS_VISIBLE | BS_AUTOCHECKBOX,
		     14, y, 240, 24,
		     hwnd, HWND_TOP, IDC_MOUSE, NULL, NULL);

    WinCreateWindow (hwnd, (PSZ)WC_STATIC, (PSZ)"Sensitivity:",
		     WS_VISIBLE | SS_TEXT | DT_RIGHT | DT_VCENTER,
		     270, y, 110, 24,
		     hwnd, HWND_TOP, -1, NULL, NULL);

    WinCreateWindow (hwnd, (PSZ)WC_ENTRYFIELD, (PSZ)"5",
		     WS_VISIBLE | ES_MARGIN | ES_LEFT,
		     390, y, 60, 24,
		     hwnd, HWND_TOP, IDE_SENS, NULL, NULL);

    y -= 34;

    WinCreateWindow (hwnd, (PSZ)WC_STATIC, (PSZ)"Window size",
		     WS_VISIBLE | SS_TEXT | DT_LEFT | DT_VCENTER,
		     14, y, 200, 22,
		     hwnd, HWND_TOP, -1, NULL, NULL);

    y -= 28;

    WinCreateWindow (hwnd, (PSZ)WC_BUTTON, (PSZ)"3~20 x 240",
		     WS_VISIBLE | BS_AUTORADIOBUTTON,
		     24, y, 130, 24,
		     hwnd, HWND_TOP, IDR_1X, NULL, NULL);

    WinCreateWindow (hwnd, (PSZ)WC_BUTTON, (PSZ)"6~40 x 480",
		     WS_VISIBLE | BS_AUTORADIOBUTTON,
		     164, y, 130, 24,
		     hwnd, HWND_TOP, IDR_2X, NULL, NULL);

    WinCreateWindow (hwnd, (PSZ)WC_BUTTON, (PSZ)"9~60 x 720",
		     WS_VISIBLE | BS_AUTORADIOBUTTON,
		     304, y, 140, 24,
		     hwnd, HWND_TOP, IDR_3X, NULL, NULL);

    WinCreateWindow (hwnd, (PSZ)WC_STATIC,
		     (PSZ)"The picture is always 320x200; a larger window is "
			  "the same picture, bigger.",
		     WS_VISIBLE | SS_TEXT | DT_LEFT | DT_VCENTER,
		     14, 52, CLIENT_W - 28, 20,
		     hwnd, HWND_TOP, -1, NULL, NULL);

    WinCreateWindow (hwnd, (PSZ)WC_BUTTON, (PSZ)"~Save",
		     WS_VISIBLE | BS_PUSHBUTTON | BS_DEFAULT,
		     CLIENT_W - 220, 12, 100, 32,
		     hwnd, HWND_TOP, IDB_SAVE, NULL, NULL);

    WinCreateWindow (hwnd, (PSZ)WC_BUTTON, (PSZ)"C~ancel",
		     WS_VISIBLE | BS_PUSHBUTTON,
		     CLIENT_W - 110, 12, 100, 32,
		     hwnd, HWND_TOP, IDB_CANCEL, NULL, NULL);
}


//
// SetupWndProc
//
static MRESULT EXPENTRY SetupWndProc (HWND hwnd, ULONG msg,
				      MPARAM mp1, MPARAM mp2)
{
    switch (msg)
    {
      case WM_CREATE:
	hwndClient = hwnd;
	MakeControls (hwnd);
	return (MRESULT)FALSE;

      case WM_ERASEBACKGROUND:
	return (MRESULT)TRUE;

      case WM_CHAR:
      {
	USHORT	fsflags = SHORT1FROMMP(mp1);
	USHORT	usch    = SHORT1FROMMP(mp2);
	USHORT	usvk    = SHORT2FROMMP(mp2);
	int	key;

	if (capturing < 0 || (fsflags & KC_KEYUP))
	    break;

	// Escape backs out without changing anything.
	if ((fsflags & KC_VIRTUALKEY) && usvk == VK_ESC)
	{
	    EndCapture ();
	    return (MRESULT)TRUE;
	}

	key = KeyFromMessage (fsflags, usch, usvk);

	if (!key)
	{
	    WinAlarm (HWND_DESKTOP, WA_WARNING);
	    return (MRESULT)TRUE;
	}

	CfgSetInt ((char *)actions[capturing].name, key);
	EndCapture ();
	FillKeyList ();

	return (MRESULT)TRUE;
      }

      case WM_COMMAND:
	switch (SHORT1FROMMP(mp1))
	{
	  case IDB_CHANGE:
	    StartCapture ();
	    return 0;

	  case IDB_DEFAULTS:
	  {
	    int	i;

	    for (i = 0; i < NUM_ACTIONS; i++)
		CfgSetInt ((char *)actions[i].name, actions[i].fallback);

	    FillKeyList ();
	    return 0;
	  }

	  case IDB_SAVE:
	    ControlsToCfg ();

	    if (!CfgSave ())
	    {
		char	msg[CCHMAXPATH + 64];

		sprintf (msg, "%s could not be written.", cfgPath);
		WinMessageBox (HWND_DESKTOP, hwndFrame, (PSZ)msg,
			       (PSZ)"DOOM Setup", 0, MB_OK | MB_ERROR);
		return 0;
	    }

	    WinPostMsg (hwnd, WM_QUIT, 0, 0);
	    return 0;

	  case IDB_CANCEL:
	    WinPostMsg (hwnd, WM_QUIT, 0, 0);
	    return 0;

	  default:
	    break;
	}
	break;

      case WM_CLOSE:
	WinPostMsg (hwnd, WM_QUIT, 0, 0);
	return 0;

      default:
	break;
    }

    return WinDefWindowProc (hwnd, msg, mp1, mp2);
}


int main (int argc, char** argv)
{
    HAB		hab;
    HMQ		hmq;
    QMSG	qmsg;
    ULONG	flFrame;
    RECTL	rcl;
    LONG	scr_cx, scr_cy, frame_cx, frame_cy;

    // A configuration file may be named; otherwise the one in the current
    // directory, which is where the game keeps its own.
    if (argc > 1)
    {
	strncpy (cfgPath, argv[1], sizeof(cfgPath) - 1);
	cfgPath[sizeof(cfgPath) - 1] = 0;
    }

    CfgLoad ();

    hab = WinInitialize (0);
    if (hab == NULLHANDLE)
	return 1;

    hmq = WinCreateMsgQueue (hab, 0);
    if (hmq == NULLHANDLE)
    {
	WinTerminate (hab);
	return 1;
    }

    if (!WinRegisterClass (hab, (PSZ)CLASS_NAME, SetupWndProc, 0, 0))
    {
	WinDestroyMsgQueue (hmq);
	WinTerminate (hab);
	return 1;
    }

    flFrame = FCF_TITLEBAR | FCF_SYSMENU | FCF_MINBUTTON | FCF_BORDER
	    | FCF_TASKLIST;

    hwndFrame = WinCreateStdWindow (HWND_DESKTOP, 0, &flFrame,
				    (PSZ)CLASS_NAME,
				    (PSZ)"DOOM for OS/2 -- Setup",
				    0, NULLHANDLE, 0, &hwndClient);

    if (hwndFrame == NULLHANDLE)
    {
	WinDestroyMsgQueue (hmq);
	WinTerminate (hab);
	return 1;
    }

    //
    // A configuration with no stamp was not written by this port, which means
    // its key bindings are DOS scan codes rather than DOOM key codes.
    //
    // That matters more here than anywhere else.  Saving from this program
    // stamps the file -- it has to, since the stamp is what stops the game
    // translating bindings that are already ours -- and stamping a file full
    // of scan codes would freeze them in place as though they were correct,
    // leaving a keyboard that does nothing and nothing left to explain why.
    //
    // The game knows how to convert them and this program deliberately does
    // not, so rather than half-know, the ten bindings go back to DOOM's own
    // defaults and the player is told what to do instead.
    //
    if (cfgCount > 0 && CfgGetInt ("os2_cfgversion", 0) < 1)
    {
	int	i;

	for (i = 0; i < NUM_ACTIONS; i++)
	    CfgSetInt ((char *)actions[i].name, actions[i].fallback);

	WinMessageBox (HWND_DESKTOP, hwndFrame,
		       (PSZ)"This DEFAULT.CFG was not written by DOOM for "
			    "OS/2, so its key bindings are in the DOS game's "
			    "format and cannot be shown correctly here.\n\n"
			    "They have been set back to the standard "
			    "bindings.\n\n"
			    "To keep the bindings you already had, press "
			    "Cancel, run DOOM once and quit it -- the game "
			    "converts them on the way in -- then start Setup "
			    "again.",
		       (PSZ)"DOOM Setup", 0, MB_OK | MB_INFORMATION);
    }

    LoadToControls ();

    rcl.xLeft	= 0;
    rcl.yBottom	= 0;
    rcl.xRight	= CLIENT_W;
    rcl.yTop	= CLIENT_H;
    WinCalcFrameRect (hwndFrame, &rcl, FALSE);

    frame_cx = rcl.xRight - rcl.xLeft;
    frame_cy = rcl.yTop - rcl.yBottom;

    scr_cx = WinQuerySysValue (HWND_DESKTOP, SV_CXSCREEN);
    scr_cy = WinQuerySysValue (HWND_DESKTOP, SV_CYSCREEN);

    WinSetWindowPos (hwndFrame, HWND_TOP,
		     (scr_cx - frame_cx) / 2, (scr_cy - frame_cy) / 2,
		     frame_cx, frame_cy,
		     SWP_SIZE | SWP_MOVE | SWP_SHOW | SWP_ACTIVATE);

    WinSetFocus (HWND_DESKTOP, hwndClient);

    while (WinGetMsg (hab, &qmsg, NULLHANDLE, 0, 0))
	WinDispatchMsg (hab, &qmsg);

    WinDestroyWindow (hwndFrame);
    WinDestroyMsgQueue (hmq);
    WinTerminate (hab);

    return 0;
}
