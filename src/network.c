/*
 *  network.c - common networking functions module - implementation
 * 
 *  nc6 - an advanced netcat clone
 *  Copyright (C) 2001-2003 Mauro Tortonesi <mauro _at_ deepspace6.net>
 *  Copyright (C) 2002-2003 Chris Leishman <chris _at_ leishman.org>
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
#include <assert.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include "config.h"
#include "network.h"
#include "parser.h"
#include "filter.h"
#include "netsupport.h"

RCSID("@(#) $Header: /Users/cleishma/work/nc6-repo/nc6/src/network.c,v 1.33 2003-01-13 20:07:05 mauro Exp $");

static const int AI_STR_SIZE = (2 * (NI_MAXHOST + NI_MAXSERV + 2)) + 8;

#define CONNECTION_VALID	0
#define CONNECTION_TIMEOUT	-1
#define CONNECTION_ABORTED	-2



static bool skip_addrinfo(const struct addrinfo *ai)
{
	assert(ai != NULL);
	
	/* only use socktypes we can handle */
	if (ai->ai_socktype != SOCK_STREAM &&
	    ai->ai_socktype != SOCK_DGRAM) 
		return TRUE;

#ifdef ENABLE_IPV6
	/* 
	 * skip IPv4 mapped addresses returned from getaddrinfo,
	 * for security reasons. see:
	 *
	 * http://playground.iijlab.net/i-d/
	 *       /draft-itojun-ipv6-transition-abuse-01.txt
	 */
	if (is_address_ipv4_mapped(ai->ai_addr))
		return TRUE;
#else
#ifdef PF_INET6
	/* skip IPv6 if disabled */
	if (ai->ai_family == PF_INET6)
		return TRUE;
#endif
#endif
	
	return FALSE;
}



static void ai_to_string(const struct sockaddr *sa, socklen_t len, char *str, 
                         size_t size, bool numeric_mode)
{
	int err;
	char hbuf_rev[NI_MAXHOST + 1];
	char hbuf_num[NI_MAXHOST + 1];
	char sbuf_rev[NI_MAXSERV + 1];
	char sbuf_num[NI_MAXSERV + 1];

	assert(sa != NULL);
	assert(len > 0);
	assert(str != NULL);
	assert(size > 0);
	
	/* get the numeric name for this destination as a string */
	err = getnameinfo(sa, len, hbuf_num, sizeof(hbuf_num),
			  sbuf_num, sizeof(sbuf_num),
			  NI_NUMERICHOST | NI_NUMERICSERV);

	/* this should never happen */
	if (err != 0)
		fatal("getnameinfo failed: %s", gai_strerror(err));

	/* get the real name for this destination as a string */
	if (numeric_mode == FALSE) {
		/* get the real name for this destination as a string */
		err = getnameinfo(sa, len, hbuf_rev, sizeof(hbuf_rev),
				  sbuf_rev, sizeof(sbuf_rev), 0);
		if (err == 0) {
			snprintf(str, size, "%s (%s) %s [%s]", hbuf_rev, 
			         hbuf_num, sbuf_num, sbuf_rev);
		} else {
			warn("inverse lookup failed for %s: %s",
			     hbuf_num, gai_strerror(err));
			
			snprintf(str, size, "%s %s", hbuf_num, sbuf_num);
		}
	} else {
		snprintf(str, size, "%s %s", hbuf_num, sbuf_num);
	}
}



static int connect_with_timeout(int fd, const struct addrinfo *ai, int timeout)
{
	int err;
	struct timeval tv, *tvp = NULL;
	fd_set connect_fdset;
	socklen_t len;
	
	/* set connect timeout */
	if (timeout > 0) {
		tv.tv_sec = (time_t)timeout;
		tv.tv_usec = 0;
		tvp = &tv;
	}

	/* set fd to nonblocking */
	nonblock(fd);
	
	/* attempt the connection */
	err = connect(fd, ai->ai_addr, ai->ai_addrlen);
	
	if (err != 0 && errno == EINPROGRESS) {
		/* connection is proceeding
		 * it is complete (or failed) when select returns */

		/* initialize connect_fdset */
		FD_ZERO(&connect_fdset);
		FD_SET(fd, &connect_fdset);

		/* call select */
		do {
			err = select(fd + 1, NULL, &connect_fdset, 
				     NULL, tvp);
		} while (err < 0 && errno == EINTR);

		/* select error */
		if (err < 0)
			fatal("select error: %s", strerror(errno));
	
		/* we have reached a timeout */
		if (err == 0) 
			return CONNECTION_TIMEOUT;
		
		/* select returned successfully, but we must test socket 
		 * error for result */
		len = sizeof(err);
		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0)
			fatal("getsockopt error: %s", strerror(errno));
		
		/* setup errno according to the result returned by 
		 * getsockopt */
		if (err != 0) {
			errno = err;
			return CONNECTION_ABORTED;
		}
		
		return CONNECTION_VALID;
	}

	return CONNECTION_ABORTED;
}



int do_connect(const connection_attributes *attrs, int *rt_socktype)
{
	const address *remote, *local;
	int err, fd = -1;
	struct addrinfo hints, *res = NULL, *ptr;
	bool connect_attempted = FALSE;
	bool numeric_mode      = FALSE;
	bool verbose_mode      = FALSE;
	bool disable_nagle     = FALSE;
	char buf[AI_STR_SIZE];

	/* make sure that attrs is a valid pointer */
	assert(attrs != NULL);
	
	/* setup the addresses of the two connection endpoints */
	remote = ca_remote_address(attrs);
	local  = ca_local_address(attrs);

	/* make sure all the preconditions are respected */
	assert(remote->address != NULL && strlen(remote->address) > 0);
	assert(remote->service != NULL && strlen(remote->service) > 0);
	assert(local->address == NULL  || strlen(local->address)  > 0);
	assert(local->service == NULL  || strlen(local->service)  > 0);

	/* setup flags */
	numeric_mode  = is_flag_set(NUMERIC_MODE);
	verbose_mode  = is_flag_set(VERBOSE_MODE);
	disable_nagle = is_flag_set(DISABLE_NAGLE);
	
	/* setup hints structure to be passed to getaddrinfo */
	memset(&hints, 0, sizeof(hints));
	ca_to_addrinfo(&hints, attrs);

#ifdef HAVE_GETADDRINFO_AI_ADDRCONFIG
	hints.ai_flags |= AI_ADDRCONFIG;
#endif
	
	if (numeric_mode == TRUE)
		hints.ai_flags |= AI_NUMERICHOST;

	/* get the address of the remote end of the connection */
	err = getaddrinfo(remote->address, remote->service, &hints, &res);
	if (err != 0)
		fatal("forward host lookup failed for remote endpoint %s: %s",
		      remote->address, gai_strerror(err));

	/* check the results of getaddrinfo */
	assert(res != NULL);

	/* try connecting to any of the addresses returned by getaddrinfo */
	for (ptr = res; ptr != NULL; ptr = ptr->ai_next) {

		/* only accept socktypes we can handle */
		if (skip_addrinfo(ptr) == TRUE) continue;

		/* we are going to try to connect to this address */
		connect_attempted = TRUE;

		/* create the socket */
		fd = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
		if (fd < 0) {
			/* ignore this address if it is not supported */
			if (unsupported_sock_error(errno))
				continue;
			fatal("cannot create the socket: %s", strerror(errno));
		}
		
#if defined(ENABLE_IPV6) && defined(IPV6_V6ONLY)
		if (ptr->ai_family == PF_INET6) {
			int on = 1;
			/* in case of error, we will go on anyway... */
			err = setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY,
			                 &on, sizeof(on));
			if (err < 0) 
				warn("error with sockopt IPV6_V6ONLY");
		}
#endif 

		/* disable the nagle option for TCP sockets */
		if (disable_nagle == TRUE && ptr->ai_protocol == IPPROTO_TCP) {
			int on = 1;
			/* in case of error, we will go on anyway... */
			err = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
			                 &on, sizeof(on));
			if (err < 0) 
				warn("error with sockopt TCP_NODELAY");
		}

		/* setup buf if we're in verbose mode */
		if (verbose_mode == TRUE) 
			ai_to_string(ptr->ai_addr, ptr->ai_addrlen, buf, 
			             sizeof(buf), numeric_mode);
					
		/* setup local source address and/or service */
		if (local->address != NULL || local->service != NULL) {
			struct addrinfo *src_res = NULL, *src_ptr;
		
			/* setup hints structure to be passed to getaddrinfo */
			memset(&hints, 0, sizeof(hints));
			hints.ai_family   = ptr->ai_family;
			hints.ai_flags    = AI_PASSIVE;
			hints.ai_socktype = ptr->ai_socktype;
			hints.ai_protocol = ptr->ai_protocol;
	
			if (is_flag_set(NUMERIC_MODE) == TRUE) 
				hints.ai_flags |= AI_NUMERICHOST;
		
			/* get the local IP address of the connection */
			err = getaddrinfo(local->address, local->service,
			                  &hints, &src_res);
			if (err != 0)
				fatal("forward host lookup failed "
				      "for source address %s: %s",
				      local->address, gai_strerror(err));

			/* check the results of getaddrinfo */
			assert(src_res != NULL);

			/* try binding to any of the addresses */
			for (src_ptr = src_res; src_ptr != NULL;
			     src_ptr = src_ptr->ai_next) {
				err = bind(fd, src_ptr->ai_addr,
					   src_ptr->ai_addrlen);
				if (err == 0) break;
			}
			
			if (err != 0) {
				/* make sure we have tried all addresses */
				assert(src_ptr == NULL);
				
				if (verbose_mode == TRUE) {
					warn("bind to source addr/port failed "
					     "when connecting to "
					     "%s: %s", buf, strerror(errno));
				}
				freeaddrinfo(src_res);
				close(fd);
				fd = -1;
				continue;
			}

			freeaddrinfo(src_res);
		}

		/* attempt the connection */
		err = connect_with_timeout(fd, ptr, ca_connect_timeout(attrs));
		
		switch (err) {
		case CONNECTION_VALID: 
			/* everything went ok */
			break;
		case CONNECTION_ABORTED: 
			/* connection failed */
			if (verbose_mode == TRUE) 
				warn("cannot connect to %s: %s",
				     buf, strerror(errno));
			close(fd);
			fd = -1;
			continue;
		case CONNECTION_TIMEOUT: 
			/* connection failed */
			if (verbose_mode == TRUE) 
				warn("timeout while connecting to %s", buf);
			close(fd);
			fd = -1;
			continue;
		default: 
			fatal("internal error");
		}

		/* exit from the loop if we have a valid connection */
		if (fd >= 0)
			break;
	}

	/* either all possibilities were exahusted, or a connection was made */
	assert(ptr == NULL || fd >= 0);
	
	/* if the connection failed, output an error message */
	if (ptr == NULL) {
		/* if a connection was attempted, an error has been output */
		if (connect_attempted == FALSE) {
			fatal("forward lookup returned no usable socket types");
		} else {
			fatal("unable to connect to address %s, service %s", 
			      remote->address, remote->service);
		}
	}

	/* let the user know the connection has been established */
	if (verbose_mode == TRUE) {
		warn("%s open", buf);
	}

	/* return the socktype */
	if (rt_socktype != NULL)
		*rt_socktype = ptr->ai_socktype;

	/* cleanup addrinfo structure */
	freeaddrinfo(res);

	return fd;
}



void do_listen_continuous(const connection_attributes* attrs,
                          listen_callback callback, void* cdata, int max_accept)
{
	const address *remote, *local;
	int nfd, i, fd, err, maxfd = -1;
	struct addrinfo hints, *res = NULL, *ptr;
	char hbuf_num[NI_MAXHOST + 1];
	char sbuf_num[NI_MAXSERV + 1];
	char c_hbuf_rev[NI_MAXHOST + 1];
	char c_hbuf_num[NI_MAXHOST + 1];
	char c_sbuf_num[NI_MAXSERV + 1];
	bool numeric_mode      = FALSE;
	bool verbose_mode      = FALSE;
	bool dont_reuse_addr   = FALSE;
	bool disable_nagle     = FALSE;
#ifdef ENABLE_IPV6
	bool set_ipv6_only     = FALSE;
	bool bound_ipv6_any    = FALSE;
#endif
	struct bound_socket_t* bound_sockets = NULL;
	fd_set accept_fdset;
	char src_buf[AI_STR_SIZE];

	/* make sure that the arguments are correct */
	assert(attrs != NULL);
	assert(callback != NULL);
	
	/* setup the addresses of the two connection endpoints */
	remote = ca_remote_address(attrs);
	local  = ca_local_address(attrs);

	/* make sure all the preconditions are respected */
	assert(local->address == NULL  || strlen(local->address)  > 0);
	assert(local->service != NULL  && strlen(local->service)  > 0);
	assert(remote->address == NULL || strlen(remote->address) > 0);
	assert(remote->service == NULL || strlen(remote->service) > 0);

	/* if max_accept is 0, just return */
	if (max_accept == 0)
		return;

	/* setup flags */
	numeric_mode    = is_flag_set(NUMERIC_MODE);
	verbose_mode    = is_flag_set(VERBOSE_MODE);
	dont_reuse_addr = is_flag_set(DONT_REUSE_ADDR);
	disable_nagle   = is_flag_set(DISABLE_NAGLE);
	
	/* initialize accept_fdset */
	FD_ZERO(&accept_fdset);
	
	/* setup hints structure to be passed to getaddrinfo */
	memset(&hints, 0, sizeof(hints));
	ca_to_addrinfo(&hints, attrs);

	hints.ai_flags = AI_PASSIVE;
	if (numeric_mode == TRUE)
		hints.ai_flags |= AI_NUMERICHOST;

	/* get the IP address of the local end of the connection */
	err = getaddrinfo(local->address, local->service, &hints, &res);
	if (err != 0) 
		fatal("forward host lookup failed "
		      "for local endpoint %s (%s): %s",
		      local->address? local->address : "[unspecified]",
		      local->service, gai_strerror(err));
		
	/* check the results of getaddrinfo */
	assert(res != NULL);

#ifdef ENABLE_IPV6
	/* 
	 * Some systems (notably Linux) with a shared stack for ipv6 and ipv4, 
	 * have a broken bind(2) implementation. In these systems, binding an 
	 * ipv6 socket to ipv6_addr_any (::) will also bind the socket to
	 * the ipv4 unspecified address (0.0.0.0).
	 * 
	 * So when listening on ipv6_addr_any (::) accept(2) will return also
	 * ipv4 connection attemptes, as ipv4 mapped ipv6 addresses.
	 *
	 * This is a problem, since when called with AF_UNSPEC and AI_PASSIVE, 
	 * getaddrinfo will return results for both ipv6 AND ipv4, but if we 
	 * try to bind on the ipv4 unspecified address (0.0.0.0) AFTER we have 
	 * bound our socket to ipv6_addr_any (::), the syscall bind(2) will 
	 * fail and return -1, setting errno to EADDRINUSE.
	 *
	 * The following algorithm is used to work around this error to some
	 * degree:
	 *
	 * Ensure binds to IPv6 addresses are attempted before IPv4 addresses.
	 * On broken systems where IPV6_V6ONLY is not defined or the setsockopt
	 * fails:
	 * 
	 *   - Keep track of whether an IPv6 socket has been bound to 
	 *     in6_addr_any.
	 *   - If a bind to IPv4 fails with EADDRINUSE and an IPv6 socket
	 *     has been bound, then just ignore the error.
	 */

	/* 
	 * TODO: instead of reordering the results, just loop through the
	 * results twice - once for ipv6 then for the rest.
	 */
	res = order_ipv6_first(res);
#endif

	/* try binding to all of the addresses returned by getaddrinfo */
	nfd = 0;
	for (ptr = res; ptr != NULL; ptr = ptr->ai_next) {

		if (skip_addrinfo(ptr) == TRUE) continue;
		
		ai_to_string(ptr->ai_addr, ptr->ai_addrlen, src_buf, 
		             sizeof(src_buf), TRUE);

		/* create the socket */
		fd = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
		if (fd < 0) {
			/* ignore this address if it is not supported */
			if (unsupported_sock_error(errno))
				continue;
			fatal("cannot create the socket: %s", strerror(errno));
		}

#if defined(ENABLE_IPV6) && defined(IPV6_V6ONLY)
		if (ptr->ai_family == PF_INET6) {
			int on = 1;
			/* in case of error, we will go on anyway... */
			err = setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY,
			                 &on, sizeof(on));
			if (err < 0)
				warn("error with sockopt IPV6_V6ONLY");
			else
				set_ipv6_only = TRUE;
		}
#endif 
	
		/* set the reuse address socket option */
		if (dont_reuse_addr == FALSE) {
			int on = 1;
			/* in case of error, we will go on anyway... */
			err = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
			                 &on, sizeof(on));
			if (err < 0) warn("error with sockopt SO_REUSEADDR");
		}

		/* disable the nagle option for TCP sockets */
		if (disable_nagle == TRUE  && ptr->ai_protocol == IPPROTO_TCP) {
			int on = 1;
			/* in case of error, we will go on anyway... */
			err = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
			                 &on, sizeof(on));
			if (err < 0) 
				warn("error with sockopt TCP_NODELAY");
		}

		/* bind to the local address */
		err = bind(fd, ptr->ai_addr, ptr->ai_addrlen);
		if (err != 0) {
#ifdef ENABLE_IPV6
			/* suppress ADDRINUSE error for systems that double
			 * bind ipv6 and ipv4 */
			if (errno == EADDRINUSE &&
			    ptr->ai_family == PF_INET &&
			    set_ipv6_only == FALSE &&
			    bound_ipv6_any == TRUE) {
				warn("listening on %s ...", src_buf);
				close(fd);
				continue;
			}
#endif
			warn("bind to source %s failed: %s",
			     src_buf, strerror(errno));
			close(fd);
			continue;
		}

		/* for stream based sockets, we need to listen for incoming
		 * connections. the backlog parameter is set to 5 for backward
		 * compatibility (it seems that at least some BSD-derived
		 * system limit the backlog parameter to this value). */
		if (ptr->ai_socktype == SOCK_STREAM) {
			err = listen(fd, 5);
			if (err != 0)
				fatal("cannot listen on %s: %s",
				      src_buf, strerror(errno));
		}

		if (verbose_mode == TRUE)
			warn("listening on %s ...", src_buf);

#ifdef ENABLE_IPV6
		/* check if this was an IPv6 socket bound to IN6_ADDR_ANY */
		if (ptr->ai_family == PF_INET6 &&
		    memcmp(&((struct sockaddr_in6*)(ptr->ai_addr))->sin6_addr,
		           &in6addr_any, sizeof(struct in6_addr)) == 0) {
			bound_ipv6_any = TRUE;
		}
#endif

		/* add fd to bound_sockets (just add to the head of the list) */
		bound_sockets =	add_bound_socket(bound_sockets, fd, 
				                 ptr->ai_socktype);

		/* add fd to accept_fdset */
		FD_SET(fd, &accept_fdset);
		maxfd = MAX(maxfd, fd);
		nfd++;
	}

	freeaddrinfo(res);
	
	if (nfd == 0)
		fatal("failed to bind to any local addr/port");

	/* enter into the accept loop */
 	for (;;) {
		fd_set tmp_ap_fdset;
		struct timeval tv, *tvp = NULL;
		struct sockaddr_storage dest;
		socklen_t destlen;
		int ns, socktype;

		/* make a copy of accept_fdset before passing to select */
		memcpy(&tmp_ap_fdset, &accept_fdset, sizeof(fd_set));

		/* setup timeout */
		if (ca_connect_timeout(attrs) > 0) {
			tv.tv_sec = (time_t)ca_connect_timeout(attrs);
			tv.tv_usec = 0;
			tvp = &tv;
		}

		/* wait for an incoming connection */
		err = select(maxfd + 1, &tmp_ap_fdset, NULL, NULL, tvp);

		if (err == 0)
			fatal("connection timed out");
		
		if (err < 0) {
			if (errno == EINTR) continue;
			fatal("select error: %s", strerror(errno));
		}

		/* find the ready filedescriptor */
		for (fd = 0; fd <= maxfd && !FD_ISSET(fd, &tmp_ap_fdset); ++fd)
			;

		/* if none were ready, loop to select again */
		if (fd > maxfd)
			continue;

		/* find socket type in bound_sockets */
		socktype = find_bound_socket(bound_sockets, fd);

		destlen = sizeof(dest);	

		/* for stream sockets we accept a new connection, whereas for
		 * dgram sockets we use MSG_PEEK to determine the sender */
		if (socktype == SOCK_STREAM) {
			ns = accept(fd, (struct sockaddr *)&dest, &destlen);
			if (ns < 0)
				fatal("cannot accept connection: %s",
				      strerror(errno));
		} else {
			/* this is checked when binding listen sockets */
			assert(socktype == SOCK_DGRAM);

			err = recvfrom(fd, NULL, 0, MSG_PEEK,
			               (struct sockaddr*)&dest, &destlen);
			if (err < 0)
				fatal("cannot recv from socket: %s",
				      strerror(errno));

			ns = dup(fd);
			if (ns < 0)
				fatal("cannot duplicate file descriptor %d: %s",
				      fd, strerror(errno));
		}

		/* get names for each end of the connection */
		if (verbose_mode == TRUE) {
			struct sockaddr_storage src;
			socklen_t srclen = sizeof(src);

			/* find out what address the connection was to */
			err = getsockname(ns, (struct sockaddr *)&src, &srclen);
			if (err < 0)
				fatal("getsockname failed: %s",
				      strerror(errno));

			/* get the numeric name for this source as a string */
			err = getnameinfo((struct sockaddr *)&src, srclen,
			                  hbuf_num, sizeof(hbuf_num), NULL, 0,
			                  NI_NUMERICHOST | NI_NUMERICSERV);

			/* this should never happen */
			if (err != 0)
				fatal("getnameinfo failed: %s",
				      gai_strerror(err));

			/* get the numeric name for this client as a string */
			err = getnameinfo((struct sockaddr *)&dest, destlen,
			                  c_hbuf_num, sizeof(c_hbuf_num),
					  c_sbuf_num, sizeof(c_sbuf_num),
					  NI_NUMERICHOST | NI_NUMERICSERV);
			if (err != 0)
				fatal("getnameinfo failed: %s",
				      gai_strerror(err));

			/* get the real name for this client as a string */
			if (numeric_mode == FALSE) {
				err = getnameinfo((struct sockaddr *)&dest,
				                  destlen, c_hbuf_rev, 
						  sizeof(c_hbuf_rev), 
						  NULL, 0, 0);
				if (err != 0) {
					warn("inverse lookup failed for %s: %s",
				             c_hbuf_num, gai_strerror(err));
					strcpy(c_hbuf_rev, c_hbuf_num);
				}
			} else {
				strcpy(c_hbuf_rev, c_hbuf_num);
			}
		}

		/* check if connections from this client are allowed */
		if ((remote == NULL) ||
		    (remote->address == NULL && remote->service == NULL) ||
		    (is_allowed((struct sockaddr*)&dest,remote,attrs) == TRUE))
		{

			if (socktype == SOCK_DGRAM) {
				/* connect the socket to ensure we only talk
				 * with this client */
				err = connect(ns, (struct sockaddr*)&dest, 
				              destlen);
				if (err != 0)
					fatal("cannot connect "
					      "datagram socket: %s",
					      strerror(errno));
			}

			if (verbose_mode == TRUE) {
				warn("connect to %s (%s) from %s [%s] %s",
				     hbuf_num, sbuf_num,
				     c_hbuf_rev, c_hbuf_num, c_sbuf_num);
			}

			callback(ns, socktype, cdata);

			if (max_accept > 0 && --max_accept == 0)
				break;
		} else {
			if (socktype == SOCK_DGRAM) {
				/* the connection wasn't accepted - 
				 * remove the queued packet */
				recvfrom(ns, NULL, 0, 0, NULL, 0);
			}
			close(ns);

			if (verbose_mode == TRUE) {
				warn("refused connect "
				     "to %s (%s) from %s [%s] %s",
				     hbuf_num, sbuf_num,
				     c_hbuf_rev, c_hbuf_num, c_sbuf_num);
			}
		}
	}

	/* close the listening sockets */
	for (i = 0; i <= maxfd; ++i) {
		if (FD_ISSET(i, &accept_fdset) != 0) close(i);
	}

	/* free the bound_socket list */
	destroy_bound_sockets(bound_sockets);
}


typedef struct listen_once_result_t {
	int fd;
	int socktype;
} listen_once_result;


static void listen_once_callback(int fd, int socktype, void *cdata)
{
	listen_once_result *result = (listen_once_result*)cdata;

	assert(fd >= 0);
	assert(socktype >= 0);
	assert(result != NULL);

	result->fd = fd;
	result->socktype = socktype;
}


int do_listen(const connection_attributes *attrs, int *rt_socktype)
{
	listen_once_result result;

	/* listen for exactly one connection */
	do_listen_continuous(attrs, listen_once_callback, (void*)&result, 1);

	if (rt_socktype != NULL)
		*rt_socktype = result.socktype;

	return result.fd;
}
