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
//	Globally defined strings.
// 
//-----------------------------------------------------------------------------

static const char
rcsid[] = "$Id: m_bbox.c,v 1.1 1997/02/03 22:45:10 b1 Exp $";


#ifdef __GNUG__
#pragma implementation "dstrings.h"
#endif
#include "dstrings.h"



char* endmsg[NUM_QUITMESSAGES+1]=
{
  // DOOM1
  QUITMSG,
  "please don't leave, there's more\ndemons to toast!",
  "let's beat it -- this is turning\ninto a bloodbath!",
  // These three said "dos" and "the dos prompt", which is not where anyone
  // playing this ends up.
  "i wouldn't leave if i were you.\nos/2 is much worse.",
  "you're trying to say you like os/2\nbetter than me, right?",
  "don't leave yet -- there's a\ndemon around that corner!",
  "ya know, next time you come in here\ni'm gonna toast ya.",

  // The comma at the end of the line below is not in the released source, and
  // leaving it out was a bug: two adjacent string literals in C are one
  // string, so this message and the first DOOM II one were quietly glued into
  // a single entry that ran off the side of the box.  The same thing happened
  // again at the end of the DOOM II block.  Both are commas now.
  "go ahead and leave. see if i care.",

  // QuitDOOM II messages
  "you want to quit?\nthen, thou hast lost an eighth!",
  "don't go now, there's a \ndimensional shambler waiting\nfor you out in os/2!",
  "get outta here and go back\nto your boring programs.",
  "if i were your boss, i'd \n deathmatch ya in a minute!",
  "look, bud. you leave now\nand you forfeit your body count!",
  "just leave. when you come\nback, i'll be waiting with a bat.",
  "you're lucky i don't smack\nyou for thinking about leaving.",

  // FinalDOOM?
  //
  // The six that stood here were the crude ones, and they are replaced
  // rather than deleted: the table is indexed by gametic modulo a count, so
  // taking entries out would have meant changing the count in DSTRINGS.H to
  // match, and a table with a hole in it is a worse thing to leave behind
  // than a table with different jokes in it.  Same voice, same shape.
  "go on then, coward!\nsee if i care!",
  "quit, and every demon here\ngets to keep the change.",
  "if you leave, i'm telling\nthe cyberdemon you said that.",
  "hey, ron! can we say\n'darn' in the game?",
  "i'd leave: this is just\nmore monsters and levels.\nwhat a load.",
  "is that it? i've seen\nimps with more backbone.",
  "don't quit now! we're \nstill spending your money!",

  // Internal debug. Different style, too.
  "THIS IS NO MESSAGE!\nPage intentionally left blank."
};


  


