#include <assert.h>
#include <errno.h>
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include "misc.h"
#include "network.h"
#include "readwrite.h"

typedef void callback(int, int, io_stream*);



#if 0
void do_inetd(address *addr, io_source *local_io)
{
	int fd;
	io_stream local;

	assert(addr != NULL);
	assert(local_io != NULL);
	assert(local_io->program != NULL || local_io->addr != NULL);
	
	if (local_io->program != NULL) {
		program_to_io_stream(local_io->program, &local);
	} else {
		fd = tcp_connect_to(local_io->addr);
		socket_to_io_stream(fd, &local);
	}

	fd = tcp_bind_to(addr, callback_fn, &local);
}



static void callback_fn(int sock, int old_sock, io_stream *local)
{
	int pid;

	pid = fork();
	
	if (pid < 0) {
		/* error */
		fatal("error while forking: %s", strerror(errno));
	} else if (pid == 0) {
		/* child process */
		io_stream remote;
		
		close(old_sock);		
		socket_to_io_stream(sock, &remote);

		readwrite(&remote, local);		
	}
}
#endif



/* this function opens a socket, connects the socket to the address specified 
 * in addr and returns the file descriptor of the socket. */
static int tcp_connect_to(sa_family_t family, unsigned int flags, 
                          address *remote, address *local)
{
	int err, fd, destlen;
	struct addrinfo hints, *res = NULL;
	struct sockaddr_storage dest;

	/* make sure preconditions on remote address are respected */
	assert(remote != NULL);
	assert(remote->address != NULL && strlen(remote->address) > 0);
	assert(remote->port != NULL && strlen(remote->port) > 0 );

	assert((flags & USE_UDP) == 0);
	
	/* setup hints structure to be passed to getaddrinfo */
	memset(&hints, 0, sizeof(hints));
	hints.ai_family   = family;
	hints.ai_socktype = SOCK_STREAM;
	
	if (flags & NUMERIC_MODE) 
		hints.ai_flags |= AI_NUMERICHOST;
	
	/* get the IP address of the remote end of the connection */
	err = getaddrinfo(remote->address, remote->port, &hints, &res);
	if(err != 0) fatal("getaddrinfo error: %s", gai_strerror(err));

	/* check the results of getaddrinfo */
	assert(res != NULL);
	assert(res->ai_addrlen <= sizeof(dest));

	/* get the fisrt sockaddr structure returned by getaddrinfo */
	memcpy(&dest, res->ai_addr, res->ai_addrlen);	
	destlen = res->ai_addrlen;

	/* cleanup to avoid memory leaks */
	freeaddrinfo(res);

	/* create the socket */
	fd = socket(dest.ss_family, SOCK_STREAM, 0);
	if (fd < 0) fatal("cannot create the socket: %s", strerror(errno));

	/* handle -s option */
	if (local != NULL && (local->address != NULL || local->port != NULL)) {
		int on, srclen;
		struct sockaddr_storage src;
		
		/* make sure preconditions on local address are respected */
		assert(local->address == NULL || strlen(local->address) > 0);
		assert(local->port    == NULL || strlen(local->port) > 0);
		
		/* setup hints structure to be passed to getaddrinfo */
		memset(&hints, 0, sizeof(hints));
		hints.ai_family   = family;
		hints.ai_socktype = SOCK_STREAM;
	
		if (flags & NUMERIC_MODE) 
			hints.ai_flags |= AI_NUMERICHOST;
		
		/* get the IP address of the local end of the connection */
		err = getaddrinfo(local->address, local->port, &hints, &res);
		if(err != 0) fatal("getaddrinfo error: %s", gai_strerror(err));

		/* check the results of getaddrinfo */
		assert(res != NULL);
		assert(res->ai_addrlen <= sizeof(src));

		/* get the fisrt sockaddr structure returned by getaddrinfo */
		memcpy(&src, res->ai_addr, res->ai_addrlen);
		srclen = res->ai_addrlen;
		
		/* cleanup to avoid memory leaks */
		freeaddrinfo(res);

#ifdef IPV6_V6ONLY
		if (family == AF_INET6) {
			on = 1;
			/* in case of error, we will go on anyway... */
			err = setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on,
					 sizeof(on));
			if (err < 0) perror("error with sockopt IPV6_V6ONLY");
		}
#endif 
		
		on = 1;
		/* in case of error, we will go on anyway... */
		err = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
		if (err < 0) perror("error with sockopt SO_REUSEADDR");
		
		/* bind to the local address */
		err = bind(fd, (struct sockaddr *)&src, srclen);
		if (err != 0) 
			fatal("cannot use specified source addr/port: %s", 
		              strerror(errno));
		
	}

	err = connect(fd, (struct sockaddr *)&dest, destlen);
	if (err < 0) fatal("cannot establish connection: %s", strerror(errno));
	
	return fd;
}



/* this function has two working modes. 
 *
 * fd == NULL (single binding mode):
 * 	the function binds to the specified address and listens
 * 	for a single incoming connection. it returns the socket 
 * 	file descriptor of the first connection received.
 *
 * fd != NULL (continous binding mode):
 * 	the function binds to the specified address and enters
 * 	an infinite loop that listens for all incoming connections. 
 * 	at each incoming connection the function fn is called, with
 * 	the socket file descriptor and the prm parameter as arguments.
 */
static int tcp_bind_to(sa_family_t family, unsigned int flags,
                       address *remote, address *local, 
                       callback *fn, io_stream *prm)
{
	int err, fd, ns;
	struct addrinfo hints, *res = NULL;
	struct sockaddr_storage src;

	assert(remote != NULL);
	assert(local != NULL);
	assert(local->address != NULL || local->port != NULL);
	assert(local->address == NULL || strlen(local->address) > 0);
	assert(local->port    == NULL || strlen(local->port) > 0);
	
	assert((flags & USE_UDP) == 0);
	 
	memset(&hints, 0, sizeof(hints));
	hints.ai_family   = family;
	hints.ai_flags    = AI_PASSIVE;
	hints.ai_socktype = SOCK_STREAM;
	
	if (flags & NUMERIC_MODE) 
		hints.ai_flags |= AI_NUMERICHOST;

	err = getaddrinfo(local->address, local->port, &hints, &res);
	if (err != 0) fatal("getaddrinfo error: %s", gai_strerror(err));
		
	assert(res != NULL);

	memcpy(&src, res->ai_addr, res->ai_addrlen);	
	freeaddrinfo(res);

	fd = socket(res->ai_family, SOCK_STREAM, 0);
	if (fd < 0) fatal("cannot create the socket: %s", strerror(errno));

#ifdef IPV6_V6ONLY
	if (family == AF_INET6) {
		int on = 1;
		/* in case of error, we will go on anyway... */
		err = setsockopt(fd,IPPROTO_IPV6,IPV6_V6ONLY,&on,sizeof(on));
		if (err < 0) perror("error with sockopt IPV6_V6ONLY");
	}
#endif 
		
	if (flags & REUSE_ADDR) {
		int on = 1;
		/* in case of error, we will go on anyway... */
		err = setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&on,sizeof(on));
		if (err < 0) perror("error with sockopt SO_REUSEADDR");
	}

	err = bind(fd, (struct sockaddr *)&src, sizeof(src));
	if (err != 0) fatal("cannot use specified source addr/port: %s", 
		            strerror(errno));

	err = listen(fd, 5);
	if (err != 0) fatal("cannot listen on specified addr/port: %s", 
		            strerror(errno));

	/* enter in the accept loop */
 	for (;;) {
		size_t destlen;
		struct sockaddr_storage dest;
	
		destlen = sizeof(dest);	
		
		ns = accept(fd, (struct sockaddr *)&dest, &destlen);
		if (ns < 0) fatal("cannot accept connection: %s", 
		                  strerror(errno));

		if ((remote->address == NULL && remote->port == NULL) || 
		    is_allowed((struct sockaddr*)&dest, remote, flags) == TRUE) {
			/* break the loop after first accept if 
			 * we're not running in continuos mode */
			if (fn == NULL) break;
			/* else, if we're running in continuos mode 
			 * let's send the results to the callback 
			 * function */
			else fn(ns, fd, prm);
		} else close(ns);
	} 
	
	close(fd);	
	return ns;
}



void tcp_connect(sa_family_t family, unsigned int flags, 
		 address *remote_addr, address *local_addr)
{
	int fd;
	io_stream remote, local;

	assert(remote_addr != NULL);
	
	stdio_to_io_stream(&local);

	fd = tcp_connect_to(family, flags, remote_addr, local_addr);
	socket_to_io_stream(fd, &remote);

	readwrite(&remote, &local);
}



void tcp_listen(sa_family_t family, unsigned int flags, 
 	        address *remote_addr, address *local_addr)
{
	int fd;
	io_stream remote, local;

	assert(local_addr != NULL);
	
	stdio_to_io_stream(&local);

	fd = tcp_bind_to(family, flags, remote_addr, local_addr, NULL, NULL);
	socket_to_io_stream(fd, &remote);

	readwrite(&remote, &local);
}
