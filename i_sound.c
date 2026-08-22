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
typedef ULONG (APIENTRY *PFNMCISENDCOMMAND)(USHORT, USHORT, ULONG,
					    PVOID, USHORT);

static HMODULE			hmodMdm		= NULLHANDLE;
static PFNMCISENDCOMMAND	pMciSendCommand	= NULL;

// How many buffers DART keeps in flight.  Each one holds whole mixes of
// SAMPLECOUNT stereo frames -- about 46 ms at 11025 Hz -- so three of them
// is something over a tenth of a second of slack.  Fewer buffers means less
// delay between pulling the trigger and hearing it; more means fewer breaks
// in the sound when the machine is busy.
#define NUM_DART_BUFFERS	3

// One mix, in bytes: SAMPLECOUNT frames, two channels, two bytes each.
#define DART_CHUNK		(SAMPLECOUNT*BUFMUL)

static USHORT			mciDeviceID = 0;
static MCI_MIXSETUP_PARMS	mciMixSetup;
static MCI_BUFFER_PARMS		mciBufferParms;
static MCI_MIX_BUFFER		mciBuffers[NUM_DART_BUFFERS];

// The size DART actually gave us, which need not be the size asked for.
static ULONG			dartBufSize = 0;

static boolean			dartUp	    = false;	// stream is running
static boolean			dartBuffers = false;	// memory is allocated

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

// MUSIC API - dummy. Some code from DOS version.
void I_SetMusicVolume(int volume)
{
  // Internal state variable.
  snd_MusicVolume = volume;
  // Now set volume on output device.
  // Whatever( snd_MusciVolume );
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
void
I_UpdateSound(void)
{
}

void
I_SubmitSound(void)
{
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

  if (sndMutex != NULLHANDLE)
  {
      DosCloseMutexSem (sndMutex);
      sndMutex = NULLHANDLE;
  }

  DosFreeModule (hmodMdm);
  hmodMdm = NULLHANDLE;
  pMciSendCommand = NULL;

  // Done.
  return;
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

  if (ulFlags == MIX_WRITE_COMPLETE && dartUp && pBuffer)
  {
      ULONG	done = 0;

      if (sndMutex != NULLHANDLE)
	  DosRequestMutexSem (sndMutex, SEM_INDEFINITE_WAIT);

      // The mixer produces one fixed-size chunk per call, and DART's buffer
      // is not obliged to be that size, so fill it a whole chunk at a time.
      // Anything left over at the end is not played: a partial chunk would
      // mean throwing away samples the mixer had already stepped past, and
      // the sound would run fast.
      while (done + DART_CHUNK <= dartBufSize)
      {
	  I_MixBuffer ();
	  memcpy ((byte *)pBuffer->pBuffer + done, mixbuffer, DART_CHUNK);
	  done += DART_CHUNK;
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
  UCHAR			failed[CCHMAXPATH];
  int			i;

  if (DosLoadModule (failed, sizeof(failed), (PSZ)"MDM", &hmodMdm)
      != NO_ERROR)
  {
      hmodMdm = NULLHANDLE;
      return false;
  }

  if (DosQueryProcAddr (hmodMdm, 0, (PSZ)"mciSendCommand",
			(PFN *)&pMciSendCommand) != NO_ERROR)
  {
      DosFreeModule (hmodMdm);
      hmodMdm = NULLHANDLE;
      pMciSendCommand = NULL;
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
      return false;

  mciDeviceID = amp.usDeviceID;

  //
  // Tell the mixer what it is going to be fed, and where to call back.
  //
  memset (&mciMixSetup, 0, sizeof(mciMixSetup));
  mciMixSetup.ulBitsPerSample	= 16;
  mciMixSetup.ulFormatTag	= MCI_WAVE_FORMAT_PCM;
  mciMixSetup.ulSamplesPerSec	= SAMPLERATE;
  mciMixSetup.ulChannels	= 2;
  mciMixSetup.ulFormatMode	= MCI_PLAY;
  mciMixSetup.ulDeviceType	= MCI_DEVTYPE_WAVEFORM_AUDIO;
  mciMixSetup.pmixEvent		= I_DartEvent;

  if (pMciSendCommand (mciDeviceID, MCI_MIXSETUP,
		       MCI_WAIT | MCI_MIXSETUP_INIT,
		       (PVOID)&mciMixSetup, 0) != MCIERR_SUCCESS)
      return false;

  //
  // Ask DART for the buffers.  It allocates them itself -- they have to be
  // memory the audio driver can reach -- and it is free to hand back a
  // different size from the one requested, which is why dartBufSize is read
  // back rather than assumed.
  //
  memset (&mciBufferParms, 0, sizeof(mciBufferParms));
  mciBufferParms.ulStructLength	= sizeof(mciBufferParms);
  mciBufferParms.ulNumBuffers	= NUM_DART_BUFFERS;
  mciBufferParms.ulBufferSize	= DART_CHUNK;
  mciBufferParms.pBufList	= mciBuffers;

  if (pMciSendCommand (mciDeviceID, MCI_BUFFER,
		       MCI_WAIT | MCI_ALLOCATE_MEMORY,
		       (PVOID)&mciBufferParms, 0) != MCIERR_SUCCESS)
      return false;

  dartBuffers = true;
  dartBufSize = mciBufferParms.ulBufferSize;

  // A buffer too small to hold one whole mix would play nothing but silence
  // -- see the loop in I_DartEvent -- so say so now rather than leave the
  // user wondering why the game is mute.
  if (dartBufSize < DART_CHUNK)
      return false;

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
      memset (mciBuffers[i].pBuffer, 0, dartBufSize);
  }

  dartUp = true;

  if (mciMixSetup.pmixWrite (mciMixSetup.ulMixHandle,
			     mciBuffers, NUM_DART_BUFFERS) != MCIERR_SUCCESS)
  {
      dartUp = false;
      return false;
  }

  return true;
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
  printf ("I_InitSound: ");

  if (M_CheckParm ("-nosound"))
      printf ("disabled by -nosound.\n");
  else if (I_InitDart ())
      printf ("MMPM/2 DART, %i Hz 16 bit stereo, %i x %i byte buffers.\n",
	      SAMPLERATE, NUM_DART_BUFFERS, (int)dartBufSize);
  else
  {
      printf ("no MMPM/2 waveaudio device available, running silent.\n");

      // Give back whatever did open before the failure.
      I_ShutdownSound ();
  }

  // Finished initialization.
  printf ("I_InitSound: sound module ready\n");
}




//
// MUSIC API.
// Still no music done.
// Remains. Dummies.
//
void I_InitMusic(void)		{ }
void I_ShutdownMusic(void)	{ }

static int	looping=0;
static int	musicdies=-1;

void I_PlaySong(int handle, int looping)
{
  // UNUSED.
  handle = looping = 0;
  musicdies = gametic + TICRATE*30;
}

void I_PauseSong (int handle)
{
  // UNUSED.
  handle = 0;
}

void I_ResumeSong (int handle)
{
  // UNUSED.
  handle = 0;
}

void I_StopSong(int handle)
{
  // UNUSED.
  handle = 0;
  
  looping = 0;
  musicdies = 0;
}

void I_UnRegisterSong(int handle)
{
  // UNUSED.
  handle = 0;
}

int I_RegisterSong(void* data)
{
  // UNUSED.
  data = NULL;
  
  return 1;
}

// Is the song playing?
int I_QrySongPlaying(int handle)
{
  // UNUSED.
  handle = 0;
  return looping || musicdies > gametic;
}

