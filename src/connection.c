/*
 *  connection.h - connection description structures and functions - header
 * 
 *  nc6 - an advanced netcat clone
 *  Copyright (C) 2001-2002 Mauro Tortonesi <mauro _at_ ferrara.linux.it>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */  
#include "config.h"
#include "misc.h"
#include "connection.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <string.h>
#include <assert.h>
#include <netinet/in.h>


void connection_attributes_init(connection_attributes *attrs)
{
	assert(attrs);

	attrs->proto = PROTO_UNSPECIFIED;
	attrs->type = TCP_SOCKET;

	memset((void*)&(attrs->remote_address), 0, sizeof(attrs->remote_address));
	memset((void*)&(attrs->local_address), 0, sizeof(attrs->local_address));

	io_stream_init(&(attrs->remote_stream));
	io_stream_init(&(attrs->local_stream));

	/* the local stream has an infinite hold timeout by default */
	ios_set_hold_timeout(&(attrs->local_stream), -1);
}



void connection_attributes_destroy(connection_attributes *attrs)
{
	assert(attrs);

	io_stream_destroy(&(attrs->remote_stream));
	io_stream_destroy(&(attrs->local_stream));
}



void connection_attributes_to_addrinfo(struct addrinfo *ainfo,
		const connection_attributes *attrs)
{
	assert(ainfo);
	assert(attrs);

	switch (attrs->proto) {
		case PROTO_IPv6:
			ainfo->ai_family = AF_INET6;
			break;
		case PROTO_IPv4:
			ainfo->ai_family = AF_INET;
			break;
		case PROTO_UNSPECIFIED:
			ainfo->ai_family = AF_UNSPEC;
			break;
		default:
			fatal("internal error: unknown socket domain");
	}
	
	switch (attrs->type) {
		case UDP_SOCKET:
			ainfo->ai_protocol = IPPROTO_UDP;
			break;
		case TCP_SOCKET:
			ainfo->ai_protocol = IPPROTO_TCP;
			break;
		default:
			fatal("internal error: unknown socket type");
	}
}
