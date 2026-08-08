/*
net_ps3.h - PS3 (PSL1GHT) network stubs
Copyright (C) 2026 xashPS3 contributors

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#pragma once
#ifndef NET_PS3_H
#define NET_PS3_H

#include "platform/posix/net.h"
#include <stdlib.h> // calloc/free, for the getaddrinfo shim below
#include <string.h> // memcpy/strncpy

// PSL1GHT is IPv4-only, but net_ws.c references IPv6 types unconditionally, so
// they must exist to compile even though no real IPv6 socket is ever created.
#define XASH_NO_IPV6_RESOLVE 1

struct in6_addr
{
	u8 s6_addr[16];
};

// Leading sin6_len/sin6_family match sockaddr_in's layout so net_ws.c's
// sockaddr_storage <-> sockaddr_in6 pointer casts read the same family field.
struct sockaddr_in6
{
	u8              sin6_len;
	sa_family_t     sin6_family;
	in_port_t       sin6_port;
	u32             sin6_flowinfo;
	struct in6_addr sin6_addr;
	u32             sin6_scope_id;
};

struct sockaddr_storage
{
	u8          ss_len;
	sa_family_t ss_family;
	char        ss_pad[sizeof( struct sockaddr_in6 ) - 2];
};

static const struct in6_addr in6addr_any;

#ifndef IPPROTO_IPV6
#define IPPROTO_IPV6 41
#endif

#ifndef IPV6_MULTICAST_LOOP
#define IPV6_MULTICAST_LOOP 19
#endif

#ifndef IPV6_V6ONLY
#define IPV6_V6ONLY 26
#endif

#ifndef IN6_IS_ADDR_V4MAPPED
#define IN6_IS_ADDR_V4MAPPED( p ) ( 0 )
#endif

// PSL1GHT has no ioctl(); net_ws.c only uses ioctlsocket() for FIONBIO, which
// PSL1GHT exposes as setsockopt(SO_NBIO) instead.

#ifndef FIONBIO
#define FIONBIO SO_NBIO
#endif

static inline int ioctl_ps3( int fd, int req, unsigned int *arg )
{
	if( req == FIONBIO )
		return setsockopt( fd, SOL_SOCKET, SO_NBIO, arg, sizeof( *arg ));
	return -ENOSYS;
}

#define ioctlsocket ioctl_ps3

// PSL1GHT has no getaddrinfo()/freeaddrinfo(); implement a real IPv4-only wrapper
// around gethostbyname(), since NET_GetHostByName() calls them unconditionally.

static inline int getaddrinfo_ps3( const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res )
{
	struct hostent *he;
	struct sockaddr_in *sin;
	struct addrinfo *ai;

	if( hints && hints->ai_family == AF_INET6 )
		return -1;

	he = gethostbyname( node );
	if( !he || he->h_addrtype != AF_INET || !he->h_addr_list || !he->h_addr_list[0] )
		return -1;

	sin = (struct sockaddr_in *)calloc( 1, sizeof( *sin ));
	if( !sin )
		return -1;

	sin->sin_family = AF_INET;
	sin->sin_len = sizeof( *sin );
	memcpy( &sin->sin_addr, he->h_addr_list[0], sizeof( sin->sin_addr ));

	ai = (struct addrinfo *)calloc( 1, sizeof( *ai ));
	if( !ai )
	{
		free( sin );
		return -1;
	}

	ai->ai_family = AF_INET;
	ai->ai_addrlen = sizeof( *sin );
	ai->ai_addr = (struct sockaddr *)sin;
	ai->ai_next = NULL;

	*res = ai;
	return 0;
}

static inline void freeaddrinfo_ps3( struct addrinfo *res )
{
	if( res )
	{
		free( res->ai_addr );
		free( res );
	}
}

#define getaddrinfo getaddrinfo_ps3
#define freeaddrinfo freeaddrinfo_ps3

// No hostname concept on this console; callers only use this as a fallback bind
// address, so a fixed name is correct, not a lossy stub.
static inline int gethostname_ps3( char *name, size_t len )
{
	strncpy( name, "localhost", len );
	return 0;
}

#define gethostname gethostname_ps3

#endif // NET_PS3_H
