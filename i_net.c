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
//	Network stuff -- OS/2 version, over IBM TCP/IP.
//
//	The packet format and the protocol are untouched, so this talks to
//	linuxxdoom and to any other port that kept them.
//
//	Three things differ from Berkeley sockets, and all three bite:
//
//	  - sock_init() has to be called before anything else works.
//	  - a socket is closed with soclose(), not close(); the descriptors
//	    are not file descriptors and close() does not know them.
//	  - errno says nothing about sockets.  sock_errno() does.
//
//	The socket calls are resolved by name out of SO32DLL and TCP32DLL at
//	run time rather than imported.  A DOOM.EXE that imported them would
//	not load at all on a machine with no TCP/IP installed -- which is a
//	poor way to treat somebody who only wanted a single player game.
//
//-----------------------------------------------------------------------------

static const char
rcsid[] = "$Id: i_net.c,v 1.2 1997/02/03 22:45:10 b1 Exp $";

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "os2doom.h"

#include <types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <nerrno.h>

#include "i_system.h"
#include "d_event.h"
#include "d_net.h"
#include "m_argv.h"

#include "doomstat.h"

#include "i_net.h"


//
// The socket API, resolved at run time.
//
// SO32DLL holds the sockets themselves; TCP32DLL holds the resolver.  Both
// come with IBM TCP/IP, and both are absent on a machine that never had it
// installed.
//
typedef int  (_System *PFNSOCKINIT)  (void);
typedef int  (_System *PFNSOCKERRNO) (void);
typedef int  (_System *PFNSOCKET)    (int, int, int);
typedef int  (_System *PFNBIND)      (int, struct sockaddr *, int);
typedef int  (_System *PFNSENDTO)    (int, char *, int, int,
				      struct sockaddr *, int);
typedef int  (_System *PFNRECVFROM)  (int, char *, int, int,
				      struct sockaddr *, int *);
typedef int  (_System *PFNSOCLOSE)   (int);
typedef int  (_System *PFNIOCTL)     (int, unsigned long, char *);
typedef int  (_System *PFNGETHOSTNAME)   (char *, int);
typedef struct hostent * (_System *PFNGETHOSTBYNAME) (char *);
typedef unsigned long    (_System *PFNINETADDR)      (char *);

static HMODULE		hmodSo32   = NULLHANDLE;
static HMODULE		hmodTcp32  = NULLHANDLE;

static PFNSOCKINIT	p_sock_init;
static PFNSOCKERRNO	p_sock_errno;
static PFNSOCKET	p_socket;
static PFNBIND		p_bind;
static PFNSENDTO	p_sendto;
static PFNRECVFROM	p_recvfrom;
static PFNSOCLOSE	p_soclose;
static PFNIOCTL		p_ioctl;
static PFNGETHOSTNAME	p_gethostname;
static PFNGETHOSTBYNAME	p_gethostbyname;
static PFNINETADDR	p_inet_addr;

static boolean		tcpipReady = false;


//
// I_LoadTcpip
//
// Both libraries, then every entry point.  All or nothing: a half-resolved
// socket API is worse than none, because the failure would arrive in the
// middle of a game rather than at startup.
//
static boolean I_LoadTcpip (void)
{
    UCHAR	failed[CCHMAXPATH];

    if (tcpipReady)
	return true;

    if (DosLoadModule (failed, sizeof(failed), (PSZ)"SO32DLL", &hmodSo32)
	!= NO_ERROR)
    {
	hmodSo32 = NULLHANDLE;
	return false;
    }

    if (DosLoadModule (failed, sizeof(failed), (PSZ)"TCP32DLL", &hmodTcp32)
	!= NO_ERROR)
    {
	DosFreeModule (hmodSo32);
	hmodSo32 = NULLHANDLE;
	hmodTcp32 = NULLHANDLE;
	return false;
    }

    if (DosQueryProcAddr (hmodSo32, 0, (PSZ)"sock_init",
			  (PFN *)&p_sock_init) != NO_ERROR
     || DosQueryProcAddr (hmodSo32, 0, (PSZ)"sock_errno",
			  (PFN *)&p_sock_errno) != NO_ERROR
     || DosQueryProcAddr (hmodSo32, 0, (PSZ)"socket",
			  (PFN *)&p_socket) != NO_ERROR
     || DosQueryProcAddr (hmodSo32, 0, (PSZ)"bind",
			  (PFN *)&p_bind) != NO_ERROR
     || DosQueryProcAddr (hmodSo32, 0, (PSZ)"sendto",
			  (PFN *)&p_sendto) != NO_ERROR
     || DosQueryProcAddr (hmodSo32, 0, (PSZ)"recvfrom",
			  (PFN *)&p_recvfrom) != NO_ERROR
     || DosQueryProcAddr (hmodSo32, 0, (PSZ)"soclose",
			  (PFN *)&p_soclose) != NO_ERROR
     || DosQueryProcAddr (hmodSo32, 0, (PSZ)"ioctl",
			  (PFN *)&p_ioctl) != NO_ERROR
     || DosQueryProcAddr (hmodTcp32, 0, (PSZ)"gethostname",
			  (PFN *)&p_gethostname) != NO_ERROR
     || DosQueryProcAddr (hmodTcp32, 0, (PSZ)"gethostbyname",
			  (PFN *)&p_gethostbyname) != NO_ERROR
     || DosQueryProcAddr (hmodTcp32, 0, (PSZ)"inet_addr",
			  (PFN *)&p_inet_addr) != NO_ERROR)
    {
	DosFreeModule (hmodTcp32);
	DosFreeModule (hmodSo32);
	hmodSo32 = hmodTcp32 = NULLHANDLE;
	return false;
    }

    // Nothing in the sockets library works until this has been called.  It
    // is the one call with no Berkeley equivalent, and forgetting it makes
    // every socket() fail for no visible reason.
    if (p_sock_init() != 0)
    {
	DosFreeModule (hmodTcp32);
	DosFreeModule (hmodSo32);
	hmodSo32 = hmodTcp32 = NULLHANDLE;
	return false;
    }

    tcpipReady = true;
    return true;
}


void	NetSend (void);
boolean NetListen (void);


//
// NETWORKING
//

//
// The byte swapping is done here rather than by the library, which is what
// the Linux original did too -- "for some odd reason", as its comment had
// it.  On OS/2 there is a very concrete reason: htons and htonl are real
// functions living in TCPIP32.DLL, and calling them would put an import on
// that DLL into DOOM.EXE.  The whole point of resolving the sockets by name
// further up is that this executable loads and plays on a machine with no
// TCP/IP installed at all, and one call to htons would undo it.
//
// The results are identical: both reverse the bytes, and OS/2 runs on
// little endian machines only, so the packets match what every other DOOM
// puts on the wire.
//
#undef ntohl
#undef ntohs
#undef htonl
#undef htons

#define ntohl(x) \
        ((unsigned long int)((((unsigned long int)(x) & 0x000000ffU) << 24) | \
                             (((unsigned long int)(x) & 0x0000ff00U) <<  8) | \
                             (((unsigned long int)(x) & 0x00ff0000U) >>  8) | \
                             (((unsigned long int)(x) & 0xff000000U) >> 24)))

#define ntohs(x) \
        ((unsigned short int)((((unsigned short int)(x) & 0x00ff) << 8) | \
                              (((unsigned short int)(x) & 0xff00) >> 8)))

#define htonl(x) ntohl(x)
#define htons(x) ntohs(x)


// 5029.  The Linux original wrote this as IPPORT_USERRESERVED + 0x1d, which
// came to 5029 there because Linux defines IPPORT_USERRESERVED as 5000.  The
// OS/2 headers define it as 65535, so the same expression would land past the
// end of the port range -- and, more to the point, would not be the port any
// other DOOM is listening on.  The number is what matters for talking to
// them, so it is written out.
int	DOOMPORT =	5029;

int			sendsocket;
int			insocket;

struct	sockaddr_in	sendaddress[MAXNETNODES];

void	(*netget) (void);
void	(*netsend) (void);


//
// UDPsocket
//
int UDPsocket (void)
{
    int	s;

    // allocate a socket
    s = p_socket (PF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s<0)
	I_Error ("can't create socket: sock_errno %d", p_sock_errno());

    return s;
}

//
// BindToLocalPort
//
void
BindToLocalPort
( int	s,
  int	port )
{
    int			v;
    struct sockaddr_in	address;

    memset (&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = port;

    v = p_bind (s, (struct sockaddr *)&address, sizeof(address));
    if (v == -1)
	I_Error ("BindToPort: bind: sock_errno %d", p_sock_errno());
}


//
// PacketSend
//
void PacketSend (void)
{
    int		c;
    doomdata_t	sw;

    // byte swap
    sw.checksum = htonl(netbuffer->checksum);
    sw.player = netbuffer->player;
    sw.retransmitfrom = netbuffer->retransmitfrom;
    sw.starttic = netbuffer->starttic;
    sw.numtics = netbuffer->numtics;
    for (c=0 ; c< netbuffer->numtics ; c++)
    {
	sw.cmds[c].forwardmove = netbuffer->cmds[c].forwardmove;
	sw.cmds[c].sidemove = netbuffer->cmds[c].sidemove;
	sw.cmds[c].angleturn = htons(netbuffer->cmds[c].angleturn);
	sw.cmds[c].consistancy = htons(netbuffer->cmds[c].consistancy);
	sw.cmds[c].chatchar = netbuffer->cmds[c].chatchar;
	sw.cmds[c].buttons = netbuffer->cmds[c].buttons;
    }

    //printf ("sending %i\n",gametic);
    c = p_sendto (sendsocket , (char *)&sw, doomcom->datalength
		  ,0,(struct sockaddr *)&sendaddress[doomcom->remotenode]
		  ,sizeof(sendaddress[doomcom->remotenode]));

    //	if (c == -1)
    //		I_Error ("SendPacket error: sock_errno %d",p_sock_errno());
}


//
// PacketGet
//
void PacketGet (void)
{
    int			i;
    int			c;
    struct sockaddr_in	fromaddress;
    int			fromlen;
    doomdata_t		sw;

    fromlen = sizeof(fromaddress);
    c = p_recvfrom (insocket, (char *)&sw, sizeof(sw), 0
		    , (struct sockaddr *)&fromaddress, &fromlen );
    if (c == -1 )
    {
	// Nothing waiting is the normal case on a non-blocking socket, not
	// an error.  Note this is sock_errno, not errno: the sockets library
	// keeps its own, and reading errno here would report whatever the
	// last unrelated file operation did.
	if (p_sock_errno() != SOCEWOULDBLOCK)
	    I_Error ("GetPacket: sock_errno %d", p_sock_errno());
	doomcom->remotenode = -1;		// no packet
	return;
    }

    {
	static int first=1;
	if (first)
	    printf("len=%d:p=[0x%x 0x%x] \n", c, *(int*)&sw, *((int*)&sw+1));
	first = 0;
    }

    // find remote node number
    for (i=0 ; i<doomcom->numnodes ; i++)
	if ( fromaddress.sin_addr.s_addr == sendaddress[i].sin_addr.s_addr )
	    break;

    if (i == doomcom->numnodes)
    {
	// packet is not from one of the players (new game broadcast)
	doomcom->remotenode = -1;		// no packet
	return;
    }

    doomcom->remotenode = i;			// good packet from a game player
    doomcom->datalength = c;

    // byte swap
    netbuffer->checksum = ntohl(sw.checksum);
    netbuffer->player = sw.player;
    netbuffer->retransmitfrom = sw.retransmitfrom;
    netbuffer->starttic = sw.starttic;
    netbuffer->numtics = sw.numtics;

    for (c=0 ; c< netbuffer->numtics ; c++)
    {
	netbuffer->cmds[c].forwardmove = sw.cmds[c].forwardmove;
	netbuffer->cmds[c].sidemove = sw.cmds[c].sidemove;
	netbuffer->cmds[c].angleturn = ntohs(sw.cmds[c].angleturn);
	netbuffer->cmds[c].consistancy = ntohs(sw.cmds[c].consistancy);
	netbuffer->cmds[c].chatchar = sw.cmds[c].chatchar;
	netbuffer->cmds[c].buttons = sw.cmds[c].buttons;
    }
}



int GetLocalAddress (void)
{
    char		hostname[1024];
    struct hostent*	hostentry;	// host information entry
    int			v;

    // get local address
    v = p_gethostname (hostname, sizeof(hostname));
    if (v == -1)
	I_Error ("GetLocalAddress : gethostname: sock_errno %d",
		 p_sock_errno());

    hostentry = p_gethostbyname (hostname);
    if (!hostentry)
	I_Error ("GetLocalAddress : gethostbyname: couldn't get local host");

    return *(int *)hostentry->h_addr_list[0];
}


//
// I_InitNetwork
//
void I_InitNetwork (void)
{
    int			trueval = true;
    int			i;
    int			p;
    struct hostent*	hostentry;	// host information entry

    doomcom = malloc (sizeof (*doomcom) );
    memset (doomcom, 0, sizeof(*doomcom) );

    // set up for network
    i = M_CheckParm ("-dup");
    if (i && i< myargc-1)
    {
	doomcom->ticdup = myargv[i+1][0]-'0';
	if (doomcom->ticdup < 1)
	    doomcom->ticdup = 1;
	if (doomcom->ticdup > 9)
	    doomcom->ticdup = 9;
    }
    else
	doomcom-> ticdup = 1;

    if (M_CheckParm ("-extratic"))
	doomcom-> extratics = 1;
    else
	doomcom-> extratics = 0;

    p = M_CheckParm ("-port");
    if (p && p<myargc-1)
    {
	DOOMPORT = atoi (myargv[p+1]);
	printf ("using alternate port %i\n",DOOMPORT);
    }

    // parse network game options,
    //  -net <consoleplayer> <host> <host> ...
    i = M_CheckParm ("-net");
    if (!i)
    {
	// single player game
	netgame = false;
	doomcom->id = DOOMCOM_ID;
	doomcom->numplayers = doomcom->numnodes = 1;
	doomcom->deathmatch = false;
	doomcom->consoleplayer = 0;
	return;
    }

    // Only now is TCP/IP actually needed.  A single player game on a machine
    // with none never gets here, and never has to care.
    if (!I_LoadTcpip ())
	I_Error ("I_InitNetwork: -net needs TCP/IP, and SO32DLL or TCP32DLL\n"
		 "could not be loaded.  Check that IBM TCP/IP is installed\n"
		 "and that its DLLs are on the LIBPATH.");

    netsend = PacketSend;
    netget = PacketGet;
    netgame = true;

    // parse player number and host list
    doomcom->consoleplayer = myargv[i+1][0]-'1';

    doomcom->numnodes = 1;	// this node for sure

    i++;
    while (++i < myargc && myargv[i][0] != '-')
    {
	sendaddress[doomcom->numnodes].sin_family = AF_INET;
	sendaddress[doomcom->numnodes].sin_port = htons(DOOMPORT);
	if (myargv[i][0] == '.')
	{
	    sendaddress[doomcom->numnodes].sin_addr.s_addr
		= p_inet_addr (myargv[i]+1);
	}
	else
	{
	    hostentry = p_gethostbyname (myargv[i]);
	    if (!hostentry)
		I_Error ("gethostbyname: couldn't find %s", myargv[i]);
	    sendaddress[doomcom->numnodes].sin_addr.s_addr
		= *(int *)hostentry->h_addr_list[0];
	}
	doomcom->numnodes++;
    }

    doomcom->id = DOOMCOM_ID;
    doomcom->numplayers = doomcom->numnodes;

    // build message to receive
    insocket = UDPsocket ();
    BindToLocalPort (insocket,htons(DOOMPORT));

    // Non-blocking, so that PacketGet can be asked every tic whether
    // anything arrived.  OS/2's ioctl takes the argument as a char pointer
    // and works out the length from the request code -- it is not the three
    // argument BSD one, whatever the name suggests.
    p_ioctl (insocket, FIONBIO, (char *)&trueval);

    sendsocket = UDPsocket ();
}


void I_NetCmd (void)
{
    if (doomcom->command == CMD_SEND)
    {
	netsend ();
    }
    else if (doomcom->command == CMD_GET)
    {
	netget ();
    }
    else
	I_Error ("Bad net cmd: %i\n",doomcom->command);
}
