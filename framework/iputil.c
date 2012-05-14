/*
Copyright (C) 2012  Gregory Nietsky <gregory@distrotetch.co.za>
        http://www.distrotech.co.za

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <netdb.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <arpa/inet.h>

#include "framework.h"

/* socket select thread (read)*/
struct framework_sockdata {
	struct fwsocket *sock;
	void *data;
	socketrecv	read;
};

/* socket server thread*/
struct socket_server {
	struct fwsocket *sock;
	void *data;
	socketrecv	client;
	socketrecv	connect;
	threadcleanup	cleanup;
};

/*from sslutils im the only consumer*/
void dtsl_serveropts(struct fwsocket *sock);
void dtlstimeout(struct fwsocket *sock, struct timeval *timeleft, int defusec);
void dtlshandltimeout(struct fwsocket *sock);

void closesocket(struct fwsocket *sock) {
	int cnt;
	void *ssl;

	objlock(sock);
	if (sock->ssl) {
		ssl = sock->ssl;
		objlock(ssl);
		sock->ssl = NULL;
		objunlock(ssl);
		objunref(sock->ssl);
	}
	sock->flags &= ~SOCK_FLAG_RUNNING;
	objunlock(sock);

	for(cnt = 10; (cnt && !testflag(sock, SOCK_FLAG_CLOSING)); cnt--) {
		usleep(10000);
	}

	objlock(sock);
	shutdown(sock->sock, SHUT_RDWR);
	sock->sock = -1;
	objunlock(sock);
	objunref(sock);
}

void clean_fwsocket(void *data) {
	struct fwsocket *sock = data;
	int cnt;
	void *ssl;

	if (sock->ssl) {
		ssl = sock->ssl;
		objlock(ssl);
		sock->ssl = NULL;
		objunlock(ssl);
		objunref(sock->ssl);
	}
	sock->flags &= ~SOCK_FLAG_RUNNING;

	for(cnt = 10; (cnt && !testflag(sock, SOCK_FLAG_CLOSING)); cnt--) {
		usleep(10000);
	}

	if (sock->sock) {
		shutdown(sock->sock, SHUT_RDWR);
		sock->sock = -1;
	}
}

struct fwsocket *make_socket(int family, int type, int proto, void *ssl) {
	struct fwsocket *si;
/*	int fl;*/

	if (!(si = objalloc(sizeof(*si),clean_fwsocket))) {
		return NULL;
	}

	if ((si->sock = socket(family, type, proto)) < 0) {
		objunref(si);
		return NULL;
	};

/*	fl =fcntl(si->sock, F_GETFL, 0);
	fcntl(si->sock, F_SETFL, fl | O_NONBLOCK);*/
	if (ssl) {
		si->ssl = ssl;
	}
	si->type = type;
	si->proto = proto;

	return si;
}

struct fwsocket *accept_socket(struct fwsocket *sock) {
	struct fwsocket *si;
	socklen_t salen = sizeof(si->addr.sa);

	if (!(si = objalloc(sizeof(*si),clean_fwsocket))) {
		return NULL;
	}

	objlock(sock);
	if ((si->sock = accept(sock->sock, &si->addr.sa, &salen)) < 0) {
		objunlock(sock);
		objunref(si);
		return NULL;
	}

	si->type = sock->type;
	si->proto = sock->proto;

	if (sock->ssl) {
		si->ssl = sock->ssl;
		objunlock(sock);
		tlsaccept(si);
	} else {
		objunlock(sock);
	}

	return si;
}

struct fwsocket *_opensocket(int family, int stype, int proto, const char *ipaddr, const char *port, void *ssl, int ctype, int backlog) {
	struct	addrinfo hint, *result, *rp;
	struct fwsocket *sock = NULL;
	socklen_t salen = sizeof(struct sockaddr);
	int on = 1;

	memset(&hint, 0, sizeof(hint));
	hint.ai_family = family;
	hint.ai_socktype = stype;
	hint.ai_protocol = proto;

	if (getaddrinfo(ipaddr, port, &hint, &result)) {
		return (NULL);
	}

	for(rp = result; rp; rp = result->ai_next) {
		if (!(sock = make_socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol, ssl))) {
			continue;
		}
		if ((!ctype && !connect(sock->sock, rp->ai_addr, rp->ai_addrlen)) ||
		    (ctype && !bind(sock->sock, rp->ai_addr, rp->ai_addrlen))) {
			break;
		}
		objunref(sock);
		sock = NULL;
	}

	if (ctype && sock) {
		sock->flags |= SOCK_FLAG_BIND;
		memcpy(&sock->addr.ss, rp->ai_addr, sizeof(sock->addr.ss));
		setsockopt(sock->sock, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#ifdef SO_REUSEPORT
		setsockopt(sock->sock, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
#endif
		switch(sock->type) {
			case SOCK_STREAM:
			case SOCK_SEQPACKET:
				listen(sock->sock, backlog);
				break;
		}
	} else if (sock) {
		getsockname(sock->sock, &sock->addr.sa, &salen);
	}

	freeaddrinfo(result);
	return (sock);
}

struct fwsocket *sockconnect(int family, int stype, int proto, const char *ipaddr, const char *port, void *ssl) {
	return(_opensocket(family, stype, proto, ipaddr, port, ssl, 0, 0));
}

struct fwsocket *udpconnect(const char *ipaddr, const char *port, void *ssl) {
	return (_opensocket(PF_UNSPEC, SOCK_DGRAM, IPPROTO_UDP, ipaddr, port, ssl, 0, 0));
}

struct fwsocket *tcpconnect(const char *ipaddr, const char *port, void *ssl) {
	return (_opensocket(PF_UNSPEC, SOCK_STREAM, IPPROTO_TCP, ipaddr, port, ssl, 0, 0));
}

struct fwsocket *sockbind(int family, int stype, int proto, const char *ipaddr, const char *port, void *ssl, int backlog) {
	return(_opensocket(family, stype, proto, ipaddr, port, ssl, 1, backlog));
}

struct fwsocket *udpbind(const char *ipaddr, const char *port, void *ssl) {
	return (_opensocket(PF_UNSPEC, SOCK_DGRAM, IPPROTO_UDP, ipaddr, port, ssl, 1, 0));
}

struct fwsocket *tcpbind(const char *ipaddr, const char *port, void *ssl, int backlog) {
	return (_opensocket(PF_UNSPEC, SOCK_STREAM, IPPROTO_TCP, ipaddr, port, ssl, 1, backlog));
}

void *sock_select(void **data) {
	struct framework_sockdata *fwsel = *data;
	fd_set  rd_set, act_set;
	struct  timeval tv;
	int selfd, sock;

	if (!fwsel->sock) {
		return NULL;
	}

	FD_ZERO(&rd_set);
	objlock(fwsel->sock);
	sock = fwsel->sock->sock;
	FD_SET(sock, &rd_set);
	fwsel->sock->flags |= SOCK_FLAG_RUNNING;
	objunlock(fwsel->sock);

	while (framework_threadok(data) && testflag(fwsel->sock, SOCK_FLAG_RUNNING)) {
		act_set = rd_set;
		tv.tv_sec = 0;
		tv.tv_usec = 20000;

		selfd = select(sock + 1, &act_set, NULL, NULL, &tv);

		if ((selfd < 0 && errno == EINTR) || (!selfd)) {
			continue;
		} else if (selfd < 0) {
			break;
		}

		if (fwsel->read && FD_ISSET(sock, &act_set)) {
			fwsel->read(fwsel->sock, fwsel->data);
		}
	}
	setflag(fwsel->sock, SOCK_FLAG_CLOSING);

	objunref(fwsel->data);
	objunref(fwsel->sock);

	return NULL;
}

void *serv_threadclean(void *data) {
	struct socket_server *tcpsock = data;

	/*call cleanup and remove refs to data*/
	if (tcpsock->cleanup) {
		tcpsock->cleanup(tcpsock->data);
	}
	if (tcpsock->data) {
		objunref(tcpsock->data);
	}

	return NULL;
}

/*
 * tcp thread spawns a thread on each connect
 */
void *tcpsock_serv(void **data) {
	struct socket_server *tcpsock = *data;
	struct	timeval	tv;
	fd_set	rd_set, act_set;
	int selfd, sockfd;
	struct fwsocket *newfd;

	objlock(tcpsock->sock);
	FD_ZERO(&rd_set);
	sockfd = tcpsock->sock->sock;
	FD_SET(sockfd, &rd_set);
	objunlock(tcpsock->sock);

	setflag(tcpsock->sock, SOCK_FLAG_RUNNING);
	while (framework_threadok(data) && testflag(tcpsock->sock, SOCK_FLAG_RUNNING)) {
		act_set = rd_set;
		tv.tv_sec = 0;
		tv.tv_usec = 20000;

		selfd = select(sockfd + 1, &act_set, NULL, NULL, &tv);

		/*returned due to interupt continue or timed out*/
		if ((selfd < 0 && errno == EINTR) || (!selfd)) {
     			continue;
		} else if (selfd < 0) {
			break;
		}
		if ((FD_ISSET(sockfd, &act_set)) &&
		    (newfd = accept_socket(tcpsock->sock))) {
			socketclient(newfd, tcpsock->data, tcpsock->client);
			if (tcpsock->connect) {
				tcpsock->connect(newfd, tcpsock->data);
			}
			objunref(newfd);
		}
	}
	setflag(tcpsock->sock, SOCK_FLAG_CLOSING);
	objunref(tcpsock->sock);

	return NULL;
}

/*
 * tcp thread spawns a thread on each connect
 */
void *dtls_serv(void **data) {
	struct socket_server *dtlssock = *data;
	struct fwsocket *newsock;
	struct	timeval	tv;
	fd_set	rd_set, act_set;
	int selfd, sockfd;

	dtsl_serveropts(dtlssock->sock);

	FD_ZERO(&rd_set);
	objlock(dtlssock->sock);
	sockfd = dtlssock->sock->sock;
	FD_SET(sockfd, &rd_set);
	objunlock(dtlssock->sock);

	setflag(dtlssock->sock, SOCK_FLAG_RUNNING);
	while (framework_threadok(data) && testflag(dtlssock->sock, SOCK_FLAG_RUNNING)) {
		act_set = rd_set;
		dtlstimeout(dtlssock->sock, &tv, 20000);
		selfd = select(sockfd + 1, &act_set, NULL, NULL, &tv);

		/*returned due to interupt continue or timed out*/
		if ((selfd < 0 && errno == EINTR) || (!selfd)) {
			dtlshandltimeout(dtlssock->sock);
     			continue;
		} else if (selfd < 0) {
			break;
		}

		if (FD_ISSET(sockfd, &act_set) && (newsock = dtls_listenssl(dtlssock->sock))) {
			socketclient(newsock, dtlssock->data, dtlssock->client);
			if (dtlssock->connect) {
				dtlssock->connect(newsock, dtlssock->data);
			}
		}
	}
	dtlssock->sock->flags |= SOCK_FLAG_CLOSING;

	objunref(dtlssock->sock);

	return NULL;
}

void socketclient(struct fwsocket *sock, void *data, socketrecv read) {
	struct framework_sockdata *fwsel;

	if (!(fwsel = objalloc(sizeof(*fwsel), NULL))) {
		return;
	}

	fwsel->sock = sock;
	fwsel->data = data;
	fwsel->read = read;

	/* grab ref for data and pass fwsel*/
	startsslclient(sock);
	objref(data);
	objref(sock);
	framework_mkthread(sock_select, NULL, NULL, fwsel);
	objunref(fwsel);
}

void socketserver(struct fwsocket *sock, socketrecv connectfunc,
				socketrecv acceptfunc, threadcleanup cleanup, void *data) {
	struct socket_server *servsock;
	int type;

	if (!sock || !(servsock = objalloc(sizeof(*servsock), NULL))) {
		return;
	}

	servsock->sock = sock;
	servsock->client = connectfunc;
	servsock->cleanup = cleanup;
	servsock->connect = acceptfunc;
	servsock->data = data;

	/* grab ref for data and pass servsock*/
	objlock(sock);
	type = sock->type;
	objunlock(sock);
	switch(type) {
		case SOCK_STREAM:
			objref(data);
			objref(sock);
			framework_mkthread(tcpsock_serv, serv_threadclean, NULL, servsock);
			break;
		case SOCK_DGRAM:
			if (sock->ssl) {
				objref(data);
				objref(sock);
				framework_mkthread(dtls_serv, serv_threadclean, NULL, servsock);
			} else {
				socketclient(sock, data, connectfunc);
			}
			break;
	}
	objunref(servsock);
}
