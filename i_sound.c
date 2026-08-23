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
//	System interface for sound.
//
//-----------------------------------------------------------------------------

static const char
rcsid[] = "$Id: i_unix.c,v 1.5 1997/02/03 22:45:10 b1 Exp $";

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include <math.h>

// Presentation Manager handles and the OS/2 base API.
#include "os2doom.h"

// MMPM/2.
//
// DART -- Direct Audio Real-Time -- is OS/2's low latency waveform
// interface.  It hands the program a small ring of buffers and calls back on
// a thread of its own each time one has finished playing, expecting it to be
// filled again.
//
// That is a better fit for DOOM than what the Linux original did, which was
// to mix a buffer per rendered frame and write it to /dev/dsp.  The write
// blocked once the driver's four kilobytes were full, so the sound card
// ended up dictating the frame rate -- and since each mix is about 46 ms of
// audio and a frame is meant to be 28, it dictated a slow one.  Here the
// mixing happens in the callback instead, at exactly the rate the hardware
// drains it, and the game loop is left alone.
#define INCL_OS2MM
#define INCL_MMIOOS2

// A name collision worth knowing about.  os2medef.h has "typedef WORD
// VERSION", and doomdef.h has "enum { VERSION = 110 }" -- the game version
// that goes into the network protocol and the savegames.  Neither one is a
// macro, so no amount of undef separates them; the MMPM/2 type is renamed
// on the way in instead.  Nothing in this port ever refers to it.
#define VERSION MMPM_VERSION
#include <os2me.h>
#undef VERSION

#include "z_zone.h"

#include "i_system.h"
#include "i_sound.h"
#include "m_argv.h"
#include "m_misc.h"
#include "w_wad.h"

#include "doomdef.h"

// UNIX hack, to be removed.
#ifdef SNDSERV
// Separate sound server process.
FILE*	sndserver=0;
char*	sndserver_filename = "./sndserver ";
#elif SNDINTR

// Update all 30 millisecs, approx. 30fps synchronized.
// Linux resolution is allegedly 10 millisecs,
//  scale is microseconds.
#define SOUND_INTERVAL     500

// Get the interrupt. Set duration in millisecs.
int I_SoundSetTimer( int duration_of_tick );
void I_SoundDelTimer( void );
#else
// None?
#endif


// A quick hack to establish a protocol between
// synchronous mix buffer updates and asynchronous
// audio writes. Probably redundant with gametic.
static int flag = 0;

// The number of internal mixing channels,
//  the samples calculated for each mixing step,
//  the size of the 16bit, 2 hardware channel (stereo)
//  mixing buffer, and the samplerate of the raw data.


// Needed for calling the actual sound output.
#define SAMPLECOUNT		512
#define NUM_CHANNELS		8
// It is 2 for 16bit, and 2 for two channels.
#define BUFMUL                  4
#define MIXBUFFERSIZE		(SAMPLECOUNT*BUFMUL)

#define SAMPLERATE		11025	// Hz
#define SAMPLESIZE		2   	// 16bit

// The actual lengths of all sound effects.
int 		lengths[NUMSFX];

//
// The actual output device: MMPM/2 DART.
//
// mciSendCommand lives in MDM.DLL and is loaded by name rather than
// imported, for the same reason DIVE is in I_VIDEO.C.  A machine with no
// multimedia support installed has no MDM.DLL, and a DOOM.EXE that imported
// it would refuse to load at all rather than simply running without sound.
//
// The typedef and the loading both live in os2doom.h / I_SYSTEM.C now, so
// that the music module can share the one MDM.DLL.  This is just the cached
// entry point.
static PFNMCISENDCOMMAND	pMciSendCommand	= NULL;

// How many buffers DART keeps in flight.  Each one holds whole mixes of
// SAMPLECOUNT stereo frames -- about 46 ms at 11025 Hz -- so three of them
// is something over a tenth of a second of slack.  Fewer buffers means less
// delay between pulling the trigger and hearing it; more means fewer breaks
// in the sound when the machine is busy.
#define NUM_DART_BUFFERS	3

// One mix as DOOM produces it, in bytes: SAMPLECOUNT frames, two channels,
// two bytes each.  What is handed to DART may be smaller -- see below.
#define DART_CHUNK		(SAMPLECOUNT*BUFMUL)

//
// What the sound card agreed to take.
//
// DOOM mixes in 16 bit signed stereo and nothing else, but not every card
// will play that.  The Sound Blaster this port was first tried on is an MCA
// card of 1989 vintage: eight bit, mono, and no amount of asking will make it
// anything else.  MCI_MIXSETUP simply refuses, and refusing is all it does --
// it does not say what it would have accepted instead.
//
// So the formats are offered in turn, best first, and the mix is converted
// into whichever one is taken.  Sixteen bit stereo costs nothing, being a
// straight copy; the others cost one pass over 512 frames, which is nothing
// measured against mixing them in the first place.
//
// The rate is negotiated too, because a driver that only advertises 22050 or
// 44100 refuses 11025 exactly as flatly as it refuses a format it cannot
// play.  Both are whole multiples of DOOM's own 11025, so meeting them costs
// only repeating each frame -- crude as resampling goes, but the samples in
// the WAD are 11025 Hz mono to begin with and interpolating invents detail
// that was never recorded.
static int			dartBits     = 16;
static int			dartChannels = 2;
static int			dartRate     = SAMPLERATE;
static int			dartRateMul  = 1;

// One mix in the format actually agreed: SAMPLECOUNT frames of it.
static ULONG			dartChunk    = DART_CHUNK;

static USHORT			mciDeviceID = 0;
static MCI_MIXSETUP_PARMS	mciMixSetup;
static MCI_BUFFER_PARMS		mciBufferParms;
static MCI_MIX_BUFFER		mciBuffers[NUM_DART_BUFFERS];

// The size DART actually gave us, which need not be the size asked for.
static ULONG			dartBufSize = 0;

static boolean			dartUp	    = false;	// stream is running
static boolean			dartBuffers = false;	// memory is allocated

//
// The playlist path: streaming for drivers that came before DART.
//
// MCI_MIXSETUP *is* the DART interface, so a waveaudio driver that does not
// implement DART refuses every format offered, identically, with no hint that
// the formats were never the problem.  The Sound Blaster MCV driver is one of
// these -- the card is a 1989 design and its OS/2 support predates the direct
// audio interface by years.
//
// What such a driver does support is the older mechanism: a playlist.  A
// playlist is a little program the audio device executes -- play this block
// of memory, tell the application about it, play the next, branch back to the
// start -- and it streams continuously without DART.
//
// Two things follow from the shape of it.  The refill happens on the window
// thread rather than on a callback thread of the driver's, which means no
// mutex is needed but also that a long stall in the game is heard.  And the
// BRANCH at the end means the device never runs out of anything to play: if
// the game is too busy to refill a buffer the old contents are played again,
// which is a glitch rather than silence.  A stream that stops cannot easily
// be restarted; one that repeats itself recovers by itself.
//
#define NUM_PL_BUFFERS		4

// Started late, from I_SubmitSound, once there is a window for it to report
// to; defined further down, with the rest of the playlist code.
static boolean I_InitPlaylist (void);
static void    I_FreePlaylistMemory (void);
static void    I_PlaylistTimedRefill (void);

//
// Everything the audio driver is given a pointer to is allocated with
// OBJ_TILE, and that is not a detail.
//
// The Sound Blaster MCV driver is a 16 bit device driver from the OS/2 1.x
// era.  Handed a 32 bit flat address it converts it to a 16:16 selector and
// offset -- and that conversion is only meaningful for memory in the tiled
// arena, where a valid 16 bit alias for each page is guaranteed to exist.
// Ordinary DosAllocMem memory has no such alias, so the driver computes an
// address that belongs to something else entirely and then reads and writes
// it at interrupt time.
//
// The symptom of getting this wrong is not a trap in DOOM.  It is the whole
// machine stopping, and the disk being left in whatever state it was in.
//
static boolean		playlistUp	= false;
static boolean		playlistPending	= false;
static ULONG*		playlist	= NULL;
static PVOID		plListMem	= NULL;
static byte*		plBuffer[NUM_PL_BUFFERS];
static PVOID		plMemory	= NULL;
static ULONG		plNext		= 0;

// How big one block is.  Not the same as one mix: a 512 byte block is only
// 46 milliseconds of eight bit mono, and a driver of this age may well have a
// minimum transfer size larger than that and simply do nothing with blocks
// below it.  Blocks are therefore rounded up to at least PL_MIN_BLOCK, and
// hold however many whole mixes that comes to.
#define PL_MIN_BLOCK		4096

static ULONG		plBlockSize	= 0;

// Whether the device is actually asking for data.  A playlist that opens and
// plays but never reports a block is indistinguishable, from the speaker, from
// one that was never started -- both are silence -- so it is worth saying
// which of the two happened.
static ULONG		plMessages	= 0;
static int		plStartTime	= 0;
static boolean		plComplained	= false;

//
// Refilling by the clock, for a driver that never asks.
//
// MESSAGE_OPERATION is the part of a playlist that a driver is most likely to
// have left out: DATA and BRANCH are what make sound come out, and a message
// back to the application is a convenience.  A driver that ignores them plays
// the same few blocks round and round for ever and never says a word, which
// from the speaker is silence.
//
// Nothing about that stops us knowing when a block is free, though.  The
// device consumes a block in a time that is fixed by its size and the sample
// rate, so the clock says where the playlist has got to just as well as the
// driver would have -- and unlike the driver, the clock is always there.
//
// Only used when no message has arrived at all.  A driver that does send them
// is believed in preference: it knows, and this only calculates.
//
static boolean		plTimed		= false;
static ULONG		plNextFill	= 0;
static int		plBlockTics	= 1;

//
// When the device last said anything.
//
// This was once used to restart a playlist that had gone quiet for a second,
// on the theory that a playlist which ends instead of branching looks exactly
// like a driver that has stopped reporting.  It cost a trap that took the
// machine down with it, and the reason is worth keeping written down: the
// count only advances when a window message is dispatched, and nothing
// dispatches messages during a level load.  So the first tic after every level
// saw a second of apparent silence, rewrote all four blocks -- including the
// one the card was reading out of at that moment -- and issued a second
// MCI_PLAY at a device that had never stopped playing.  A 16 bit driver from
// 1992 is under no obligation to survive that, and did not.
//
// It is kept only as something the diagnostics can report.  Nothing acts on
// it.
//
static int		plLastMsg	= 0;

// The loudest sample the mixer has produced, and how many times the game has
// asked for a sound effect.  Between them these are the one thing the log
// cannot otherwise show: whether the silence is DOOM's or the driver's.
#define PL_MAX_REPORTS		8

static int		plPeak		= 0;
static int		sfxStarts	= 0;
static int		plReports	= 0;
static int		plNextReport	= 0;

// Blocks mixed for so far this tic, and how many requests have been turned
// down because the game was already behind.  See the safety valve in
// I_OS2_PlaylistNotify.
static int		plBurst		= 0;
static int		plDropped	= 0;

// Whether the playlist asks the driver to report its progress, or works the
// timing out for itself.  Off by default, for reasons set out at length above
// I_BuildPlaylist; -plmessages turns it on.
static boolean		plUseMessages	= false;

//
// Chunked playback: one pass at a time, re-armed.
//
// A looping playlist -- four blocks and a BRANCH back to the first -- takes
// this machine down every time, after a few seconds of playing perfectly
// well.  A playlist that ends instead of looping does not: it plays its four
// blocks and stops, and the machine is still standing afterwards.  That was
// tested for exactly this reason and it is the only thing separating the two.
//
// So the driver can be handed buffers and will DMA out of them safely.  What
// it cannot survive is being left to walk the same list round and round: the
// most likely reading is that something is accounted for per DATA element
// executed and reclaimed only when a play completes, so a list that never
// completes never gives it back, and a few hundred elements in, the driver
// runs out and faults at ring 0.  That fits the timing, it fits why every
// earlier fix missed, and it fits why -plexit survives.
//
// It also says what to do about it.  Rather than one endless play, every pass
// is a complete one: fill the blocks, play them, wait for the device to say
// it has finished, then do it again.  Each pass ends properly, so whatever is
// being accumulated is handed back at the end of each.
//
// The cost is a seam between passes, and it is not free -- 371 milliseconds
// of sound at a time, with however long the game takes to notice the pass
// ended before the next one starts.  Choppy sound is worth having where the
// alternative is a reboot.
//
// -plbranch restores the single looping playlist for a driver that can take
// one, which is the better arrangement where it works.
//
static boolean		plChunked	= true;
static boolean		plPlaying	= false;	// a pass is in flight
static int		plPassTics	= 1;		// how long one lasts
static int		plPassEnd	= 0;		// tic it should end by
static int		plPasses	= 0;

// How long past the calculated end of a pass to wait before giving up on
// being told and assuming it finished.  The notification is the primary
// signal; this only stops a dropped one from stopping the sound for good.
#define PL_PASS_MARGIN		4

static boolean I_StartPass (void);

static void I_ReportSound (void);

// Guards the mixing channel table, which two threads touch: the game thread
// by way of I_StartSound, and DART's own thread by way of the callback.
static HMTX			sndMutex = NULLHANDLE;

// The mixer proper, further down this file.  It fills mixbuffer with exactly
// SAMPLECOUNT stereo frames.  In this port it is driven from DART's thread
// when a buffer comes free, not from the game loop -- see I_UpdateSound.
static void I_MixBuffer (void);

// The global mixing buffer.
// Basically, samples from all active internal channels
//  are modifed and added, and stored in the buffer
//  that is submitted to the audio device.
signed short	mixbuffer[MIXBUFFERSIZE];


// The channel step amount...
unsigned int	channelstep[NUM_CHANNELS];
// ... and a 0.16 bit remainder of last step.
unsigned int	channelstepremainder[NUM_CHANNELS];


// The channel data pointers, start and end.
unsigned char*	channels[NUM_CHANNELS];
unsigned char*	channelsend[NUM_CHANNELS];


// Time/gametic that the channel started playing,
//  used to determine oldest, which automatically
//  has lowest priority.
// In case number of active sounds exceeds
//  available channels.
int		channelstart[NUM_CHANNELS];

// The sound in channel handles,
//  determined on registration,
//  might be used to unregister/stop/modify,
//  currently unused.
int 		channelhandles[NUM_CHANNELS];

// SFX id of the playing sound effect.
// Used to catch duplicates (like chainsaw).
int		channelids[NUM_CHANNELS];			

// Pitch to stepping lookup, unused.
int		steptable[256];

// Volume lookups.
int		vol_lookup[128*256];

// Hardware left and right channel volume lookup.
int*		channelleftvol_lookup[NUM_CHANNELS];
int*		channelrightvol_lookup[NUM_CHANNELS];









//
// This function loads the sound data from the WAD lump,
//  for single sound.
//
void*
getsfx
( char*         sfxname,
  int*          len )
{
    unsigned char*      sfx;
    unsigned char*      paddedsfx;
    int                 i;
    int                 size;
    int                 paddedsize;
    char                name[20];
    int                 sfxlump;

    
    // Get the sound data from the WAD, allocate lump
    //  in zone memory.
    sprintf(name, "ds%s", sfxname);

    // Now, there is a severe problem with the
    //  sound handling, in it is not (yet/anymore)
    //  gamemode aware. That means, sounds from
    //  DOOM II will be requested even with DOOM
    //  shareware.
    // The sound list is wired into sounds.c,
    //  which sets the external variable.
    // I do not do runtime patches to that
    //  variable. Instead, we will use a
    //  default sound for replacement.
    if ( W_CheckNumForName(name) == -1 )
      sfxlump = W_GetNumForName("dspistol");
    else
      sfxlump = W_GetNumForName(name);
    
    size = W_LumpLength( sfxlump );

    // Debug.
    // fprintf( stderr, "." );
    //fprintf( stderr, " -loading  %s (lump %d, %d bytes)\n",
    //	     sfxname, sfxlump, size );
    //fflush( stderr );
    
    sfx = (unsigned char*)W_CacheLumpNum( sfxlump, PU_STATIC );

    // Pads the sound effect out to the mixing buffer size.
    // The original realloc would interfere with zone memory.
    paddedsize = ((size-8 + (SAMPLECOUNT-1)) / SAMPLECOUNT) * SAMPLECOUNT;

    // Allocate from zone memory.
    paddedsfx = (unsigned char*)Z_Malloc( paddedsize+8, PU_STATIC, 0 );
    // ddt: (unsigned char *) realloc(sfx, paddedsize+8);
    // This should interfere with zone memory handling,
    //  which does not kick in in the soundserver.

    // Now copy and pad.
    memcpy(  paddedsfx, sfx, size );
    for (i=size ; i<paddedsize+8 ; i++)
        paddedsfx[i] = 128;

    // Remove the cached lump.
    Z_Free( sfx );
    
    // Preserve padded length.
    *len = paddedsize;

    // Return allocated padded data.
    return (void *) (paddedsfx + 8);
}





//
// This function adds a sound to the
//  list of currently active sounds,
//  which is maintained as a given number
//  (eight, usually) of internal channels.
// Returns a handle.
//
int
addsfx
( int		sfxid,
  int		volume,
  int		step,
  int		seperation )
{
    static unsigned short	handlenums = 0;
 
    int		i;
    int		rc = -1;
    
    int		oldest = gametic;
    int		oldestnum = 0;
    int		slot;

    int		rightvol;
    int		leftvol;

    // Chainsaw troubles.
    // Play these sound effects only one at a time.
    if ( sfxid == sfx_sawup
	 || sfxid == sfx_sawidl
	 || sfxid == sfx_sawful
	 || sfxid == sfx_sawhit
	 || sfxid == sfx_stnmov
	 || sfxid == sfx_pistol	 )
    {
	// Loop all channels, check.
	for (i=0 ; i<NUM_CHANNELS ; i++)
	{
	    // Active, and using the same SFX?
	    if ( (channels[i])
		 && (channelids[i] == sfxid) )
	    {
		// Reset.
		channels[i] = 0;
		// We are sure that iff,
		//  there will only be one.
		break;
	    }
	}
    }

    // Loop all channels to find oldest SFX.
    for (i=0; (i<NUM_CHANNELS) && (channels[i]); i++)
    {
	if (channelstart[i] < oldest)
	{
	    oldestnum = i;
	    oldest = channelstart[i];
	}
    }

    // Tales from the cryptic.
    // If we found a channel, fine.
    // If not, we simply overwrite the first one, 0.
    // Probably only happens at startup.
    if (i == NUM_CHANNELS)
	slot = oldestnum;
    else
	slot = i;

    // Okay, in the less recent channel,
    //  we will handle the new SFX.
    // Set pointer to raw data.
    channels[slot] = (unsigned char *) S_sfx[sfxid].data;
    // Set pointer to end of raw data.
    channelsend[slot] = channels[slot] + lengths[sfxid];

    // Reset current handle number, limited to 0..100.
    if (!handlenums)
	handlenums = 100;

    // Assign current handle number.
    // Preserved so sounds could be stopped (unused).
    channelhandles[slot] = rc = handlenums++;

    // Set stepping???
    // Kinda getting the impression this is never used.
    channelstep[slot] = step;
    // ???
    channelstepremainder[slot] = 0;
    // Should be gametic, I presume.
    channelstart[slot] = gametic;

    // Separation, that is, orientation/stereo.
    //  range is: 1 - 256
    seperation += 1;

    // Per left/right channel.
    //  x^2 seperation,
    //  adjust volume properly.
    leftvol =
	volume - ((volume*seperation*seperation) >> 16); ///(256*256);
    seperation = seperation - 257;
    rightvol =
	volume - ((volume*seperation*seperation) >> 16);	

    // Sanity check, clamp volume.
    if (rightvol < 0 || rightvol > 127)
	I_Error("rightvol out of bounds");
    
    if (leftvol < 0 || leftvol > 127)
	I_Error("leftvol out of bounds");
    
    // Get the proper lookup table piece
    //  for this volume level???
    channelleftvol_lookup[slot] = &vol_lookup[leftvol*256];
    channelrightvol_lookup[slot] = &vol_lookup[rightvol*256];

    // Preserve sound SFX id,
    //  e.g. for avoiding duplicates of chainsaw.
    channelids[slot] = sfxid;

    // You tell me.
    return rc;
}





//
// SFX API
// Note: this was called by S_Init.
// However, whatever they did in the
// old DPMS based DOS version, this
// were simply dummies in the Linux
// version.
// See soundserver initdata().
//
void I_SetChannels()
{
  // Init internal lookups (raw data, mixing buffer, channels).
  // This function sets up internal lookups used during
  //  the mixing process. 
  int		i;
  int		j;
    
  int*	steptablemid = steptable + 128;
  
  // Okay, reset internal mixing channels to zero.
  /*for (i=0; i<NUM_CHANNELS; i++)
  {
    channels[i] = 0;
  }*/

  // This table provides step widths for pitch parameters.
  // I fail to see that this is currently used.
  for (i=-128 ; i<128 ; i++)
    steptablemid[i] = (int)(pow(2.0, (i/64.0))*65536.0);
  
  
  // Generates volume lookup tables
  //  which also turn the unsigned samples
  //  into signed samples.
  for (i=0 ; i<128 ; i++)
    for (j=0 ; j<256 ; j++)
      vol_lookup[i*256+j] = (i*(j-128)*256)/127;
}	

 
void I_SetSfxVolume(int volume)
{
  // Identical to DOS.
  // Basically, this should propagate
  //  the menu/config file setting
  //  to the state variable used in
  //  the mixing.
  snd_SfxVolume = volume;
}



//
// Retrieve the raw data lump index
//  for a given SFX name.
//
int I_GetSfxLumpNum(sfxinfo_t* sfx)
{
    char namebuf[9];
    sprintf(namebuf, "ds%s", sfx->name);
    return W_GetNumForName(namebuf);
}

//
// Starting a sound means adding it
//  to the current list of active sounds
//  in the internal channels.
// As the SFX info struct contains
//  e.g. a pointer to the raw data,
//  it is ignored.
// As our sound handling does not handle
//  priority, it is ignored.
// Pitching (that is, increased speed of playback)
//  is set, but currently not used by mixing.
//
int
I_StartSound
( int		id,
  int		vol,
  int		sep,
  int		pitch,
  int		priority )
{

  // UNUSED
  priority = 0;

  // Counted only so the log can say whether the game ever asks for a sound
  // effect at all.  Silence with this at zero is not an audio problem.
  sfxStarts++;

  //
  // The volume arrives on the menu's scale and has to be put on the mixer's.
  //
  // Two scales are in play and the released source does not reconcile them.
  // snd_SfxVolume is 0 to 15 -- that is what the slider shows, what the
  // config file holds, and what S_AdjustSoundParams does its distance
  // arithmetic in.  vol_lookup below is built for 0 to 127.  Everything
  // between S_StartSound and here leaves the number alone, so a full volume
  // setting of 15 reached the mixer as 15 of 127 and the game played at an
  // eighth of the level it was mixed for.
  //
  // id knew: d_main.c and m_menu.c both call the sound volume functions with
  // "/* *8 */" commented out beside the argument.  Under linuxdoom that was
  // correct, because the volume went to a separate sound server that scaled
  // it again at the other end.  This port mixes in process, so nothing ever
  // scales it and the eight is simply missing.
  //
  // It cost a long hunt.  Eight bit output is what made it silence rather
  // than quietness: at 15 of 127 the samples spanned about a dozen of the
  // 256 levels the card has, which is inaudible over a speaker.  The log
  // said so precisely once it was asked -- a peak of 1548 out of 32767,
  // which is exactly (6 * 128 * 256) / 127, the number a volume of 8 with a
  // little stereo separation produces and no other volume does.
  //
  // Applied here rather than at the call sites so that the menu, the config
  // file and S_AdjustSoundParams all keep working in the units they were
  // written for.  The clamp is defensive: 15 * 8 is 120, and addsfx calls
  // I_Error above 127.
  //
  vol *= 8;

  if (vol > 127)
      vol = 127;
  else if (vol < 0)
      vol = 0;

  // addsfx writes the channel table that DART's thread is reading, so the
  // two are kept apart here.  Without it the mixer can pick up a channel
  // whose sample pointer has been stored but whose end pointer has not, and
  // walk off the end of the lump -- intermittently, and only when a sound
  // starts on a busy frame, which is the worst kind of fault to go looking
  // for later.
  if (sndMutex != NULLHANDLE)
      DosRequestMutexSem (sndMutex, SEM_INDEFINITE_WAIT);

  // Returns a handle (not used).
  id = addsfx( id, vol, steptable[pitch], sep );

  if (sndMutex != NULLHANDLE)
      DosReleaseMutexSem (sndMutex);

  return id;
}



void I_StopSound (int handle)
{
  // You need the handle returned by StartSound.
  // Would be looping all channels,
  //  tracking down the handle,
  //  an setting the channel to zero.
  
  // UNUSED.
  handle = 0;
}


int I_SoundIsPlaying(int handle)
{
    // Ouch.
    return gametic < handle;
}




//
// This function loops all active (internal) sound
//  channels, retrieves a given number of samples
//  from the raw sound data, modifies it according
//  to the current (internal) channel parameters,
//  mixes the per channel samples into the global
//  mixbuffer, clamping it to the allowed range,
//  and sets up everything for transferring the
//  contents of the mixbuffer to the (two)
//  hardware channels (left and right, that is).
//
// This function currently supports only 16bit.
//
// Renamed from I_UpdateSound for the OS/2 port.  The body below is the
// original mixer, untouched; what changed is who calls it.  On Linux the
// game loop did, once per rendered frame.  Here DART's callback does, once
// per buffer it has finished playing -- see I_DartEvent.
static void I_MixBuffer( void )
{
#ifdef SNDINTR
  // Debug. Count buffer misses with interrupt.
  static int misses = 0;
#endif

  
  // Mix current sound data.
  // Data, from raw sound, for right and left.
  register unsigned int	sample;
  register int		dl;
  register int		dr;
  
  // Pointers in global mixbuffer, left, right, end.
  signed short*		leftout;
  signed short*		rightout;
  signed short*		leftend;
  // Step in mixbuffer, left and right, thus two.
  int				step;

  // Mixing channel index.
  int				chan;
    
    // Left and right channel
    //  are in global mixbuffer, alternating.
    leftout = mixbuffer;
    rightout = mixbuffer+1;
    step = 2;

    // Determine end, for left channel only
    //  (right channel is implicit).
    leftend = mixbuffer + SAMPLECOUNT*step;

    // Mix sounds into the mixing buffer.
    // Loop over step*SAMPLECOUNT,
    //  that is 512 values for two channels.
    while (leftout != leftend)
    {
	// Reset left/right value. 
	dl = 0;
	dr = 0;

	// Love thy L2 chache - made this a loop.
	// Now more channels could be set at compile time
	//  as well. Thus loop those  channels.
	for ( chan = 0; chan < NUM_CHANNELS; chan++ )
	{
	    // Check channel, if active.
	    if (channels[ chan ])
	    {
		// Get the raw data from the channel. 
		sample = *channels[ chan ];
		// Add left and right part
		//  for this channel (sound)
		//  to the current data.
		// Adjust volume accordingly.
		dl += channelleftvol_lookup[ chan ][sample];
		dr += channelrightvol_lookup[ chan ][sample];
		// Increment index ???
		channelstepremainder[ chan ] += channelstep[ chan ];
		// MSB is next sample???
		channels[ chan ] += channelstepremainder[ chan ] >> 16;
		// Limit to LSB???
		channelstepremainder[ chan ] &= 65536-1;

		// Check whether we are done.
		if (channels[ chan ] >= channelsend[ chan ])
		    channels[ chan ] = 0;
	    }
	}
	
	// Clamp to range. Left hardware channel.
	// Has been char instead of short.
	// if (dl > 127) *leftout = 127;
	// else if (dl < -128) *leftout = -128;
	// else *leftout = dl;

	if (dl > 0x7fff)
	    *leftout = 0x7fff;
	else if (dl < -0x8000)
	    *leftout = -0x8000;
	else
	    *leftout = dl;

	// Same for right hardware channel.
	if (dr > 0x7fff)
	    *rightout = 0x7fff;
	else if (dr < -0x8000)
	    *rightout = -0x8000;
	else
	    *rightout = dr;

	// Increment current pointers in mixbuffer.
	leftout += step;
	rightout += step;
    }

#ifdef SNDINTR
    // Debug check.
    if ( flag )
    {
      misses += flag;
      flag = 0;
    }
    
    if ( misses > 10 )
    {
      fprintf( stderr, "I_SoundUpdate: missed 10 buffer writes\n");
      misses = 0;
    }
    
    // Increment flag for update.
    flag++;
#endif
}


//
// I_UpdateSound / I_SubmitSound
//
// On Linux these were the game loop's way of driving the sound: mix a
// buffer, then write it to /dev/dsp, once per rendered frame.  It only
// worked because the write blocked when the driver's buffer was full -- and
// that blocking is what held the frame rate down to whatever the sound card
// was draining at.
//
// Under DART the flow runs the other way.  MMPM/2 calls I_DartEvent when it
// has finished with a buffer, and the mixing happens there, on DART's
// thread, at exactly the rate the hardware consumes it.  The game loop has
// nothing left to do.
//
// They are left here empty rather than deleted: D_DoomLoop still calls both,
// and removing them would mean changing the engine to suit the platform,
// which is the wrong way round.
//
// The playlist path is the same story with one difference: it reports
// through the window rather than a thread of its own, so it cannot be
// started until there is a window.  This is the first place in the game loop
// that is certain to run after I_InitGraphics, which makes it the place.
//
void
I_UpdateSound(void)
{
}

void
I_SubmitSound(void)
{
    if (playlistPending)
    {
	playlistPending = false;

	if (I_InitPlaylist ())
	    printf ("I_InitSound: MMPM/2 playlist, %i Hz %i bit %s,"
		    " %i x %i byte blocks,\n"
		    "             refilled %s (%i tics per block)%s.\n",
		    dartRate, dartBits,
		    dartChannels == 2 ? "stereo" : "mono",
		    NUM_PL_BUFFERS, (int)plBlockSize,
		    plChunked	  ? "a pass at a time"
		    : plUseMessages ? "on the driver's messages"
				    : "on the clock",
		    plBlockTics,
		    M_CheckParm ("-plexit")
			? ", ONE PASS ONLY (-plexit)" : "");
	else
	    printf ("I_InitSound: the playlist interface failed too;"
		    " running silent.\n");
    }

    //
    // A playlist that started but has never asked for a block is playing the
    // same few hundred milliseconds over and over.  One second is several
    // times a block, so by then the device has either asked or it never will
    // -- and if it never will, the clock takes over.
    //
    if (playlistUp && !plMessages && !plTimed
	&& I_GetTime () - plStartTime > TICRATE)
    {
	plTimed = true;

	if (!plComplained)
	{
	    plComplained = true;
	    printf ("I_InitSound: this driver does not report playlist"
		    " progress;\n"
		    "             refilling on the clock instead"
		    " (%i tics per block).\n", plBlockTics);
	}
    }

    if (playlistUp && plTimed)
	I_PlaylistTimedRefill ();

    //
    // Chunked playback: keep the passes coming.
    //
    // The device is asked to say when a pass has finished, and normally does.
    // The clock is only here so that one lost notification does not end the
    // sound for the rest of the session -- it waits out the whole pass and a
    // margin on top before assuming anything, because guessing early would
    // mean writing buffers under a device that is still reading them, which
    // is precisely the thing that used to stop the machine.
    //
    if (playlistUp && plChunked)
    {
	if (plPlaying && I_GetTime () > plPassEnd + PL_PASS_MARGIN)
	    plPlaying = false;

	if (!plPlaying)
	    I_StartPass ();
    }

    // One tic's worth of mixing allowance, restored once a tic.  Whatever the
    // device asked for beyond it during the last one is gone, and better gone
    // than mixed.
    plBurst = 0;

    //
    // A few seconds in, and every quarter minute after that, say what the
    // sound side is actually doing.
    //
    // Four numbers, and between them they say where the silence is:
    //
    //	starts	how many times the game has asked for a sound effect.  Zero
    //		means nothing further down matters -- the fault would be in
    //		S_StartSound or the volume, not in any of this.
    //	vol	the sfx volume in force, 0 to 15.  Zero explains everything.
    //	peak	the loudest sample the mixer has produced, of 32767.  Zero
    //		alongside a non-zero starts means the channel table or the
    //		sample data is empty.
    //	blocks	how many blocks the device has taken.  Climbing, alongside a
    //		non-zero peak, means the samples left here in good order and
    //		went missing somewhere past this program.
    //
    // Repeated because one line cannot tell a number that is stuck from a
    // number that had not started yet, and a run has to be worth the trip.
    //
    if (playlistUp && plReports < PL_MAX_REPORTS
	&& I_GetTime () - plStartTime > plNextReport)
    {
	plReports++;
	plNextReport += 15*TICRATE;

	I_ReportSound ();
    }
}



void
I_UpdateSoundParams
( int	handle,
  int	vol,
  int	sep,
  int	pitch)
{
  // I fail too see that this is used.
  // Would be using the handle to identify
  //  on which channel the sound might be active,
  //  and resetting the channel parameters.

  // UNUSED.
  handle = vol = sep = pitch = 0;
}




void I_ShutdownSound(void)
{
  MCI_GENERIC_PARMS	gp;

  // Reachable twice over: once from I_Quit on the way out, and once from
  // I_Error if something went wrong before that.  Everything below is
  // guarded by the flag that says it was set up in the first place.
  if (!pMciSendCommand)
      return;

  if (dartUp)
  {
      // Stop before anything is freed.  MCI_STOP is what brings back the
      // buffers DART still has in flight, and deallocating them while its
      // thread is reading one is not a fault that shows up straight away.
      memset (&gp, 0, sizeof(gp));
      gp.hwndCallback = NULLHANDLE;
      pMciSendCommand (mciDeviceID, MCI_STOP, MCI_WAIT, (PVOID)&gp, 0);

      pMciSendCommand (mciDeviceID, MCI_MIXSETUP,
		       MCI_WAIT | MCI_MIXSETUP_DEINIT,
		       (PVOID)&mciMixSetup, 0);

      dartUp = false;
  }

  if (playlistUp)
  {
      // A last tally on the way out, so that even a run cut short by a quit
      // in the first three seconds leaves something in the log.
      I_ReportSound ();

      // Same rule as DART: stop the device before the memory it is reading
      // goes away.  The playlist branches for ever, so nothing else will
      // ever stop it.
      memset (&gp, 0, sizeof(gp));
      gp.hwndCallback = NULLHANDLE;
      pMciSendCommand (mciDeviceID, MCI_STOP, MCI_WAIT, (PVOID)&gp, 0);

      playlistUp = false;
  }

  if (dartBuffers)
  {
      pMciSendCommand (mciDeviceID, MCI_BUFFER,
		       MCI_WAIT | MCI_DEALLOCATE_MEMORY,
		       (PVOID)&mciBufferParms, 0);
      dartBuffers = false;
  }

  if (mciDeviceID)
  {
      memset (&gp, 0, sizeof(gp));
      gp.hwndCallback = NULLHANDLE;
      pMciSendCommand (mciDeviceID, MCI_CLOSE, MCI_WAIT, (PVOID)&gp, 0);
      mciDeviceID = 0;
  }

  //
  // The playlist memory goes back only now, AFTER the device has been closed.
  //
  // Stopping a device is not the same as it having finished with the memory
  // it was given.  The driver holds the playlist and every block in it until
  // the close, so freeing them in between hands back tiled memory that a 16
  // bit driver still has an alias to -- and this one is not the sort to check.
  //
  I_FreePlaylistMemory ();

  if (sndMutex != NULLHANDLE)
  {
      DosCloseMutexSem (sndMutex);
      sndMutex = NULLHANDLE;
  }

  // MDM.DLL itself is deliberately left loaded.  It belongs to
  // I_OS2_MciEntry, and the music may still be using it -- I_ShutdownSound
  // runs before I_ShutdownMusic on the way out, and I_Error can call this
  // one on its own.
  pMciSendCommand = NULL;

  // Done.
  return;
}






//
// The formats offered, best first.
//
// Native rate before anything resampled, stereo before mono, sixteen bits
// before eight -- so the first entry a device accepts is the best it can do.
// The rate multipliers are whole numbers because DOOM's 11025 divides into
// both 22050 and 44100 exactly; a driver advertising only 8000 is out of
// luck, and would be badly served by the fractional resampling it needed.
//
static const struct { int bits, channels, ratemul; } sndFormats[] =
{
    { 16, 2, 1 }, { 16, 1, 1 }, {  8, 2, 1 }, {  8, 1, 1 },
    { 16, 2, 2 }, { 16, 1, 2 }, {  8, 2, 2 }, {  8, 1, 2 },
    { 16, 2, 4 }, { 16, 1, 4 }, {  8, 2, 4 }, {  8, 1, 4 }
};

#define NUM_SND_FORMATS	(sizeof(sndFormats)/sizeof(sndFormats[0]))


//
// I_TakeFormat
//
// Adopt one of the above, and work out how many bytes one mix becomes in it.
//
static void I_TakeFormat (int f)
{
    dartBits	 = sndFormats[f].bits;
    dartChannels = sndFormats[f].channels;
    dartRateMul	 = sndFormats[f].ratemul;
    dartRate	 = SAMPLERATE * dartRateMul;

    dartChunk	 = (ULONG)SAMPLECOUNT * (dartBits / 8)
		 * dartChannels * dartRateMul;
}


//
// I_FormatName
//
static char* I_FormatName (int f)
{
    static char	buf[64];

    sprintf (buf, "%i Hz %i bit %s",
	     SAMPLERATE * sndFormats[f].ratemul, sndFormats[f].bits,
	     sndFormats[f].channels == 2 ? "stereo" : "mono");

    return buf;
}


//
// I_MciErrName
//
// The few MCI errors worth telling apart here.  The distinction that matters
// is between "this device cannot do that format" and "this device cannot do
// this at all": the first means keep offering, the second means the whole
// interface is missing and something else must be tried.
//
static char* I_MciErrName (ULONG rc)
{
    static char	buf[64];
    ULONG	raw = rc;

    //
    // mciSendCommand does not return a bare error code.  The low word holds
    // the error; the high word holds the device id it happened to, so the
    // caller can tell which device answered when several are open.  Comparing
    // the whole thing against MCIERR_ANYTHING therefore never matches -- the
    // 70541 that started this was 0x1138D, which is device 1 and error 5005,
    // MCIERR_UNRECOGNIZED_COMMAND.
    //
    rc &= 0xFFFF;

    switch (rc)
    {
      case MCIERR_UNRECOGNIZED_COMMAND:	return "command not implemented";
      case MCIERR_UNSUPPORTED_FUNCTION:	return "unsupported function";
      case MCIERR_INVALID_FLAG:		return "invalid flag";
      case MCIERR_MISSING_FLAG:		return "missing flag";
      case MCIERR_UNSUPP_FORMAT_TAG:	return "unsupported format tag";
      case MCIERR_UNSUPP_SAMPLESPERSEC:	return "unsupported sample rate";
      case MCIERR_UNSUPP_BITSPERSAMPLE:	return "unsupported sample size";
      case MCIERR_UNSUPP_CHANNELS:	return "unsupported channel count";
      case MCIERR_INVALID_DEVICE_NAME:	return "no such device";
      case MCIERR_DEVICE_LOCKED:	return "device busy";
      default:				break;
    }

    sprintf (buf, "MCI error %u", (unsigned)rc);
    return buf;
}


//
// I_ConvertMix
//
// Copy one finished mix into a DART buffer, in whatever format the card
// agreed to take.
//
// The mixer's output is SAMPLECOUNT frames of 16 bit signed stereo, left
// sample first.  Going to mono is an average rather than a sum, because the
// two channels of a DOOM mix are largely the same sound at different volumes
// and summing them would clip everything loud.  Going to eight bits is a
// shift and a bias: the PCM the Sound Blaster generation wants is unsigned,
// with silence at 128 rather than at 0.
//
static void I_ConvertMix (byte* out)
{
    signed short*	in = mixbuffer;
    int			i;
    int			r;

    if (dartBits == 16 && dartChannels == 2 && dartRateMul == 1)
    {
	// What DOOM already produces.
	memcpy (out, mixbuffer, DART_CHUNK);
	return;
    }

    if (dartBits == 16)
    {
	signed short*	o = (signed short *)out;

	for (i = 0; i < SAMPLECOUNT; i++, in += 2)
	{
	    signed short	l = in[0];
	    signed short	rr = in[1];

	    if (dartChannels == 1)
		l = rr = (signed short)(((int)in[0] + (int)in[1]) >> 1);

	    for (r = 0; r < dartRateMul; r++)
	    {
		*o++ = l;
		if (dartChannels == 2)
		    *o++ = rr;
	    }
	}
    }
    else
    {
	for (i = 0; i < SAMPLECOUNT; i++, in += 2)
	{
	    byte	l, rr;

	    if (dartChannels == 1)
		l = rr = (byte)(((((int)in[0] + (int)in[1]) >> 1) >> 8) + 128);
	    else
	    {
		l  = (byte)((in[0] >> 8) + 128);
		rr = (byte)((in[1] >> 8) + 128);
	    }

	    for (r = 0; r < dartRateMul; r++)
	    {
		*out++ = l;
		if (dartChannels == 2)
		    *out++ = rr;
	    }
	}
    }
}


//
// I_DartEvent
//
// MMPM/2 calls this on a thread of its own every time one of our buffers has
// finished playing.  Mix the next samples straight into it and hand it back.
//
// Two rules govern what may go in here.  It runs at a raised priority, so it
// has to be short -- the mixing loop is a few thousand multiply-adds and
// nothing else, no allocation and no file access.  And it runs on a thread
// the game knows nothing about, so everything it touches that the game also
// touches goes under sndMutex.
//
static LONG APIENTRY
I_DartEvent
( ULONG			ulStatus,
  PMCI_MIX_BUFFER	pBuffer,
  ULONG			ulFlags )
{
  // UNUSED.
  ulStatus = 0;

  // A bitmask test, not an equality test.  MMPM/2 is free to report other
  // bits alongside MIX_WRITE_COMPLETE, and if it does, an == comparison
  // silently stops refilling buffers -- which is silence, with no error
  // anywhere to say why.
  if ((ulFlags & MIX_WRITE_COMPLETE) && dartUp && pBuffer)
  {
      ULONG	done = 0;

      if (sndMutex != NULLHANDLE)
	  DosRequestMutexSem (sndMutex, SEM_INDEFINITE_WAIT);

      // The mixer produces one fixed-size chunk per call, and DART's buffer
      // is not obliged to be that size, so fill it a whole chunk at a time.
      // Anything left over at the end is not played: a partial chunk would
      // mean throwing away samples the mixer had already stepped past, and
      // the sound would run fast.
      while (done + dartChunk <= dartBufSize)
      {
	  I_MixBuffer ();
	  I_ConvertMix ((byte *)pBuffer->pBuffer + done);
	  done += dartChunk;
      }

      if (sndMutex != NULLHANDLE)
	  DosReleaseMutexSem (sndMutex);

      pBuffer->ulBufferLength = done;

      // Straight back into the queue.  This is what keeps the stream alive:
      // stop returning buffers and the sound simply ends.
      mciMixSetup.pmixWrite (mciMixSetup.ulMixHandle, pBuffer, 1);
  }

  return TRUE;
}


//
// I_InitDart
//
// Open the waveaudio device, set the mixer up for what DOOM produces, get
// the buffers, and start the stream.  Any step may fail -- there may be no
// sound card, or another program may have the device -- and failing is not
// fatal: DOOM runs silent.
//
static boolean I_InitDart (void)
{
  MCI_AMP_OPEN_PARMS	amp;
  int			i;

  // The one door into MMPM/2, shared with the music (I_OS2MUS.C) so that
  // MDM.DLL is loaded once however many of the two end up being used.
  // NULL means this machine has no multimedia support installed.
  pMciSendCommand = I_OS2_MciEntry ();
  if (!pMciSendCommand)
  {
      puts ("I_InitSound: MDM.DLL not present - no MMPM/2 on this machine.");
      return false;
  }

  if (DosCreateMutexSem (NULL, &sndMutex, 0, FALSE) != NO_ERROR)
      sndMutex = NULLHANDLE;

  //
  // Open the device, shareable, so that DOOM does not take the sound card
  // away from everything else on the desktop for as long as it runs.
  //
  memset (&amp, 0, sizeof(amp));
  amp.pszDeviceType = (PSZ) MAKEULONG (MCI_DEVTYPE_WAVEFORM_AUDIO, 0);

  if (pMciSendCommand (0, MCI_OPEN,
		       MCI_WAIT | MCI_OPEN_TYPE_ID | MCI_OPEN_SHAREABLE,
		       (PVOID)&amp, 0) != MCIERR_SUCCESS)
  {
      puts ("I_InitSound: MCI_OPEN of the waveaudio device failed.");
      return false;
  }

  mciDeviceID = amp.usDeviceID;

  //
  // Tell the mixer what it is going to be fed, and where to call back.
  //
  // Offered best first.  MCI_MIXSETUP will not negotiate -- it either takes
  // what it is given or fails, without saying what it would have preferred --
  // so the only way to find out what a card can do is to ask it, in order,
  // until one answer sticks.  An eight bit mono Sound Blaster of the MCA era
  // gets all the way to the last entry, and plays.
  //
  {
      int	f;
      ULONG	rc   = MCIERR_UNSUPPORTED_FUNCTION;
      boolean	got  = false;

      for (f = 0; f < NUM_SND_FORMATS && !got; f++)
      {
	  memset (&mciMixSetup, 0, sizeof(mciMixSetup));
	  mciMixSetup.ulBitsPerSample	= sndFormats[f].bits;
	  mciMixSetup.ulFormatTag	= MCI_WAVE_FORMAT_PCM;
	  mciMixSetup.ulSamplesPerSec	= SAMPLERATE * sndFormats[f].ratemul;
	  mciMixSetup.ulChannels	= sndFormats[f].channels;
	  mciMixSetup.ulFormatMode	= MCI_PLAY;
	  mciMixSetup.ulDeviceType	= MCI_DEVTYPE_WAVEFORM_AUDIO;
	  mciMixSetup.pmixEvent		= I_DartEvent;

	  rc = pMciSendCommand (mciDeviceID, MCI_MIXSETUP,
				MCI_WAIT | MCI_MIXSETUP_INIT,
				(PVOID)&mciMixSetup, 0);

	  if (rc == MCIERR_SUCCESS)
	  {
	      I_TakeFormat (f);
	      got = true;
	  }
	  else
	      printf ("I_InitSound: DART refused %s (%s).\n",
		      I_FormatName (f), I_MciErrName (rc));
      }

      if (!got)
      {
	  //
	  // Every format refused in the same way means the formats were never
	  // the question.  MCI_MIXSETUP is the DART interface itself, so a
	  // driver that does not implement it says no to all of them --
	  // MCIERR_UNSUPPORTED_FUNCTION being the honest version of that
	  // answer, though not every driver troubles to give it.
	  //
	  printf ("I_InitSound: this driver has no DART (last error %s);\n"
		  "             trying the older playlist interface.\n",
		  I_MciErrName (rc));
	  return false;
      }
  }

  //
  // Ask DART for the buffers.  It allocates them itself -- they have to be
  // memory the audio driver can reach -- and it is free to hand back a
  // different size from the one requested, which is why dartBufSize is read
  // back rather than assumed.
  //
  memset (&mciBufferParms, 0, sizeof(mciBufferParms));
  mciBufferParms.ulStructLength	= sizeof(mciBufferParms);
  mciBufferParms.ulNumBuffers	= NUM_DART_BUFFERS;
  mciBufferParms.ulBufferSize	= dartChunk;
  mciBufferParms.pBufList	= mciBuffers;

  if (pMciSendCommand (mciDeviceID, MCI_BUFFER,
		       MCI_WAIT | MCI_ALLOCATE_MEMORY,
		       (PVOID)&mciBufferParms, 0) != MCIERR_SUCCESS)
  {
      puts ("I_InitSound: MCI_BUFFER could not allocate the DART buffers.");
      return false;
  }

  dartBuffers = true;
  dartBufSize = mciBufferParms.ulBufferSize;

  // A buffer too small to hold one whole mix would play nothing but silence
  // -- see the loop in I_DartEvent -- so say so now rather than leave the
  // user wondering why the game is mute.
  if (dartBufSize < dartChunk)
  {
      puts ("I_InitSound: DART gave back a buffer too small for one mix.");
      return false;
  }

  //
  // Prime every buffer with silence and set the stream running.  Queueing
  // them all at once is what starts it; from here on the callback keeps it
  // fed.
  //
  for (i = 0; i < NUM_DART_BUFFERS; i++)
  {
      mciBuffers[i].ulStructLength = sizeof(MCI_MIX_BUFFER);
      mciBuffers[i].ulBufferLength = dartBufSize;
      mciBuffers[i].ulFlags	   = 0;
      mciBuffers[i].ulUserParm	   = 0;

      // Silence is not always zero.  Sixteen bit PCM is signed and silence
      // sits at 0, but eight bit PCM is unsigned and silence sits at 128 --
      // filling an eight bit buffer with zeroes primes the stream with a
      // full-scale negative offset, which the speaker reports as a thump.
      memset (mciBuffers[i].pBuffer, dartBits == 8 ? 128 : 0, dartBufSize);
  }

  dartUp = true;

  if (mciMixSetup.pmixWrite (mciMixSetup.ulMixHandle,
			     mciBuffers, NUM_DART_BUFFERS) != MCIERR_SUCCESS)
  {
      dartUp = false;
      puts ("I_InitSound: pmixWrite would not start the stream.");
      return false;
  }

  return true;
}


//
// I_BuildPlaylist
//
// Write the little program the audio device will run: play each buffer in
// turn, say so after each one, and branch back to the beginning.
//
// The message after each block is what makes this work as a stream.  The
// playlist processor only moves past a DATA operation when the device has
// finished with that block, so the message means "buffer n is free" -- which
// is precisely when it can be filled again.
//
//
// I_FreePlaylistMemory
//
// Both tiled allocations, together, from wherever the setup gave up.
//
static void I_FreePlaylistMemory (void)
{
    if (plMemory)
    {
	DosFreeMem (plMemory);
	plMemory = NULL;
    }

    if (plListMem)
    {
	DosFreeMem (plListMem);
	plListMem = NULL;
	playlist  = NULL;
    }
}


//
// PlBlockSize
//
// One block: at least PL_MIN_BLOCK bytes, and a whole number of mixes, since
// the mixer only ever produces complete ones.
//
static ULONG PlBlockSize (ULONG chunk)
{
    ULONG	n = (PL_MIN_BLOCK + chunk - 1) / chunk;
    ULONG	size = n * chunk;

    // Never past what was allocated for a block.
    while (size > (ULONG)(SAMPLECOUNT * 2 * 2 * 4))
	size -= chunk;

    return size;
}


//
// The MESSAGE_OPERATION elements are optional, and now off by default.
//
// They are how the driver says a block is free, and on paper that is the
// right way round: the device knows where it has got to, and we can only
// estimate.  In practice they are the one part of this arrangement that runs
// at interrupt time inside a driver written in 1992, and they post into a PM
// queue that nothing drains while a level is loading.  Three hard locks came
// out of that path in a week.  Each fix addressed something real -- a missing
// 16:16 alias, a restart that raced the DMA, a mixing backlog that pegged the
// CPU -- and the machine still stopped, because none of them touched the
// mechanism itself.
//
// Nothing actually needs it.  A device consumes a block in a time fixed by
// its size and its byte rate, and the byte rate is no longer a guess: the log
// measured it at 44100 against the 44100 we feed, a ratio of one.  The clock
// therefore says where the playlist has got to as well as the driver would
// have, and unlike the driver the clock cannot wedge anything.
//
// So the playlist is now DATA and BRANCH alone: the device is handed sound
// and a loop, and is never asked to talk back.  -plmessages puts them back.
//
static void I_BuildPlaylist (ULONG chunk)
{
    int		i;
    int		e = 0;

    for (i = 0; i < NUM_PL_BUFFERS; i++)
    {
	playlist[e*4+0] = DATA_OPERATION;
	playlist[e*4+1] = (ULONG)plBuffer[i];
	playlist[e*4+2] = chunk;
	playlist[e*4+3] = 0;
	e++;

	if (plUseMessages)
	{
	    playlist[e*4+0] = MESSAGE_OPERATION;
	    playlist[e*4+1] = (ULONG)i;
	    playlist[e*4+2] = 0;
	    playlist[e*4+3] = 0;
	    e++;
	}
    }

    //
    // Round and round: BRANCH back to the first element for ever.  A stream
    // that stops cannot easily be started again, whereas one that repeats the
    // last thing it had merely stutters while the game catches up.
    //
    // -plexit ends the list instead, and exists only to answer one question.
    // Everything optional has already been taken out of this playlist and the
    // machine still stops; what remains is DATA, which is the driver reading
    // our buffers, and BRANCH, which is the driver walking the list.  A list
    // that ends separates them.  If a single pass plays -- a third of a
    // second of sound and then silence, with the machine still standing --
    // then DATA is fine and BRANCH is what kills it, and a LOOP_OPERATION is
    // worth trying next.  If it locks even so, the driver cannot be given a
    // buffer to DMA out of at all, and this interface is finished on this
    // hardware.
    //
    // Either answer is worth having and neither needs a second run.
    //
    // It was run, and the list that ends survived.  So the list now always
    // ends, and the looping form is what has to be asked for.
    //
    if (plChunked || M_CheckParm ("-plexit"))
    {
	playlist[e*4+0] = EXIT_OPERATION;
	playlist[e*4+1] = 0;
    }
    else
    {
	playlist[e*4+0] = BRANCH_OPERATION;
	playlist[e*4+1] = 0;
    }

    playlist[e*4+2] = 0;
    playlist[e*4+3] = 0;
}


//
// I_InitPlaylist
//
// The fallback for drivers without DART.  Called after the window exists,
// because the playlist reports its progress by posting to it.
//
static boolean I_InitPlaylist (void)
{
    MCI_OPEN_PARMS	op;
    MCI_WAVE_SET_PARMS	sp;
    MCI_PLAY_PARMS	pp;
    ULONG		rc = 0;
    ULONG		plOpenFlags = 0;
    int			f;
    int			i;
    boolean		got = false;

    //
    // Fetch the entry point again rather than trusting the cached one.
    //
    // I_InitSound calls I_ShutdownSound when DART is refused, to give back
    // the device that did open before the failure -- and the last thing
    // I_ShutdownSound does is set pMciSendCommand to NULL.  So by the time
    // this runs the cached pointer is always gone, and the test below used to
    // fail here and return without a word, which looked exactly like the
    // playlist having been tried and refused.  MDM.DLL is still loaded;
    // I_OS2_MciEntry hands back the same address it did the first time.
    //
    if (!pMciSendCommand)
	pMciSendCommand = I_OS2_MciEntry ();

    if (!pMciSendCommand)
    {
	puts ("I_InitSound: MDM.DLL is not available for the playlist.");
	return false;
    }

    if (os2_hwndClient == NULLHANDLE)
    {
	puts ("I_InitSound: no window yet; the playlist cannot be started.");
	return false;
    }

    // Whether to ask the driver to report its progress.  See I_BuildPlaylist.
    plUseMessages = M_CheckParm ("-plmessages") != 0;

    // One pass at a time unless a single looping list is asked for.  -plexit
    // is the bare diagnostic: it plays one pass and never starts another, so
    // it must not be re-armed either.
    plChunked = !M_CheckParm ("-plbranch") && !M_CheckParm ("-plexit");

    //
    // One block of memory for all the buffers, at the largest size any
    // offered format could need -- sixteen bits, two channels, four times
    // the rate.  Sized once so that the format can be renegotiated below
    // without allocating again.
    //
    // OBJ_TILE on both -- see the note at the top of this file's playlist
    // section.  The driver is 16 bit and needs a 16:16 alias for anything it
    // is given the address of, and that includes the playlist itself, not
    // only the sound it points at.
    if (DosAllocMem (&plMemory,
		     NUM_PL_BUFFERS * SAMPLECOUNT * 2 * 2 * 4,
		     PAG_COMMIT | PAG_READ | PAG_WRITE | OBJ_TILE) != NO_ERROR)
    {
	puts ("I_InitSound: no memory for the playlist buffers.");
	return false;
    }

    if (DosAllocMem (&plListMem, 4096,
		     PAG_COMMIT | PAG_READ | PAG_WRITE | OBJ_TILE) != NO_ERROR)
    {
	puts ("I_InitSound: no memory for the playlist itself.");
	I_FreePlaylistMemory ();
	return false;
    }

    playlist = (ULONG *)plListMem;

    for (i = 0; i < NUM_PL_BUFFERS; i++)
	plBuffer[i] = (byte *)plMemory + i * (SAMPLECOUNT * 2 * 2 * 4);

    //
    // First: which flags does this driver want a playlist opened with?
    //
    // Whether MCI_OPEN_ELEMENT belongs alongside MCI_OPEN_PLAYLIST is not
    // something the documentation is clear about, and whether an old driver
    // will share its one audio path is not something to assume.  Rather than
    // guess, all four combinations are tried once, with the first format's
    // playlist, and whichever opens is used for the negotiation proper.  It
    // costs four calls on a machine where this path is needed at all.
    //
    {
	static const ULONG	flagsets[4] =
	{
	    MCI_WAIT | MCI_OPEN_TYPE_ID | MCI_OPEN_PLAYLIST
		     | MCI_OPEN_ELEMENT | MCI_OPEN_SHAREABLE,
	    MCI_WAIT | MCI_OPEN_TYPE_ID | MCI_OPEN_PLAYLIST
		     | MCI_OPEN_ELEMENT,
	    MCI_WAIT | MCI_OPEN_TYPE_ID | MCI_OPEN_PLAYLIST
		     | MCI_OPEN_SHAREABLE,
	    MCI_WAIT | MCI_OPEN_TYPE_ID | MCI_OPEN_PLAYLIST
	};

	int	s;

	I_BuildPlaylist ((ULONG)SAMPLECOUNT * (sndFormats[0].bits / 8)
			 * sndFormats[0].channels * sndFormats[0].ratemul);

	for (s = 0; s < 4; s++)
	{
	    memset (&op, 0, sizeof(op));
	    op.hwndCallback   = os2_hwndClient;
	    op.pszDeviceType  = (PSZ) MAKEULONG (MCI_DEVTYPE_WAVEFORM_AUDIO, 0);
	    op.pszElementName = (PSZ) playlist;

	    rc = pMciSendCommand (0, MCI_OPEN, flagsets[s], (PVOID)&op, 0);

	    if (rc == MCIERR_SUCCESS)
	    {
		MCI_GENERIC_PARMS	gp;

		plOpenFlags = flagsets[s];

		memset (&gp, 0, sizeof(gp));
		pMciSendCommand (op.usDeviceID, MCI_CLOSE, MCI_WAIT,
				 (PVOID)&gp, 0);
		break;
	    }

	    printf ("I_InitSound: playlist open, flags %lu: %s.\n",
		    (unsigned long)flagsets[s], I_MciErrName (rc));
	}

	if (!plOpenFlags)
	{
	    printf ("I_InitSound: this driver will not open a playlist"
		    " either.\n");
	    I_FreePlaylistMemory ();
	    return false;
	}
    }

    //
    // The same formats, offered the same way.  The device is opened afresh
    // for each attempt because the playlist -- which carries the block length
    // and so depends on the format -- is handed over at open time and cannot
    // be changed afterwards.
    //
    for (f = 0; f < NUM_SND_FORMATS && !got; f++)
    {
	ULONG	chunk = (ULONG)SAMPLECOUNT * (sndFormats[f].bits / 8)
		      * sndFormats[f].channels * sndFormats[f].ratemul;

	plBlockSize = PlBlockSize (chunk);
	I_BuildPlaylist (plBlockSize);

	memset (&op, 0, sizeof(op));
	op.hwndCallback	  = os2_hwndClient;
	op.pszDeviceType  = (PSZ) MAKEULONG (MCI_DEVTYPE_WAVEFORM_AUDIO, 0);
	op.pszElementName = (PSZ) playlist;

	rc = pMciSendCommand (0, MCI_OPEN, plOpenFlags, (PVOID)&op, 0);

	if (rc != MCIERR_SUCCESS)
	{
	    printf ("I_InitSound: playlist open refused for %s (%s).\n",
		    I_FormatName (f), I_MciErrName (rc));
	    continue;
	}

	mciDeviceID = op.usDeviceID;

	// Now say what the blocks contain.  A device that cannot play this
	// format says so here rather than at the open.
	memset (&sp, 0, sizeof(sp));
	sp.usFormatTag	    = MCI_WAVE_FORMAT_PCM;
	sp.usChannels	    = (USHORT)sndFormats[f].channels;
	sp.ulSamplesPerSec  = (ULONG)(SAMPLERATE * sndFormats[f].ratemul);
	sp.usBitsPerSample  = (USHORT)sndFormats[f].bits;
	sp.usBlockAlign	    = (USHORT)(sndFormats[f].channels
				       * (sndFormats[f].bits / 8));
	sp.ulAvgBytesPerSec = sp.ulSamplesPerSec * sp.usBlockAlign;

	rc = pMciSendCommand (mciDeviceID, MCI_SET,
			      MCI_WAIT | MCI_WAVE_SET_FORMATTAG
			      | MCI_WAVE_SET_CHANNELS
			      | MCI_WAVE_SET_SAMPLESPERSEC
			      | MCI_WAVE_SET_BITSPERSAMPLE
			      | MCI_WAVE_SET_BLOCKALIGN
			      | MCI_WAVE_SET_AVGBYTESPERSEC,
			      (PVOID)&sp, 0);

	if (rc != MCIERR_SUCCESS)
	{
	    MCI_GENERIC_PARMS	gp;

	    printf ("I_InitSound: playlist device will not take %s (%s).\n",
		    I_FormatName (f), I_MciErrName (rc));

	    memset (&gp, 0, sizeof(gp));
	    pMciSendCommand (mciDeviceID, MCI_CLOSE, MCI_WAIT,
			     (PVOID)&gp, 0);
	    mciDeviceID = 0;
	    continue;
	}

	I_TakeFormat (f);
	got = true;
    }

    //
    // Last resort: open it and say nothing about the format.
    //
    // A driver old enough not to know MCI_MIXSETUP does not know MCI_SET's
    // wave parameters either, so it plays whatever its own default is and
    // never tells us what that is.  The bet made here has to be right,
    // because a wrong one is not merely out of tune: the device eats the
    // blocks at its rate rather than ours, and the mixing that has to keep up
    // with it is the most expensive thing this program does.
    //
    // The first bet was 11025 Hz eight bit mono, on the grounds that it is
    // all a Sound Blaster of this generation does.  The logs said otherwise,
    // twice and identically: 36 blocks consumed in the first three seconds
    // where 4096 bytes of eight bit mono would be eight.  Four and a half
    // times too fast, and four of that is exactly the ratio between one byte
    // per frame and four -- sixteen bit stereo.  The remaining half is the
    // four primed blocks, which come back immediately.  So the device's own
    // default is sixteen bit stereo, and every byte we sent it was being read
    // as half of a sixteen bit sample.
    //
    // That also explains the silence rather than the noise one would expect
    // from a format error.  Eight bit PCM is unsigned, silence is 128, so a
    // quiet passage is a run of 0x80 bytes -- which read as sixteen bit
    // signed is a constant -32640.  A constant is a DC offset, and a DC
    // offset moves a speaker cone once and then holds it there.  It is not a
    // wrong sound; it is no sound.
    //
    // Guessing wrong here is expensive enough to be worth overriding by hand,
    // hence -sndformat.  The index is into sndFormats above: 0 is 11025 Hz
    // 16 bit stereo, 3 is 11025 Hz 8 bit mono, and I_FormatName prints them.
    //
    if (!got)
    {
	int	assumed = 0;			// 11025 Hz, 16 bit, stereo
	int	p	= M_CheckParm ("-sndformat");

	if (p && p < myargc - 1)
	{
	    int	n = atoi (myargv[p+1]);

	    if (n >= 0 && n < (int)NUM_SND_FORMATS)
		assumed = n;
	    else
		printf ("I_InitSound: -sndformat %i is not one of the %i"
			" formats; ignored.\n", n, (int)NUM_SND_FORMATS);
	}

	I_TakeFormat (assumed);

	plBlockSize = PlBlockSize (dartChunk);
	I_BuildPlaylist (plBlockSize);

	memset (&op, 0, sizeof(op));
	op.hwndCallback	  = os2_hwndClient;
	op.pszDeviceType  = (PSZ) MAKEULONG (MCI_DEVTYPE_WAVEFORM_AUDIO, 0);
	op.pszElementName = (PSZ) playlist;

	rc = pMciSendCommand (0, MCI_OPEN, plOpenFlags, (PVOID)&op, 0);

	if (rc == MCIERR_SUCCESS)
	{
	    mciDeviceID = op.usDeviceID;
	    got = true;

	    printf ("I_InitSound: no format would be set; assuming the"
		    " driver's own\n"
		    "             %s.  -sndformat N overrides.\n",
		    I_FormatName (assumed));
	}
    }

    if (!got)
    {
	I_FreePlaylistMemory ();
	return false;
    }

    //
    // Turn the volume up before playing anything.
    //
    // The device's own idea of its level is whatever the last program to
    // touch it left behind, and on a driver that has just refused every
    // format flag it is not safe to assume that is anything in particular.
    // A failure here is ignored: plenty of drivers do not implement it, and
    // the ones that do not are generally the ones already at full level.
    //
    {
	MCI_SET_PARMS	vp;

	memset (&vp, 0, sizeof(vp));
	vp.ulLevel = 100;
	vp.ulAudio = MCI_SET_AUDIO_ALL;

	pMciSendCommand (mciDeviceID, MCI_SET,
			 MCI_WAIT | MCI_SET_AUDIO | MCI_SET_VOLUME,
			 (PVOID)&vp, 0);
    }

    // Prime every block with real sound rather than silence.  If the device
    // never asks for more -- the failure this is most likely to meet -- then
    // what it plays over and over is at least audible, which distinguishes
    // "the stream is not running" from "the stream is running and starved".
    for (i = 0; i < NUM_PL_BUFFERS; i++)
    {
	ULONG	off;

	for (off = 0; off + dartChunk <= plBlockSize; off += dartChunk)
	{
	    I_MixBuffer ();
	    I_ConvertMix (plBuffer[i] + off);
	}
    }

    plNext	 = 0;
    plMessages	 = 0;

    // With no MESSAGE elements in the list the device will never say a word,
    // so the clock takes over from the first tic rather than after a second
    // of waiting to be told something that is not coming.
    //
    // Chunked playback needs neither: each pass fills every block before it
    // starts, and nothing is refilled while one is running.
    plComplained = !plUseMessages;
    plTimed	 = !plUseMessages && !plChunked;

    plPlaying	 = false;
    plPasses	 = 0;

    plNextFill	 = NUM_PL_BUFFERS;		// 0..N-1 were just primed
    plPeak	 = 0;
    plReports	 = 0;
    plBurst	 = 0;
    plDropped	 = 0;
    plNextReport = 3*TICRATE;
    plStartTime	 = I_GetTime ();
    plLastMsg	 = plStartTime;

    // How long the device takes to eat one block, and a whole pass of them.
    {
	int	bps = dartRate * (dartBits / 8) * dartChannels;

	plBlockTics = bps > 0 ? (int)((plBlockSize * TICRATE) / bps) : 1;

	if (plBlockTics < 1)
	    plBlockTics = 1;

	plPassTics = bps > 0
		   ? (int)((plBlockSize * NUM_PL_BUFFERS * TICRATE) / bps)
		   : 1;

	if (plPassTics < 1)
	    plPassTics = 1;
    }

    memset (&pp, 0, sizeof(pp));
    pp.hwndCallback = os2_hwndClient;

    // MCI_NOTIFY on a chunked pass, so the device says when it has finished
    // and the next one can go in.  This is MM_MCINOTIFY for the play as a
    // whole -- one message per pass -- and not the per-block MESSAGE elements
    // that were taken out of the list; the two are unrelated mechanisms.
    rc = pMciSendCommand (mciDeviceID, MCI_PLAY,
			  plChunked ? MCI_NOTIFY : 0, (PVOID)&pp, 0);

    if (rc != MCIERR_SUCCESS)
    {
	printf ("I_InitSound: the playlist would not start (%s).\n",
		I_MciErrName (rc));
	return false;
    }

    if (plChunked)
    {
	plPlaying = true;
	plPassEnd = I_GetTime () + plPassTics;
	plPasses  = 1;
    }

    playlistUp = true;
    return true;
}


//
// I_StartPass
//
// Fill every block and play the list once through.
//
// Safe to call only when no pass is in flight.  That is the whole point of
// the arrangement: the device is not reading any of these buffers when they
// are written, because it has told us -- or the clock has -- that it finished
// with all of them.  Writing a buffer under a playing device is what took the
// machine down the first time this was tried.
//
static boolean I_StartPass (void)
{
    MCI_PLAY_PARMS	pp;
    ULONG		rc;
    int			i;

    for (i = 0; i < NUM_PL_BUFFERS; i++)
	I_FillBlock ((ULONG)i);

    memset (&pp, 0, sizeof(pp));
    pp.hwndCallback = os2_hwndClient;

    rc = pMciSendCommand (mciDeviceID, MCI_PLAY, MCI_NOTIFY, (PVOID)&pp, 0);

    if (rc != MCIERR_SUCCESS)
    {
	// Stop rather than keep asking.  A device that has started refusing
	// MCI_PLAY will refuse it thirty-five times a second otherwise, and
	// each refusal is a call into the driver that just failed.
	printf ("I_InitSound: pass %i would not start (%s); stopping the"
		" playlist.\n", plPasses + 1, I_MciErrName (rc));
	playlistUp = false;
	return false;
    }

    plPlaying = true;
    plPassEnd = I_GetTime () + plPassTics;
    plPasses++;

    return true;
}


//
// I_OS2_PlaylistPlayDone
//
// MM_MCINOTIFY: a play command has finished.  The window procedure sends
// every one of these here first, because the music sequencer is on the same
// window and the two are told apart only by which device the message names.
//
// Returns true if it belonged to the playlist and has been dealt with.
//
boolean I_OS2_PlaylistPlayDone (ULONG deviceID)
{
    if (!playlistUp || !plChunked || !mciDeviceID)
	return false;

    if (deviceID != mciDeviceID)
	return false;

    plPlaying = false;
    return true;
}


//
// I_FillBlock
//
// One block, filled with as many mixes as it holds.
//
static void I_FillBlock (ULONG which)
{
    ULONG	off;

    for (off = 0; off + dartChunk <= plBlockSize; off += dartChunk)
    {
	int	i;

	I_MixBuffer ();

	// How loud was that?  If the answer is always zero then the mixer is
	// producing silence and no amount of work on the driver will help;
	// if it is not, the samples are reaching the card and being lost
	// somewhere beyond this program.  Nothing else distinguishes the two.
	for (i = 0; i < SAMPLECOUNT*2; i++)
	{
	    int	v = mixbuffer[i];

	    if (v < 0)
		v = -v;

	    if (v > plPeak)
		plPeak = v;
	}

	I_ConvertMix (plBuffer[which] + off);
    }
}


//
// I_ReportSound
//
// One line saying where the silence is.  See I_SubmitSound for what the
// numbers mean.
//
// It also names the pistol sample, because that one is always present in
// every WAD id ever shipped -- getsfx falls back to it for anything missing
// -- so a length of zero there means the sound lumps did not load at all and
// nothing else in the line is worth reading.
//
static void I_ReportSound (void)
{
    printf ("I_InitSound: starts %i, vol %i, peak %i of 32767, blocks %i,"
	    " dspistol %i bytes.\n",
	    sfxStarts, snd_SfxVolume, plPeak,
	    plUseMessages ? (int)plMessages : (int)plNextFill,
	    lengths[sfx_pistol]);

    //
    // Chunked playback lives or dies on whether the passes keep coming, so
    // say how many there have been and how many there should have been.
    //
    // Equal, or nearly, and the device is finishing each pass and being given
    // the next: the sound is running as well as this arrangement allows.  Far
    // short, and the passes are not being re-armed -- the notification is not
    // arriving and the clock is carrying it alone, which is audible as long
    // gaps.  Stuck at one, and the very first pass never ended.
    //
    if (plChunked)
    {
	int	elapsed = I_GetTime () - plStartTime;
	int	expected = plPassTics > 0 ? elapsed / plPassTics : 0;

	printf ("I_InitSound: %i passes played, about %i expected"
		" (%i tics each).\n", plPasses, expected, plPassTics);
	return;
    }

    // On the clock there is nothing to measure the device against -- the
    // blocks above are the ones we decided to send, not ones it asked for --
    // so the byte rate below would only be this program agreeing with itself.
    if (!plUseMessages)
	return;

    //
    // And what the device's real format is, which it would not say.
    //
    // The blocks it has taken and the time it took to take them give its byte
    // rate directly, and the byte rate names the format: against the rate we
    // are feeding it for, the ratio is 1 if the assumption was right, 4 if
    // eight bit mono was offered to a sixteen bit stereo device, and so on
    // through the table.  Whatever -sndformat is set to, this line says
    // whether it was the right choice.
    //
    {
	int	elapsed = I_GetTime () - plStartTime;
	long	feeding = (long)dartRate * (dartBits / 8) * dartChannels;
	long	taking;

	if (elapsed > 0)
	{
	    taking = (long)((plMessages * plBlockSize * TICRATE) / elapsed);

	    printf ("I_InitSound: feeding %ld bytes/sec, device taking about"
		    " %ld.\n", feeding, taking);

	    if (plDropped)
		printf ("I_InitSound: %i block requests dropped to keep up.\n",
			plDropped);
	}
    }
}


//
// I_PlaylistTimedRefill
//
// Keep the blocks ahead of where the clock says the device has reached.
//
// Filled up to NUM_PL_BUFFERS-1 blocks ahead, which with four blocks means
// the one being written is the one just played -- the furthest from the
// needle it is possible to be.
//
static void I_PlaylistTimedRefill (void)
{
    int		elapsed = I_GetTime () - plStartTime;
    ULONG	playing;
    ULONG	target;
    int		guard   = 0;

    if (elapsed < 0)
	return;

    playing = (ULONG)(elapsed / plBlockTics);
    target  = playing + NUM_PL_BUFFERS - 1;

    // Never more than a full set in one go: if the game has been away for a
    // while -- a level load -- there is no value in mixing audio for a
    // stretch of time that has already been and gone.
    while (plNextFill <= target && guard++ < NUM_PL_BUFFERS)
    {
	I_FillBlock (plNextFill % NUM_PL_BUFFERS);
	plNextFill++;
    }

    if (plNextFill < playing)
	plNextFill = playing;
}


//
// I_OS2_PlaylistNotify
//
// MM_MCIPLAYLISTMESSAGE: the device has finished with one of the blocks.
//
// Unlike DART's callback this arrives on the game's own thread, by way of the
// window procedure, so nothing here needs guarding against the mixer running
// at the same time -- it cannot.
//
void I_OS2_PlaylistNotify (ULONG which)
{
    if (!playlistUp)
	return;

    // The blocks are reported in order, so our own count is as good as the
    // number that came with the message -- and better if the message ever
    // carries something unexpected.
    if (which >= NUM_PL_BUFFERS)
	which = plNext;

    plNext = (which + 1) % NUM_PL_BUFFERS;

    // The first one is worth saying out loud: it is the proof that the device
    // is actually consuming what it is given, which nothing else can show.
    if (!plMessages)
	printf ("I_InitSound: the playlist is feeding"
		" (first block requested).\n");

    plMessages++;
    plLastMsg = I_GetTime ();

    // The driver does talk to us, so the clock is not needed -- but only if we
    // asked it to.  A stray message from a list with no MESSAGE elements in it
    // must not be allowed to switch off the thing that is actually doing the
    // work.
    if (plUseMessages)
	plTimed = false;
    else
	return;

    //
    // The safety valve.
    //
    // Mixing is by a wide margin the most expensive thing this program does
    // that is not drawing, and every one of these messages asks for a block
    // of it on the game's own thread.  So the cost of this function is set
    // entirely by how fast the device asks -- which is the device's business,
    // not ours, and on a driver that will not say what format it is playing
    // it is not even something we know.
    //
    // Get that wrong on the slow side and the game merely stutters.  Get it
    // wrong on the fast side and it cannot win: every frame arrives with more
    // messages queued than the last frame managed to clear, the queue grows
    // without bound, and the machine stops responding well enough to need the
    // three-finger salute.  That happened twice, and both times it was read
    // as a driver fault because it looked exactly like one.
    //
    // A backlog is also worthless.  Blocks queued behind the one the device
    // is reading now will be overwritten before they are ever played, so
    // mixing audio for them buys nothing at all.  Past a full set of buffers,
    // the messages are counted and dropped.
    //
    if (plBurst++ >= NUM_PL_BUFFERS)
    {
	plDropped++;
	return;
    }

    I_FillBlock (which);
}


void
I_InitSound()
{
  int i;

  //
  // Initialize external data (all sounds) at start, keep static.
  //
  // This happens before DART is started, not after.  The moment the stream
  // begins, the callback can run and reach into the channel table, and it
  // has no business doing that while the lumps it points at are still being
  // loaded.
  //
  printf ("I_InitSound: ");

  for (i=1 ; i<NUMSFX ; i++)
  {
    // Alias? Example is the chaingun sound linked to pistol.
    if (!S_sfx[i].link)
    {
      // Load data from WAD file.
      S_sfx[i].data = getsfx( S_sfx[i].name, &lengths[i] );
    }
    else
    {
      // Previously loaded already?
      S_sfx[i].data = S_sfx[i].link->data;
      lengths[i] = lengths[(S_sfx[i].link - S_sfx)/sizeof(sfxinfo_t)];
    }
  }

  printf ("pre-cached all sound data\n");

  // Now initialize mixbuffer with zero.
  for ( i = 0; i< MIXBUFFERSIZE; i++ )
    mixbuffer[i] = 0;

  //
  // And now the device.
  //
  if (M_CheckParm ("-nosound"))
      printf ("I_InitSound: disabled by -nosound.\n");
  else if (I_InitDart ())
      printf ("I_InitSound: MMPM/2 DART, %i Hz %i bit %s,"
	      " %i x %i byte buffers.\n",
	      dartRate, dartBits, dartChannels == 2 ? "stereo" : "mono",
	      NUM_DART_BUFFERS, (int)dartBufSize);
  else
  {
      // Give back whatever did open before the failure.
      I_ShutdownSound ();

      //
      // The playlist path is off unless it is asked for, and that is a
      // retreat rather than a design.
      //
      // It ran automatically for a while.  On the Sound Blaster MCV driver it
      // stops the machine dead -- not a trap in DOOM, the whole system, with
      // CHKDSK to follow -- and it did so four times across a week of fixes
      // that each corrected something genuinely wrong and none of which was
      // the cause.  Memory with no 16:16 alias, a restart racing the DMA
      // engine, a mixing backlog that pegged a 486, an eight bit mono
      // assumption on a sixteen bit stereo device: all real, all fixed, all
      // beside the point.  With the format measured and correct, the volume
      // right, the mixer producing a healthy peak and the playlist reduced to
      // DATA and BRANCH with nothing optional left in it, the machine still
      // stopped -- and ran perfectly the moment -noplaylist was passed.
      //
      // What is left is that this driver cannot stream.  It has refused every
      // interface it was offered: no DART, no MCI_SET of any wave parameter,
      // no MCI_OPEN_ELEMENT.  A playlist is the last of them, and the
      // evidence says it accepts one and then cannot honour it.
      //
      // Defaulting to a path that reliably takes the machine down, on the
      // chance that some other driver handles it better, is not a trade worth
      // making: the cost of being wrong is a reboot and a file system check,
      // and the benefit is sound effects.  So it waits to be asked.  Music
      // goes through the sequencer and is unaffected either way.
      //
      // -playlist turns it on.  -noplaylist is still accepted, and still
      // means off, so that anyone who put it in a WPS object or a batch file
      // during all this keeps getting what they asked for.
      //
      // -plexit implies -playlist.  It shapes a playlist and is meaningless
      // without one, so a switch that quietly did nothing on its own would be
      // worse than useless here: the run would look like a result -- no lock,
      // no sound -- when nothing had been tested at all.  It did exactly that
      // once.
      //
      if ((!M_CheckParm ("-playlist") && !M_CheckParm ("-plexit"))
	  || M_CheckParm ("-noplaylist"))
	  printf ("I_InitSound: no DART; the playlist fallback is off"
		  " (-playlist enables it).\n");
      else
      {
	  //
	  // The playlist needs the game window to report its progress to, and
	  // that does not exist yet -- I_InitGraphics runs after this.  So it
	  // is only marked as wanted here, and started by the first call to
	  // I_SubmitSound, which by definition happens once the game loop and
	  // its window are up.
	  //
	  printf ("I_InitSound: no DART; the playlist interface will be"
		  " tried once the window is up.\n");

	  //
	  // Said out loud, every time, because of what it has cost.
	  //
	  // On the driver this was developed against, this path has stopped
	  // the machine five times -- once past the reach of Ctrl-Alt-Del,
	  // needing the power switch, with the file system damaged after it.
	  // Anyone reading a log from a machine that did not come back should
	  // find the warning in it rather than have to be told.
	  //
	  printf ("I_InitSound: WARNING -- on a driver that cannot stream,"
		  " this can lock the\n"
		  "             machine hard enough to need the power"
		  " switch.  -noplaylist\n"
		  "             avoids it; music is unaffected either"
		  " way.\n");

	  playlistPending = true;
      }
  }

  // Finished initialization.
  printf ("I_InitSound: sound module ready\n");
}




