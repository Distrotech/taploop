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

#include <sys/socket.h>
#include <linux/un.h>
#include <linux/limits.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>

#include "framework.h"

/* UNIX socket thread*/
struct framework_sockthread {
	char sock[UNIX_PATH_MAX+1];
	int mask;
	int family;
	int protocol;
	void *(*client)(void **data);
	void *(*cleanup)(void **data);
};

/*
 * client sock server
 */
void *clientsock_serv(void **data) {
	struct framework_sockthread *unsock = *data;
	struct sockaddr_un	adr;
	unsigned int salen;
	struct	timeval	tv;
	fd_set	rd_set, act_set;
	int selfd;
	int on = 1;
	int *clfd;
	int fd;

	if ((fd = socket(PF_UNIX, unsock->protocol, 0)) < 0) {
		return NULL;
	}

	/* set user RW */
	umask(unsock->mask);

	fcntl(fd, F_SETFD, O_NONBLOCK);
	memset(&adr, 0, sizeof(adr));
	adr.sun_family = PF_UNIX;
	salen = sizeof(adr);
	strncpy((char *)&adr.sun_path, unsock->sock, sizeof(adr.sun_path) -1);

	/*enable passing credentials*/
	setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &on, sizeof(on));

	if (bind(fd, (struct sockaddr *)&adr, salen)) {
		if (errno == EADDRINUSE) {
			/* delete old file*/
			unlink(unsock->sock);
			if (bind(fd, (struct sockaddr *)&adr, sizeof(struct sockaddr_un))) {
				perror("clientsock_serv (bind)");
				close(fd);
				return NULL;
			}
		} else {
			perror("clientsock_serv (bind)");
			close(fd);
			return NULL;
		}
	}

	if (listen(fd, 10)) {
		perror("client sock_serv (listen)");
		close(fd);
		return NULL;
	}

	FD_ZERO(&rd_set);
	FD_SET(fd, &rd_set);

	while (framework_threadok(data)) {
		act_set = rd_set;
		tv.tv_sec = 0;
		tv.tv_usec = 2000;

		selfd = select(fd + 1, &act_set, NULL, NULL, &tv);

		/*returned due to interupt continue or timed out*/
		if ((selfd < 0 && errno == EINTR) || (!selfd)) {
     			continue;
		} else if (selfd < 0) {
			break;
		}

		if (FD_ISSET(fd, &act_set)) {
			clfd = objalloc(sizeof(int), NULL);
			if ((*clfd = accept(fd, (struct sockaddr *)&adr, &salen))) {
				framework_mkthread(unsock->client, unsock->cleanup, NULL, clfd);
			} else {
				objunref(clfd);
			}
		}
	}

	close(fd);
	unlink(unsock->sock);

	return NULL;
}

void framework_unixsocket(char *sock, int protocol, int mask, void *connectfunc, void *cleanup) {
	struct framework_sockthread *unsock;

	unsock = objalloc(sizeof(*unsock), NULL);
	strncpy(unsock->sock, sock, UNIX_PATH_MAX);
	unsock->mask = mask;
	unsock->client = connectfunc;
	unsock->cleanup = cleanup;
	unsock->protocol = protocol;
	framework_mkthread(clientsock_serv, NULL, NULL, unsock);
}
