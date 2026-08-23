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
//	Finding and identifying the IWAD -- OS/2 port.
//
//	The released code works out which game it is holding by the *name* of
//	the file, from a list of seven it knows (D_MAIN.C).  That has two
//	consequences, and both of them bite:
//
//	  - An IWAD under any other name is not found at all.  The game stops
//	    with "W_InitFiles: no files found" and never says what it was
//	    looking for.
//
//	  - Retail DOOM is only recognised as retail if the file is called
//	    DOOMU.WAD.  Every real copy of it -- Ultimate DOOM, the Special
//	    Edition, the Depths of DOOM boxes -- ships the file as DOOM.WAD,
//	    which matches the *next* test down and is taken for the
//	    three-episode registered game.  It loads, it plays, and Episode 4
//	    is silently missing from the menu.  Nothing reports this.
//
//	So this asks the file instead.  A WAD carries a directory of every
//	lump in it, and which maps are present says exactly which game it is:
//	MAP01 means DOOM II or a mission pack, E4M1 means retail, E2M1 means
//	registered, and E1M1 alone means the shareware episode.  The name on
//	the disk is then only used for the one thing it is still the best
//	evidence for -- telling TNT and Plutonia apart from DOOM II, which are
//	identical in structure.
//
//	It also looks in more than one place, and -iwad names a file outright.
//
//-----------------------------------------------------------------------------

static const char
rcsid[] = "$Id:$";

#define INCL_DOSFILEMGR
#define INCL_DOSERRORS

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <io.h>

#include "os2doom.h"

#include "doomdef.h"
#include "doomstat.h"
#include "d_main.h"
#include "m_argv.h"
#include "i_system.h"
#include "w_wad.h"


#define MAX_SEARCH_DIRS		4

// Kept for the report printed when nothing is found: there is little more
// annoying than a program that says it cannot find a file without saying
// where it looked.
static char*	searchdir[MAX_SEARCH_DIRS];
static int	numsearchdirs;

// Every IWAD the search turned up, in the order it found them -- which is the
// released code's order of preference first, so iwads[0] is the one this
// program has always started, and anything else afterwards.
static os2iwad_t	iwads[MAX_IWAD_CHOICES];
static int		numiwads;


//
// LumpNameIs
//
// Compares one entry of a WAD directory against a name.
//
// Lump names are eight bytes, padded with NULs rather than terminated by
// them, and DOOM has never cared about their case.  A plain strcmp on the
// field would read past the end of a full eight character name.
//
static boolean LumpNameIs (char *field, char *name)
{
    int		i;

    for (i = 0; i < 8; i++)
    {
	char	a = field[i];
	char	b = name[i];

	if (a >= 'a' && a <= 'z')  a = a - 'a' + 'A';
	if (b >= 'a' && b <= 'z')  b = b - 'a' + 'A';

	if (a != b)
	    return false;

	// Both ended together: a match.  Only one ended: they differ, and
	// that was caught above.
	if (!a)
	    return true;
    }

    return true;
}


//
// D_ReadIwadMode
//
// Opens a candidate file, and works out from its lump directory which game
// it holds.  Returns false if it cannot be read, is not a WAD at all, or is
// a PWAD rather than an IWAD.
//
static boolean D_ReadIwadMode (char *path, GameMode_t *mode)
{
    int			handle;
    wadinfo_t		header;
    filelump_t*		directory;
    int			length;
    int			i;
    boolean		e1m1 = false;
    boolean		e2m1 = false;
    boolean		e4m1 = false;
    boolean		map01 = false;

    // O_BINARY matters here as much as it does anywhere else in the port: a
    // text mode read would stop dead at the first 0x1a byte, and a WAD is
    // full of them.
    handle = open (path, O_RDONLY | O_BINARY);
    if (handle == -1)
	return false;

    if (read (handle, &header, sizeof(header)) != sizeof(header))
    {
	close (handle);
	return false;
    }

    // A PWAD is a patch, not a game.  Loading one as the IWAD would leave
    // the game with no textures, no sounds and no menu graphics.
    if (strncmp (header.identification, "IWAD", 4))
    {
	close (handle);
	return false;
    }

    if (header.numlumps <= 0 || header.numlumps > 65535)
    {
	close (handle);
	return false;
    }

    length = header.numlumps * sizeof(filelump_t);

    // This runs before Z_Init, so the zone heap does not exist yet.
    directory = (filelump_t *) malloc (length);
    if (!directory)
    {
	close (handle);
	return false;
    }

    if (lseek (handle, header.infotableofs, SEEK_SET) == -1
	|| read (handle, directory, length) != length)
    {
	free (directory);
	close (handle);
	return false;
    }

    close (handle);

    for (i = 0; i < header.numlumps; i++)
    {
	if      (LumpNameIs (directory[i].name, "E1M1"))  e1m1  = true;
	else if (LumpNameIs (directory[i].name, "E2M1"))  e2m1  = true;
	else if (LumpNameIs (directory[i].name, "E4M1"))  e4m1  = true;
	else if (LumpNameIs (directory[i].name, "MAP01")) map01 = true;
    }

    free (directory);

    // Most specific first.  A retail WAD contains E1M1 and E2M1 as well, so
    // testing in the other order would call every retail IWAD shareware.
    if (map01)
	*mode = commercial;
    else if (e4m1)
	*mode = retail;
    else if (e2m1)
	*mode = registered;
    else if (e1m1)
	*mode = shareware;
    else
	return false;			// an IWAD with no maps: not a game

    return true;
}


//
// MissionFromName
//
// TNT: Evilution and Plutonia are DOOM II structurally -- same MAP01 through
// MAP32, same everything -- so nothing inside them separates the three.  The
// file name is the only evidence there is, which is why it is still consulted
// for this one decision.  Getting it wrong costs the right end-of-game text
// and nothing else.
//
static GameMission_t MissionFromName (char *path)
{
    char*	base;
    char*	p;

    base = path;
    for (p = path; *p; p++)
	if (*p == '/' || *p == '\\' || *p == ':')
	    base = p + 1;

    if (!strnicmp (base, "tnt", 3))
	return pack_tnt;

    if (!strnicmp (base, "plutonia", 8))
	return pack_plut;

    return doom2;
}


//
// D_DescribeIwad
//
// What to call this game somewhere a person will read it.
//
// The names are the ones off the boxes rather than the engine's own
// vocabulary: someone choosing between two files knows "The Ultimate DOOM"
// and would have to stop and think about "retail".  Episode counts are kept
// for the two that are otherwise easy to mistake for each other.
//
static void D_DescribeIwad (GameMode_t mode, GameMission_t mission, char *out)
{
    switch (mode)
    {
      case shareware:
	strcpy (out, "DOOM (shareware, 1 episode)");
	break;

      case registered:
	strcpy (out, "DOOM (registered, 3 episodes)");
	break;

      case retail:
	strcpy (out, "The Ultimate DOOM (4 episodes)");
	break;

      case commercial:
	if (mission == pack_tnt)
	    strcpy (out, "Final DOOM: TNT Evilution");
	else if (mission == pack_plut)
	    strcpy (out, "Final DOOM: The Plutonia Experiment");
	else
	    strcpy (out, "DOOM II: Hell on Earth");
	break;

      default:
	strcpy (out, "an unrecognised DOOM WAD");
	break;
    }
}


//
// D_IdentifyIwad
//
// Reads the file and works out which game it holds.  Changes nothing.
//
static boolean D_IdentifyIwad (char *path, os2iwad_t *out)
{
    GameMode_t		mode;
    GameMission_t	mission;

    if (strlen (path) >= CCHMAXPATH)
	return false;

    if (!D_ReadIwadMode (path, &mode))
	return false;

    mission = (mode == commercial) ? MissionFromName (path) : doom;

    strcpy (out->path, path);
    out->mode	 = (int)mode;
    out->mission = (int)mission;

    D_DescribeIwad (mode, mission, out->name);

    return true;
}


//
// D_AddCandidate
//
// Identify the file and add it to the list, unless it is not an IWAD or is
// already on it.
//
// The same file is very often reached twice: once as one of the seven names
// the released code knows, and again by the sweep for anything else that
// turns out to be an IWAD.  Both build their paths out of the same directory
// string in the same shape, so comparing whole paths settles it.
//
static boolean D_AddCandidate (char *path)
{
    int		i;

    if (numiwads >= MAX_IWAD_CHOICES)
	return false;

    for (i = 0; i < numiwads; i++)
	if (!stricmp (iwads[i].path, path))
	    return false;

    if (!D_IdentifyIwad (path, &iwads[numiwads]))
	return false;

    numiwads++;
    return true;
}


//
// D_AdoptIwad
//
// Play this one: set the game mode and mission, and put the file at the head
// of the WAD list.
//
static void D_AdoptIwad (os2iwad_t *w)
{
    char*	copy;

    gamemode	= (GameMode_t)w->mode;
    gamemission	= (GameMission_t)w->mission;

    // D_AddFile keeps the pointer it is given, so it has to outlive this
    // function -- the search builds its paths in a scratch buffer.
    copy = (char *) malloc (strlen(w->path) + 1);
    if (!copy)
	I_Error ("D_AdoptIwad: out of memory");
    strcpy (copy, w->path);

    D_AddFile (copy);

    printf ("IWAD: %s (%s)\n", copy, w->name);
}


//
// D_TryIwad
//
// Identify one named file and play it if it is an IWAD.  Only -iwad uses
// this: it names a file outright, so there is nothing to choose between.
//
static boolean D_TryIwad (char *path)
{
    os2iwad_t	w;

    if (!D_IdentifyIwad (path, &w))
	return false;

    D_AdoptIwad (&w);
    return true;
}


//
// AddSearchDir
//
static void AddSearchDir (char *dir)
{
    int		i;

    if (!dir || !*dir || numsearchdirs >= MAX_SEARCH_DIRS)
	return;

    // Do not look in the same place twice, which is the usual case:
    // DOOMWADDIR pointing at the directory the program is already in.
    for (i = 0; i < numsearchdirs; i++)
	if (!stricmp (searchdir[i], dir))
	    return;

    searchdir[numsearchdirs] = (char *) malloc (strlen(dir) + 1);
    if (!searchdir[numsearchdirs])
	return;

    strcpy (searchdir[numsearchdirs], dir);
    numsearchdirs++;
}


//
// BuildSearchDirs
//
// DOOMWADDIR, then the current directory, then wherever DOOM.EXE itself is.
//
// That last one is not padding.  A program started by double clicking its
// object on the Workplace Shell does not necessarily have its own directory
// as the current one, so a DOOM.EXE sitting happily beside its WAD can still
// fail to find it.
//
static void BuildSearchDirs (void)
{
    char	exedir[CCHMAXPATH];
    char*	p;
    char*	base;

    numsearchdirs = 0;

    AddSearchDir (getenv ("DOOMWADDIR"));
    AddSearchDir (".");

    if (myargv && myargv[0] && strlen(myargv[0]) < CCHMAXPATH)
    {
	strcpy (exedir, myargv[0]);

	base = NULL;
	for (p = exedir; *p; p++)
	    if (*p == '/' || *p == '\\' || *p == ':')
		base = p;

	if (base)
	{
	    *base = '\0';
	    AddSearchDir (exedir);
	}
    }
}


//
// ScanDirForIwads
//
// Every *.WAD in the directory that turns out to be an IWAD.
//
// This is what finds a perfectly good IWAD that happens to be called
// something nobody anticipated -- and, now that the whole list is collected
// rather than the first match taken, what makes sure a directory holding
// DOOM2.WAD and MYDOOM.WAD offers both.
//
static void ScanDirForIwads (char *dir)
{
    HDIR		hdir = HDIR_CREATE;
    FILEFINDBUF3	found;
    ULONG		count = 1;
    char		pattern[CCHMAXPATH];
    char		path[CCHMAXPATH];

    if (strlen(dir) + 8 >= CCHMAXPATH)
	return;

    sprintf (pattern, "%s/*.wad", dir);

    if (DosFindFirst ((PSZ)pattern, &hdir,
		      FILE_NORMAL | FILE_READONLY | FILE_ARCHIVED,
		      &found, sizeof(found), &count,
		      FIL_STANDARD) != NO_ERROR)
	return;

    do
    {
	if (strlen(dir) + 1 + strlen(found.achName) < CCHMAXPATH)
	{
	    sprintf (path, "%s/%s", dir, found.achName);
	    D_AddCandidate (path);
	}

	count = 1;
    }
    while (DosFindNext (hdir, &found, sizeof(found), &count) == NO_ERROR);

    DosFindClose (hdir);
}


//
// D_OS2FindIWAD
//
// Returns true once an IWAD has been found and registered.  Does not return
// at all if there is none: it says where it looked and stops, which is a good
// deal more use than "no files found".
//
boolean D_OS2FindIWAD (void)
{
    // The seven names the released code knows, still tried first and still
    // in its order of preference, so that a directory holding several IWADs
    // starts the same game this DOOM has always started.
    static char*	knownnames[] =
    {
	"doom2f.wad", "doom2.wad", "plutonia.wad", "tnt.wad",
	"doomu.wad", "doom.wad", "doom1.wad",
	NULL
    };

    char	path[CCHMAXPATH];
    int		p;
    int		d;
    int		n;

    //
    // -iwad <file>.  Names a file outright, and is not a guess, so a failure
    // here is reported rather than quietly falling through to the search.
    //
    p = M_CheckParm ("-iwad");
    if (p && p < myargc-1)
    {
	if (D_TryIwad (myargv[p+1]))
	    return true;

	// Same rule as below: the explanation to the log, the fact to the
	// screen.  A PWAD given to -iwad is the usual cause -- it holds only
	// replacement lumps and cannot start a game on its own.
	printf ("-iwad: %s must exist, be readable, and be an IWAD rather"
		" than a PWAD.\n", myargv[p+1]);

	I_Error ("%s is not a DOOM WAD file.", myargv[p+1]);
    }

    BuildSearchDirs ();

    //
    // The known names, in every search directory.
    //
    // These go in first and in the released code's own order, so that
    // iwads[0] is whatever this program would have started before there was
    // any choosing to do.  That is what the window opens on.
    //
    for (d = 0; d < numsearchdirs; d++)
    {
	for (n = 0; knownnames[n]; n++)
	{
	    if (strlen(searchdir[d]) + 1 + strlen(knownnames[n]) >= CCHMAXPATH)
		continue;

	    sprintf (path, "%s/%s", searchdir[d], knownnames[n]);

	    D_AddCandidate (path);
	}
    }

    //
    // Anything else that turns out to be an IWAD.
    //
    for (d = 0; d < numsearchdirs; d++)
	ScanDirForIwads (searchdir[d]);

    //
    // One game: play it, and say nothing.  Asking a question with a single
    // answer is not a courtesy.
    //
    if (numiwads == 1)
    {
	D_AdoptIwad (&iwads[0]);
	return true;
    }

    //
    // Several: ask.
    //
    if (numiwads > 1)
    {
	int	pick;

	printf ("Found %i games:\n", numiwads);
	for (d = 0; d < numiwads; d++)
	    printf ("    %s (%s)\n", iwads[d].path, iwads[d].name);

	pick = I_OS2_ChooseIwad (iwads, numiwads, 0);

	if (pick < 0)
	{
	    // They closed the window rather than picking.  That is an answer,
	    // and it is not an error: no message box, no "DOOM aborted", just
	    // the log saying so and a clean exit.
	    printf ("No game chosen; quitting.\n");

	    I_OS2_LoadWindowClose ();
	    I_OS2_LogShutdown ();

	    fflush (NULL);
	    DosExit (EXIT_PROCESS, 0);
	}

	D_AdoptIwad (&iwads[pick]);
	return true;
    }

    //
    // Nothing.  The particulars go to the log; the screen gets a sentence.
    //
    // Which names were tried and which directories were searched is exactly
    // what is wanted when working out why a WAD that is plainly there was not
    // found -- and exactly what nobody wants to read in a message box.
    // DOOM.LOG keeps it, and keeps it after the box has been dismissed.
    //
    printf ("\n");
    printf ("No DOOM IWAD found.  Looked for doom2f.wad, doom2.wad,"
	    " plutonia.wad, tnt.wad,\n");
    printf ("doomu.wad, doom.wad and doom1.wad -- and for any other file"
	    " that is an\n");
    printf ("IWAD -- in:\n");
    for (d = 0; d < numsearchdirs; d++)
	printf ("    %s\n", searchdir[d]);
    printf ("\n");

    I_Error ("WAD file not found.");

    return false;			// not reached
}
