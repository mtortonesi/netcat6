/*
 *  network.c - common networking functions module - implementation
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
#include "network.h"
#include "parser.h"
#include "filter.h"
#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <assert.h>

RCSID("@(#) $Header: /Users/cleishma/work/nc6-repo/nc6/src/network.c,v 1.16 2002-12-24 20:20:31 mauro Exp $");


/* Some systems (eg. linux) will bind to both ipv6 AND ipv4 when
 * listening.  Connections will still be accepted from either ipv6 or
 * ipv4 clients (ipv4 will be mapped into ipv6).  However, this means
 * that we MUST bind the ipv6 address ONLY for these hosts.
 *
 * To handle this, we will ensure that ipv6 sockets are bound first and then
 * attempt to bind the ipv4 ones.  On systems that double bind ipv6/ipv4 the
 * ipv4 bind will simply fail.  This function does the reordering - moving all
 * ipv6 addresses to the start of the getaddrinfo results.
 */
#ifdef ENABLE_IPV6
inline static struct addrinfo* order_ipv6_first(struct addrinfo *ai)
{
	struct addrinfo* ptr;
	struct addrinfo* lastv6 = NULL;
	struct addrinfo* tmp;

	assert(ai != NULL);

	/* Move all ipv6 addresses to the start of the list - keeping
	 * them in original order */

	if (ai->ai_family == PF_INET6)
		lastv6 = ai;

	for (ptr = ai; ptr && ptr->ai_next; ptr = ptr->ai_next) {
		if (ptr->ai_next->ai_family == PF_INET6) {
			tmp = ptr->ai_next;
			ptr->ai_next = tmp->ai_next;
			if (lastv6) {
				tmp->ai_next = lastv6->ai_next;
				lastv6->ai_next = tmp;
			} else {
				tmp->ai_next = ai;
				ai = tmp;
			}
			lastv6 = tmp;
		}
	}

	return ai;
}
#else
/* No ipv6 support - just make it a no-op */
#define order_ipv6_first(X)	(X)
#endif



/* on some systems, getaddrinfo will return results that can't actually be
 * used - resulting in a failure when trying to create the socket.
 * This function checks for all the different error codes that indicate this
 * situation */
inline static bool unsupported_sock_error(int err)
{
	return (err == EPFNOSUPPORT ||
	        err == EAFNOSUPPORT ||
	        err == EPROTONOSUPPORT ||
		err == ESOCKTNOSUPPORT ||
	        err == ENOPROTOOPT)?
		TRUE : FALSE;
}



void do_connect(connection_attributes *attrs)
{
	address *remote, *local;
	int err, fd = -1;
	struct addrinfo hints, *res = NULL, *ptr;
	bool connect_attempted = FALSE;
	char hbuf_rev[NI_MAXHOST + 1];
	char hbuf_num[NI_MAXHOST + 1];
	char sbuf_rev[NI_MAXSERV + 1];
	char sbuf_num[NI_MAXSERV + 1];

	assert(attrs != NULL);

	remote = &(attrs->remote_address);
	local = &(attrs->local_address);

	/* make sure all the preconditions are respected */
	assert(remote->address != NULL && strlen(remote->address) > 0);
	assert(remote->service != NULL && strlen(remote->service) > 0);
	assert(local->address == NULL || strlen(local->address) > 0);
	assert(local->service == NULL || strlen(local->service) > 0);
	
	/* setup hints structure to be passed to getaddrinfo */
	memset(&hints, 0, sizeof(hints));
	connection_attributes_to_addrinfo(&hints, attrs);

#ifdef HAVE_GETADDRINFO_AI_ADDRCONFIG
	hints.ai_flags |= AI_ADDRCONFIG;
#endif
	
	if (is_flag_set(NUMERIC_MODE) == TRUE)
		hints.ai_flags |= AI_NUMERICHOST;

	/* get the address of the remote end of the connection */
	err = getaddrinfo(remote->address, remote->service, &hints, &res);
	if (err != 0)
		fatal("forward host lookup failed for remote enpoint %s: %s",
		      remote->address, gai_strerror(err));

	/* check the results of getaddrinfo */
	assert(res != NULL);

	/* try connecting to any of the addresses returned by getaddrinfo */
	for (ptr = res; ptr != NULL; ptr = ptr->ai_next) {

		/* only accept socktypes we can handle */
		if (ptr->ai_socktype != SOCK_STREAM && ptr->ai_socktype != SOCK_DGRAM)
			continue;

#if defined(PF_INET6) && !defined(ENABLE_IPV6)
		/* skip IPv6 if disabled */
		if (ptr->ai_family == PF_INET6)
			continue;
#endif

		/* we are going to try to connect to this address */
		connect_attempted = TRUE;

		/* get the numeric name for this destination as a string */
		err = getnameinfo(ptr->ai_addr, ptr->ai_addrlen,
		                  hbuf_num, sizeof(hbuf_num), sbuf_num, 
		                  sizeof(sbuf_num), NI_NUMERICHOST | NI_NUMERICSERV);

		/* this should never happen */
		if (err != 0)
			fatal("getnameinfo failed: %s", gai_strerror(err));

		/* get the real name for this destination as a string */
		if ((is_flag_set(VERBOSE_MODE) == TRUE) &&
		    (is_flag_set(NUMERIC_MODE) == FALSE)) 
		{
			/* get the real name for this destination as a string */
			err = getnameinfo(ptr->ai_addr, ptr->ai_addrlen,
			                  hbuf_rev, sizeof(hbuf_rev), sbuf_rev, 
			                  sizeof(sbuf_rev), 0);

			if (err != 0)
				warn("inverse lookup failed for %s: %s",
				     hbuf_num, gai_strerror(err));
		} else {
			err = 1;
		}

		if (err != 0) {
			/* just make the real name the numeric string */
			strcpy(hbuf_rev, hbuf_num);
			strcpy(sbuf_rev, sbuf_num);
		}

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
			err = setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on,
					 sizeof(on));
			if (err < 0) warn("error with sockopt IPV6_V6ONLY");
		}
#endif 

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
		
			/* get the IP address of the local end of the connection */
			err = getaddrinfo(local->address, local->service, &hints, &src_res);
			if (err != 0)
				fatal("forward host lookup failed for source address %s: %s",
				      local->address, gai_strerror(err));

			/* check the results of getaddrinfo */
			assert(src_res != NULL);

			/* try binding to any of the addresses returned by getaddrinfo */
			for (src_ptr = src_res; src_ptr; src_ptr = src_ptr->ai_next) {
				err = bind(fd, src_ptr->ai_addr, src_ptr->ai_addrlen);
				if (err == 0)
					break;
			}
			
			if (err != 0) {
				/* make sure we have tried all the addresses returned by 
				 * getaddrinfo */
				assert(src_ptr == NULL);
				
				if (is_flag_set(VERBOSE_MODE) == TRUE) {
					warn("bind to source addr/port failed "
				             "when connecting %s [%s] %s (%s): %s",
					     hbuf_rev, hbuf_num, sbuf_num, sbuf_rev,
					     strerror(errno));
				}
				freeaddrinfo(src_res);
				close(fd);
				fd = -1;
				continue;
			}

			freeaddrinfo(src_res);
		}
		
		/* perform the connection */
		err = connect(fd, ptr->ai_addr, ptr->ai_addrlen);
		if (err != 0) {
			if (is_flag_set(VERBOSE_MODE) == TRUE) {
				warn("%s [%s] %s (%s): %s",
				     hbuf_rev, hbuf_num, sbuf_num, sbuf_rev, strerror(errno));
			}
			close(fd);
			fd = -1;
			continue;
		}

		if (fd >= 0)
			break;
	}

	/* either all possibilities were exahusted, or a connection was made */
	assert(ptr == NULL || fd >= 0);
	
	/* if the connection failed, output an error message */
	if (ptr == NULL) {
		/* if a connection was attempted, an error has already been output */
		if (connect_attempted == FALSE)
			fatal("forward lookup returned no usable socket types");
		exit(EXIT_FAILURE);
	}

	/* let the user know the connection has been established */
	if (is_flag_set(VERBOSE_MODE)) {
		warn("%s [%s] %s (%s) open", hbuf_rev, hbuf_num, sbuf_num, sbuf_rev);
	}

	if (is_flag_set(VERY_VERBOSE_MODE) == TRUE) {
		warn("using %s socket",
		     (ptr->ai_socktype == SOCK_STREAM)? "stream":"datagram");
	}

	/* cleanup addrinfo structure */
	freeaddrinfo(res);

	/* fill out the io_streams for the local and remote */
	ios_assign_stdio(&(attrs->local_stream));
	ios_assign_socket(&(attrs->remote_stream), fd, ptr->ai_socktype);
}



/* used to store the socktype of each fd being listened on in do_listen */
typedef struct fd_socktype_t {
	int fd;
	int socktype;
	struct fd_socktype_t* next;
} fd_socktype;



/* add a new fd/socktype pair to the list */
inline static fd_socktype* add_fd_socktype(fd_socktype* fd_socktypes,
                                    int fd, int socktype)
{
	fd_socktype* fdnew;
	fdnew = (fd_socktype*)xmalloc(sizeof(fd_socktype));
	fdnew->fd = fd;
	fdnew->socktype = socktype;
	/* prepend to the start of the list */
	fdnew->next = fd_socktypes;
	return fdnew;
}



/* retrieve a socktype for a given fd from the list */
inline static int find_fd_socktype(const fd_socktype* fd_socktypes, int fd)
{
	assert(fd_socktypes != NULL);
	while (fd_socktypes && fd_socktypes->fd != fd)
		fd_socktypes = fd_socktypes->next;
	return (fd_socktypes)? fd_socktypes->socktype : -1;
}



/* destroy an fd_socktype list */
inline static void destroy_fd_socktypes(fd_socktype* fd_socktypes)
{
	fd_socktype* tmp;
	while (fd_socktypes) {
		tmp = fd_socktypes;
		fd_socktypes = fd_socktypes->next;
		free(tmp);
	}
}



void do_listen(connection_attributes *attrs)
{
	address *remote, *local;
	int nfd, i, fd, err, ns = -1, socktype = -1, maxfd = -1;
	struct addrinfo hints, *res = NULL, *ptr;
	char hbuf_num[NI_MAXHOST + 1];
	char sbuf_num[NI_MAXSERV + 1];
	char c_hbuf_rev[NI_MAXHOST + 1];
	char c_hbuf_num[NI_MAXHOST + 1];
	char c_sbuf_num[NI_MAXSERV + 1];
	struct fd_socktype_t* fd_socktypes = NULL;
	fd_set accept_fdset;

	assert(attrs != NULL);

	remote = &(attrs->remote_address);
	local = &(attrs->local_address);

	/* make sure all the preconditions are respected */
	assert(local->address == NULL || strlen(local->address) > 0);
	assert(local->service != NULL && strlen(local->service) > 0);
	assert(remote->address == NULL || strlen(remote->address) > 0);
	assert(remote->service == NULL || strlen(remote->service) > 0);

	/* initialize accept_fdset */
	FD_ZERO(&accept_fdset);
	
	/* setup hints structure to be passed to getaddrinfo */
	memset(&hints, 0, sizeof(hints));
	connection_attributes_to_addrinfo(&hints, attrs);

	hints.ai_flags = AI_PASSIVE;
	if (is_flag_set(NUMERIC_MODE) == TRUE)
		hints.ai_flags |= AI_NUMERICHOST;

	/* get the IP address of the local end of the connection */
	err = getaddrinfo(local->address, local->service, &hints, &res);
	if (err != 0) 
		fatal("forward host lookup failed for local endpoint %s (%s): %s",
		      local->address? local->address : "[unspecified]",
		      local->service, gai_strerror(err));
		
	/* check the results of getaddrinfo */
	assert(res != NULL);

	/* Some systems (eg. linux) will bind to both ipv6 AND ipv4 when
	 * listening.  Connections will still be accepted from either ipv6 or
	 * ipv4 clients (ipv4 will be mapped into ipv6).  However, this means
	 * that we MUST bind the ipv6 address ONLY for these hosts.
	 *
	 * TODO: until we add a configure check to determine if the current
	 * host does this double binding, we will just ensure that ipv6
	 * sockets are bound first and then attempt to bind the ipv4 ones.  On
	 * systems that double bind ipv6/ipv4 the ipv4 bind will simply fail.
	 */
	res = order_ipv6_first(res);

	/* try binding to all of the addresses returned by getaddrinfo */
	nfd = 0;	
	for (ptr = res; ptr != NULL; ptr = ptr->ai_next) {

		/* only use socktypes we can handle */
		if (ptr->ai_socktype != SOCK_STREAM && ptr->ai_socktype != SOCK_DGRAM)
			continue;

#if defined(PF_INET6) && !defined(ENABLE_IPV6)
		/* skip IPv6 if disabled */
		if (ptr->ai_family == PF_INET6)
			continue;
#endif

		/* get the numeric name for this source as a string */
		err = getnameinfo(ptr->ai_addr, ptr->ai_addrlen,
		                  hbuf_num, sizeof(hbuf_num), sbuf_num, 
		                  sizeof(sbuf_num), NI_NUMERICHOST | NI_NUMERICSERV);

		/* this should never happen */
		if (err != 0)
			fatal("getnameinfo failed: %s", gai_strerror(err));

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
			err = setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on));
			if (err < 0) perror("error with sockopt IPV6_V6ONLY");
		}
#endif 
	
		/* set the reuse address socket option */
		if (!(is_flag_set(DONT_REUSE_ADDR) == TRUE)) {
			int on = 1;
			/* in case of error, we will go on anyway... */
			err = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
			if (err < 0) perror("error with sockopt SO_REUSEADDR");
		}

		/* bind to the local address */
		err = bind(fd, ptr->ai_addr, ptr->ai_addrlen);
		if (err != 0) {
			warn("bind to source %s (%s) failed: %s",
			     hbuf_num, sbuf_num, strerror(errno));
			close(fd);
			continue;
		}

		/* for stream based sockets, the socket needs to listen for incoming
		 * connections. the backlog parameter is set to 5 for backward
		 * compatibility (it seems that at least some BSD-derived system limit
		 * the backlog parameter to this value). */
		if (ptr->ai_socktype == SOCK_STREAM) {
			err = listen(fd, 5);
			if (err != 0)
				fatal("cannot listen on %s (%s): %s",
				      hbuf_num, sbuf_num, strerror(errno));
		}

		if (is_flag_set(VERBOSE_MODE) == TRUE)
			warn("listening on %s (%s) ...",
			     hbuf_num, sbuf_num, strerror(errno));

		/* add fd to fd_socktypes (just add to the head of the list) */
		fd_socktypes = add_fd_socktype(fd_socktypes, fd, ptr->ai_socktype);

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
		struct sockaddr_storage dest;
		socklen_t destlen;

		/* make a copy of accept_fdset before passing to select */
		memcpy(&tmp_ap_fdset, &accept_fdset, sizeof(fd_set));

		/* wait for an incoming connection */
		err = select(maxfd + 1, &tmp_ap_fdset, NULL, NULL, NULL);
		
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

		/* find socket type in fd_socktypes */
		socktype = find_fd_socktype(fd_socktypes, fd);

		destlen = sizeof(dest);	

		/* for stream sockets we can simply accept a new connection, while for 
		 * dgram sockets we have to use MSG_PEEK to determine the sender */
		if (socktype == SOCK_STREAM) {
			ns = accept(fd, (struct sockaddr *)&dest, &destlen);
			if (ns < 0)
				fatal("cannot accept connection: %s", strerror(errno));
		} else {
			/* this is checked when binding listen sockets */
			assert(socktype == SOCK_DGRAM);

			err = recvfrom(fd, NULL, 0, MSG_PEEK,
			               (struct sockaddr*)&dest, &destlen);
			if (err < 0)
				fatal("cannot recv from socket: %s", strerror(errno));

			ns = dup(fd);
			if (ns < 0)
				fatal("cannot duplicate file descriptor %d: %s",
				      fd, strerror(errno));
		}

		/* get names for each end of the connection */
		if (is_flag_set(VERBOSE_MODE) == TRUE) {
			struct sockaddr_storage src;
			socklen_t srclen = sizeof(src);

			/* find out what address the connection was to */
			err = getsockname(ns, (struct sockaddr *)&src, &srclen);
			if (err < 0)
				fatal("getsockname failed: %s", strerror(errno));

			/* get the numeric name for this source as a string */
			err = getnameinfo((struct sockaddr *)&src, srclen,
			                  hbuf_num, sizeof(hbuf_num), NULL, 0,
			                  NI_NUMERICHOST | NI_NUMERICSERV);

			/* this should never happen */
			if (err != 0)
				fatal("getnameinfo failed: %s", gai_strerror(err));

			/* get the numeric name for this client as a string */
			err = getnameinfo((struct sockaddr *)&dest, destlen,
			                  c_hbuf_num, sizeof(c_hbuf_num), c_sbuf_num, 
					  sizeof(c_sbuf_num), NI_NUMERICHOST | NI_NUMERICSERV);
			if (err != 0)
				fatal("getnameinfo failed: %s", gai_strerror(err));

			/* get the real name for this client as a string */
			if (is_flag_set(NUMERIC_MODE) == FALSE) {
				err = getnameinfo((struct sockaddr *)&dest, destlen,
				                  c_hbuf_rev, sizeof(c_hbuf_rev), NULL, 0, 0);
				if (err != 0)
					warn("inverse lookup failed for %s: %s",
				             c_hbuf_num, gai_strerror(err));
			} else {
				err = 1;
			}

			if (err != 0) {
				strcpy(c_hbuf_rev, c_hbuf_num);
			}
		}

		/* check if connections from this client are allowed */
		if ((remote == NULL) ||
		    (remote->address == NULL && remote->service == NULL) ||
		    (is_allowed((struct sockaddr*)&dest, remote, attrs) == TRUE)) {

			if (socktype == SOCK_DGRAM) {
				/* connect the socket to ensure we only talk with this client */
				err = connect(ns, (struct sockaddr*)&dest, destlen);
				if (err != 0)
					fatal("cannot connect datagram socket: %s",
					      strerror(errno));
			}

			if (is_flag_set(VERBOSE_MODE) == TRUE) {
				warn("connect to %s (%s) from %s [%s] %s",
				     hbuf_num, sbuf_num, c_hbuf_rev, c_hbuf_num, c_sbuf_num);
			}

			if (is_flag_set(VERY_VERBOSE_MODE) == TRUE) {
				warn("using %s socket",
				     (socktype == SOCK_STREAM)? "stream":"datagram");
			}

			break;
		} else {
			if (socktype == SOCK_DGRAM) {
				/* the connection wasn't accepted - remove the queued packet */
				recvfrom(ns, NULL, 0, 0, NULL, 0);
			}
			close(ns);
			ns = -1;

			if (is_flag_set(VERBOSE_MODE) == TRUE) {
				warn("refused connect to %s (%s) from %s [%s] %s",
				     hbuf_num, sbuf_num, c_hbuf_rev, c_hbuf_num, c_sbuf_num);
			}
		}
	}

	/* close the listening sockets */
	for (i = 0; i <= maxfd; ++i) {
		if (FD_ISSET(i, &accept_fdset) != 0) close(i);
	}

	/* the ns and socktype should be set */
	assert(ns >= 0);
	assert(socktype != -1);

	/* free the fd_socktype list */
	destroy_fd_socktypes(fd_socktypes);

	/* create io_streams for the local and remote streams */
	ios_assign_stdio(&(attrs->local_stream));
	ios_assign_socket(&(attrs->remote_stream), ns, socktype);
}
