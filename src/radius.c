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

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <arpa/inet.h>

#include <uuid/uuid.h>
#include <openssl/md5.h>
#include <framework.h>

#include "radius.h"

struct eap_info {
        char    code;
        char    id;             /*session id*/
        short   len;            /*this will be same as pae len whole eap len*/
	char	type;
};


/*
 * a radius session is based on a ID packet for
 * session stored till a response the request token is also stored
 */
struct radius_session {
	unsigned short id;
	unsigned char request[RAD_AUTH_TOKEN_LEN];
	void	*cb_data;
	radius_cb read_cb;
	unsigned int olen;
	struct radius_packet *packet;
	struct timeval sent;
	const char *passwd;
	char	retries;
};

/*
 * connect to the server one connex holds 256 sessions
 */
struct radius_connection {
	int socket;
	unsigned char id;
	struct radius_server *server;
	int flags;
	struct bucket_list *sessions;
};

/*
 * define a server with host auth/acct port and secret
 * create "connextions" on demand each with upto 256 sessions
 * servers should not be removed without removing all and reloading
 */
struct radius_server {
	const char	*name;
	const char	*authport;
	const char	*acctport;
	const char	*secret;
	unsigned char	id;
	int		timeout;
	struct timeval	service;
	struct bucket_lists *connex;
};

struct bucket_list *servers = NULL;

struct radius_connection *radconnect(struct radius_server *server);

int hash_session(void *data, int key) {
	unsigned int ret;
	struct radius_session *session = data;
	unsigned char *hashkey = (key) ? data : &session->id;

	ret = *hashkey << 24;

	return (ret);
}

int hash_connex(void *data, int key) {
	int ret;
	struct radius_connection *connex = data;
	int *hashkey = (key) ? data : &connex->socket;

	ret = *hashkey;

	return (ret);
}

int hash_server(void *data, int key) {
	int ret;
	struct radius_server *server = data;
	unsigned char *hashkey = (key) ? data : &server->id;

	ret = *hashkey;

	return(ret);
}

void del_radserver(void *data) {
	struct radius_server *server = data;

	if (server->name) {
		free((char *)server->name);
	}
	if (server->authport) {
		free((char *)server->authport);
	}
	if (server->acctport) {
		free((char *)server->acctport);
	}
	if (server->secret) {
		free((char *)server->secret);
	}
	if (server->connex) {
		objunref(server->connex);
	}
}

void add_radserver(const char *ipaddr, const char *auth, const char *acct, const char *secret, int timeout) {
	struct radius_server *server;

	if ((server = objalloc(sizeof(*server), del_radserver))) {
		ALLOC_CONST(server->name, ipaddr);
		ALLOC_CONST(server->authport, auth);
		ALLOC_CONST(server->acctport, acct);
		ALLOC_CONST(server->secret, secret);
		if (!servers) {
			servers = create_bucketlist(0, hash_server);
		}
		server->id = bucket_list_cnt(servers);
		server->timeout = timeout;
		gettimeofday(&server->service, NULL);
		addtobucket(servers, server);
	}

	objunref(server);
}

void del_radsession(void *data) {
	struct radius_session *session = data;

	if (session->passwd) {
		free((void*)session->passwd);
	}
}

struct radius_session *rad_session(struct radius_packet *packet, struct radius_connection *connex,
					const char *passwd, radius_cb read_cb, void *cb_data) {
	struct radius_session *session = NULL;

	if ((session = objalloc(sizeof(*session), del_radsession))) {
		if (!connex->sessions) {
			connex->sessions = create_bucketlist(4, hash_session);
		}
		memcpy(session->request, packet->token, RAD_AUTH_TOKEN_LEN);
		session->id = packet->id;
		session->packet = packet;
		session->read_cb = read_cb;
		session->cb_data = cb_data;
		session->olen = packet->len;
		session->retries = 3;
		ALLOC_CONST(session->passwd, passwd);
		addtobucket(connex->sessions, session);
	}
	return (session);
}

int _send_radpacket(struct radius_packet *packet, const char *userpass, struct radius_session *hint,
			radius_cb read_cb, void *cb_data) {
	int scnt;
	unsigned char* vector;
	unsigned short len;
	struct radius_server *server;
	struct radius_session *session;
	struct radius_connection *connex;
	struct bucket_loop *sloop, *cloop;
	struct timeval	curtime;


	gettimeofday(&curtime, NULL);
	sloop = init_bucket_loop(servers);
	objref(hint);
	while (sloop && (server = next_bucket_loop(sloop))) {
		objlock(server);
		if (server->service.tv_sec > curtime.tv_sec) {
			objunlock(server);
			objunref(server);
			continue;
		}
		if (!server->connex) {
			connex = radconnect(server);
			objunref(connex);
			objunlock(server);
			objref(server);
		} else {
			objunlock(server);
		}
		cloop = init_bucket_loop(server->connex);
		while (cloop && (connex = next_bucket_loop(cloop))) {
			objlock(connex);
			if (connex->sessions && (bucket_list_cnt(connex->sessions) > 254)) {
				objunlock(connex);
				objunref(connex);
				/* if im overflowing get next or add new*/
				objlock(server);
				if (!(connex = next_bucket_loop(cloop))) {
					if ((connex = radconnect(server))) {
						objunlock(server);
						objref(server);
					} else {
						break;
					}
				} else {
					objunlock(server);
				}
				objlock(connex);
			}

			connex->id++;
			if (hint) {
				packet = hint->packet;
				session = hint;
				packet->id = connex->id;
				session->id = packet->id;
				session->retries = 3;
				addtobucket(connex->sessions, session);
			} else {
				packet->id = connex->id;
				session = rad_session(packet, connex, userpass, read_cb, cb_data);
			}
			objunlock(connex);

			if (session->passwd) {
				addattrpasswd(packet, session->passwd,  server->secret);
			}

			vector = addattr(packet, RAD_ATTR_MESSAGE, NULL, RAD_AUTH_TOKEN_LEN);
			len = packet->len;
			packet->len = htons(len);
			md5hmac(vector + 2, packet, len, server->secret, strlen(server->secret));

			scnt = send(connex->socket, packet, len, 0);
			memset(packet->attrs + session->olen - RAD_AUTH_HDR_LEN, 0, len - session->olen);
			packet->len = session->olen;

			objunref(connex);
			if (len == scnt) {
				session->sent = curtime;
				objunref(session);
				objunref(server);
				stop_bucket_loop(cloop);
				stop_bucket_loop(sloop);
				objunref(hint);
				return (0);
			} else {
				remove_bucket_item(connex->sessions, session);
				objunref(session);
			}
		}
		objunref(server);
		stop_bucket_loop(cloop);
	}
	stop_bucket_loop(sloop);
	objunref(hint);

	return (-1);
}

int send_radpacket(struct radius_packet *packet, const char *userpass, radius_cb read_cb, void *cb_data) {
	return (_send_radpacket(packet, userpass, NULL, read_cb, cb_data));
}

int resend_radpacket(struct radius_session *session) {
	return (_send_radpacket(NULL, NULL, session, NULL, NULL));
}

void rad_resend(struct radius_connection *connex) {
	struct radius_session *session;
	struct bucket_loop *bloop;
	struct timeval tv;
	unsigned int tdiff, len, scnt;
	unsigned char* vector;

	gettimeofday(&tv, NULL);

	bloop=init_bucket_loop(connex->sessions);
	while (bloop && (session = next_bucket_loop(bloop))) {
		tdiff = tv.tv_sec - session->sent.tv_sec;
		if (tdiff > 3) {
			if (!session->retries) {
				remove_bucket_loop(bloop);
				resend_radpacket(session);
				objunref(session);
				continue;
			}

			if (session->passwd) {
				addattrpasswd(session->packet, session->passwd, connex->server->secret);
			}

			vector = addattr(session->packet, RAD_ATTR_MESSAGE, NULL, RAD_AUTH_TOKEN_LEN);
			len = session->packet->len;
			session->packet->len = htons(len);
			md5hmac(vector + 2, session->packet, len, connex->server->secret, strlen(connex->server->secret));

			scnt = send(connex->socket, session->packet, len, 0);
			memset(session->packet->attrs + session->olen - RAD_AUTH_HDR_LEN, 0, len - session->olen);
			session->packet->len = session->olen;
			session->sent = tv;
			session->retries--;
			if (scnt != len) {
				/*need to redo packet*/
			}
		}
		objunref(session);
	}
	stop_bucket_loop(bloop);
}

void *rad_return(void **data) {
	struct radius_connection *connex = *data;
	struct radius_session *session;
	struct radius_packet *packet;
	unsigned char buff[RAD_AUTH_PACKET_LEN];
	unsigned char rtok[RAD_AUTH_TOKEN_LEN];
	unsigned char rtok2[RAD_AUTH_TOKEN_LEN];
	fd_set  rd_set, act_set;
	struct  timeval tv;
	int chk, plen, selfd;

	FD_ZERO(&rd_set);
	FD_SET(connex->socket, &rd_set);

	while (framework_threadok(data)) {
		act_set = rd_set;
		tv.tv_sec = 0;
		tv.tv_usec = 200000;

		selfd = select(connex->socket + 1, &act_set, NULL, NULL, &tv);

		if ((selfd < 0 && errno == EINTR) || (!selfd)) {
			rad_resend(connex);
			continue;
		} else if (selfd < 0) {
			break;
		}

		if (FD_ISSET(connex->socket, &act_set)) {
			/*shutdown if 0 is returned ??*/
			chk = recv(connex->socket, buff, 4096, 0);
			if (chk < 0) {
				if (errno == ECONNREFUSED) {
					printf("Connection Bad\n");
				}
			} else if (chk == 0) {
				objlock(connex->server);
				printf("Taking server off line for %is\n", connex->server->timeout);
				gettimeofday(&connex->server->service, NULL);
				connex->server->service.tv_sec += connex->server->timeout;
				objunlock(connex->server);
			}

			packet = (struct radius_packet*)&buff;
			plen = ntohs(packet->len);

			if ((chk < plen) || (chk <= RAD_AUTH_HDR_LEN)) {
				printf("OOps Did not get proper packet\n");
				continue;
			}

			memset(buff + plen, 0, RAD_AUTH_PACKET_LEN - plen);

			if (!(session = bucket_list_find_key(connex->sessions, &packet->id))) {
				printf("Could not find session\n");
				continue;
			}

			memcpy(rtok, packet->token, RAD_AUTH_TOKEN_LEN);
			memcpy(packet->token, session->request, RAD_AUTH_TOKEN_LEN);
			md5sum2(rtok2, packet, plen, connex->server->secret, strlen(connex->server->secret));

			if (md5cmp(rtok, rtok2, RAD_AUTH_TOKEN_LEN)) {
				printf("Invalid Signature");
				continue;
			}

			if (session->read_cb) {
				session->read_cb(packet, session->cb_data);
			}

			objunref(session);
		}
		rad_resend(connex);
	}

	return NULL;
}

void del_radconnect(void *data) {
	struct radius_connection *connex = data;

	objunref(connex->server);
	objunref(connex->sessions);
	close(connex->socket);
}

struct radius_connection *radconnect(struct radius_server *server) {
	struct radius_connection *connex;
	int val = 1;

	if ((connex = objalloc(sizeof(*connex), del_radconnect))) {
		if ((connex->socket = udpconnect(server->name, server->authport)) >= 0) {
			if (!server->connex) {
				server->connex = create_bucketlist(0, hash_connex);
			}
			setsockopt(connex->socket, SOL_IP, IP_RECVERR,(char*)&val, sizeof(val));
			connex->server = server;
			genrand(&connex->id, sizeof(connex->id));
			addtobucket(server->connex, connex);
			framework_mkthread(rad_return, NULL, NULL, connex);
		}
	}
	return (connex);
}

void radius_read(struct radius_packet *packet, void *pvt_data) {
	int cnt, cnt2;
	unsigned char *data;

	printf("\nREAD PACKET\n");
	cnt = ntohs(packet->len) - RAD_AUTH_HDR_LEN;
	data = packet->attrs;
	while(cnt > 0) {
		printf("Type %i Len %i / %i 0x", data[0], data[1], cnt);
		for (cnt2 = 2;cnt2 < data[1]; cnt2++) {
			printf("%02x", data[cnt2]);
		}
		printf("\n");
		cnt -= data[1];
		data += data[1];
	}
}

int rad_dispatch(struct radius_packet *lrp, const char *userpass, radius_cb read_cb, void *cb_data) {
	unsigned char *data;
	int cnt, cnt2;

	if (_send_radpacket(lrp, userpass, NULL, read_cb, NULL)) {
		printf("Sending Failed\n");
		return (-1);
	}

	printf("\nSENT PACKET\n");
	cnt = lrp->len - RAD_AUTH_HDR_LEN;
	data = lrp->attrs;
	while(cnt > 0) {
		printf("Type %i Len %i / %i 0x", data[0], data[1], cnt);
		for (cnt2 = 2;cnt2 < data[1]; cnt2++) {
			printf("%02x", data[cnt2]);
		}
		printf("\n");
		cnt -= data[1];
		data += data[1];
	}
	return (0);
}

int radmain (void) {
	unsigned char *ebuff, uuid[16];
	struct eap_info eap;
	int cnt;
	char *user = "gregory";
	struct radius_packet *lrp;

	add_radserver("192.168.245.124", "1812", NULL, "RadSecret", 10);
	add_radserver("127.0.0.1", "1812", NULL, "RadSecret", 10);

	lrp = new_radpacket(RAD_CODE_AUTHREQUEST, 1);
	addattrstr(lrp, RAD_ATTR_USER_NAME, user);
	addattrip(lrp, RAD_ATTR_NAS_IP_ADDR, "127.0.0.1");
	addattrint(lrp, RAD_ATTR_NAS_PORT, 0);
	addattrint(lrp, RAD_ATTR_SERVICE_TYPE, 1);
	addattrint(lrp, RAD_ATTR_PORT_TYPE, 15);

	eap.type = 1;
	eap.code = 2;
	eap.id = 1;
	cnt = 5 + strlen(user);
	eap.len = htons(cnt);
	ebuff = addattr(lrp, RAD_ATTR_EAP, (unsigned char*)&eap, cnt);
	memcpy(ebuff + 7, user, strlen(user));

	uuid_generate(uuid);
	addattr(lrp, RAD_ATTR_ACCTID, uuid, 16);

	rad_dispatch(lrp, "testpass", radius_read, NULL);

/*	objunref(servers);*/

	return (0);
}
