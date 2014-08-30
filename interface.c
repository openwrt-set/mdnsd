/*
 * Copyright (C) 2014 John Crispin <blogic@openwrt.org>
 * Copyright (C) 2014 Felix Fietkau <nbd@openwrt.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2.1
 * as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define _GNU_SOURCE
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>

#include <libubox/usock.h>
#include <libubox/uloop.h>
#include <libubox/avl-cmp.h>
#include <libubox/utils.h>
#include "interface.h"
#include "util.h"
#include "dns.h"
#include "announce.h"

static int
interface_send_packet4(struct interface *iface, struct iovec *iov, int iov_len)
{
	static size_t cmsg_data[( CMSG_SPACE(sizeof(struct in_pktinfo)) / sizeof(size_t)) + 1];
	static struct sockaddr_in a;
	static struct msghdr m = {
		.msg_name = (struct sockaddr *) &a,
		.msg_namelen = sizeof(a),
		.msg_control = cmsg_data,
		.msg_controllen = CMSG_LEN(sizeof(struct in_pktinfo)),
	};
	struct in_pktinfo *pkti;
	struct cmsghdr *cmsg;
	int fd = iface->fd.fd;

	a.sin_family = AF_INET;
	a.sin_port = htons(MCAST_PORT);
	m.msg_iov = iov;
	m.msg_iovlen = iov_len;

	memset(cmsg_data, 0, sizeof(cmsg_data));
	cmsg = CMSG_FIRSTHDR(&m);
	cmsg->cmsg_len = m.msg_controllen;
	cmsg->cmsg_level = IPPROTO_IP;
	cmsg->cmsg_type = IP_PKTINFO;

	pkti = (struct in_pktinfo*) CMSG_DATA(cmsg);
	pkti->ipi_ifindex = iface->ifindex;

	a.sin_addr.s_addr = inet_addr(MCAST_ADDR);

	return sendmsg(fd, &m, 0);
}

static int
interface_send_packet6(struct interface *iface, struct iovec *iov, int iov_len)
{
	static size_t cmsg_data[( CMSG_SPACE(sizeof(struct in6_pktinfo)) / sizeof(size_t)) + 1];
	static struct sockaddr_in6 a;
	static struct msghdr m = {
		.msg_name = (struct sockaddr *) &a,
		.msg_namelen = sizeof(a),
		.msg_control = cmsg_data,
		.msg_controllen = CMSG_LEN(sizeof(struct in6_pktinfo)),
	};
	struct in6_pktinfo *pkti;
	struct cmsghdr *cmsg;
	int fd = iface->fd.fd;

	a.sin6_family = AF_INET6;
	a.sin6_port = htons(MCAST_PORT);
	m.msg_iov = iov;
	m.msg_iovlen = iov_len;

	memset(cmsg_data, 0, sizeof(cmsg_data));
	cmsg = CMSG_FIRSTHDR(&m);
	cmsg->cmsg_len = m.msg_controllen;
	cmsg->cmsg_level = IPPROTO_IPV6;
	cmsg->cmsg_type = IPV6_PKTINFO;

	pkti = (struct in6_pktinfo*) CMSG_DATA(cmsg);
	pkti->ipi6_ifindex = iface->ifindex;

	inet_pton(AF_INET6, MCAST_ADDR6, &a.sin6_addr);

	return sendmsg(fd, &m, 0);
}

int
interface_send_packet(struct interface *iface, struct iovec *iov, int iov_len)
{
	if (iface->v6)
		return interface_send_packet6(iface, iov, iov_len);

	return interface_send_packet4(iface, iov, iov_len);
}

static void interface_close(struct interface *iface)
{
	if (iface->fd.fd < 0)
		return;

	announce_free(iface);
	uloop_fd_delete(&iface->fd);
	close(iface->fd.fd);
	iface->fd.fd = -1;
}

static void interface_free(struct interface *iface)
{
	interface_close(iface);
	free(iface);
}

static void
read_socket(struct uloop_fd *u, unsigned int events)
{
	struct interface *iface = container_of(u, struct interface, fd);
	static uint8_t buffer[8 * 1024];
	struct iovec iov[1];
	char cmsg6[CMSG_SPACE(sizeof(struct in6_pktinfo))];
	struct cmsghdr *cmsgptr;
	struct msghdr msg;
	socklen_t len;
	struct sockaddr_in6 from;
	int flags = 0, ifindex = -1;

	if (u->eof) {
		interface_close(iface);
		uloop_timeout_set(&iface->reconnect, 1000);
		return;
	}

	iov[0].iov_base = buffer;
	iov[0].iov_len = sizeof(buffer);

	memset(&msg, 0, sizeof(msg));
	msg.msg_name = (struct sockaddr *) &from;
	msg.msg_namelen = (iface->v6) ? (sizeof(struct sockaddr_in6)) : (sizeof(struct sockaddr_in));
	msg.msg_iov = iov;
	msg.msg_iovlen = 1;
	msg.msg_control = &cmsg6;
	msg.msg_controllen = sizeof(cmsg6);

	len = recvmsg(u->fd, &msg, flags);
	if (len == -1) {
		perror("read failed");
		return;
	}
	for (cmsgptr = CMSG_FIRSTHDR(&msg); cmsgptr != NULL && ifindex == -1; cmsgptr = CMSG_NXTHDR(&msg, cmsgptr)) {
		void *c = CMSG_DATA(cmsgptr);

		if (cmsgptr->cmsg_level == IPPROTO_IP && cmsgptr->cmsg_type == IP_PKTINFO)
			ifindex = ((struct in_pktinfo *) c)->ipi_ifindex;
		else if (cmsgptr->cmsg_level == IPPROTO_IPV6 && cmsgptr->cmsg_type == IPV6_PKTINFO)
			ifindex = ((struct in6_pktinfo *) c)->ipi6_ifindex;
	}
	if (ifindex != iface->ifindex)
		fprintf(stderr, "invalid iface index %d != %d\n", ifindex, iface->ifindex);
	else
		dns_handle_packet(iface, buffer, len);
}

static int
interface_socket_setup4(struct interface *iface)
{
	struct ip_mreqn mreq;
	uint8_t ttl = 255;
	int yes = 1;
	int no = 0;
	struct sockaddr_in sa = { 0 };
	int fd = iface->fd.fd;

	sa.sin_family = AF_INET;
	sa.sin_port = htons(MCAST_PORT);
	inet_pton(AF_INET, MCAST_ADDR, &sa.sin_addr);

	memset(&mreq, 0, sizeof(mreq));
	mreq.imr_address.s_addr = iface->v4_addr.s_addr;
	mreq.imr_multiaddr = sa.sin_addr;
	mreq.imr_ifindex = iface->ifindex;

	if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0)
		fprintf(stderr, "ioctl failed: IP_MULTICAST_TTL\n");

	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0)
		fprintf(stderr, "ioctl failed: SO_REUSEADDR\n");

	/* Some network drivers have issues with dropping membership of
	 * mcast groups when the iface is down, but don't allow rejoining
	 * when it comes back up. This is an ugly workaround
	 * -- this was copied from avahi --
	 */
	setsockopt(fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));

	if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
		fprintf(stderr, "failed to join multicast group: %s\n", strerror(errno));
		close(fd);
		fd = -1;
		return -1;
	}

	if (setsockopt(fd, IPPROTO_IP, IP_RECVTTL, &yes, sizeof(yes)) < 0)
		fprintf(stderr, "ioctl failed: IP_RECVTTL\n");

	if (setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &yes, sizeof(yes)) < 0)
		fprintf(stderr, "ioctl failed: IP_PKTINFO\n");

	if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &no, sizeof(no)) < 0)
		fprintf(stderr, "ioctl failed: IP_MULTICAST_LOOP\n");

	return 0;
}

static int
interface_socket_setup6(struct interface *iface)
{
	struct ipv6_mreq mreq;
	int ttl = 255;
	int yes = 1;
	int no = 0;
	struct sockaddr_in6 sa = { 0 };
	int fd = iface->fd.fd;

	sa.sin6_family = AF_INET6;
	sa.sin6_port = htons(MCAST_PORT);
	inet_pton(AF_INET6, MCAST_ADDR6, &sa.sin6_addr);

	memset(&mreq, 0, sizeof(mreq));
	mreq.ipv6mr_multiaddr = sa.sin6_addr;
	mreq.ipv6mr_interface = iface->ifindex;

	if (setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &ttl, sizeof(ttl)) < 0)
		fprintf(stderr, "ioctl failed: IPV6_MULTICAST_HOPS\n");

	if (setsockopt(fd, IPPROTO_IPV6, IPV6_UNICAST_HOPS, &ttl, sizeof(ttl)) < 0)
		fprintf(stderr, "ioctl failed: IPV6_UNICAST_HOPS\n");

	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) < 0)
		fprintf(stderr, "ioctl failed: SO_REUSEADDR\n");

	setsockopt(fd, IPPROTO_IPV6, IPV6_LEAVE_GROUP, &mreq, sizeof(mreq));
	if (setsockopt(fd, IPPROTO_IPV6, IPV6_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
		fprintf(stderr, "failed to join multicast group: %s\n", strerror(errno));
		close(fd);
		fd = -1;
		return -1;
	}

	if (setsockopt(fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &yes, sizeof(yes)) < 0)
		fprintf(stderr, "ioctl failed: IPV6_RECVPKTINFO\n");

	if (setsockopt(fd, IPPROTO_IPV6, IPV6_RECVHOPLIMIT, &yes, sizeof(yes)) < 0)
		fprintf(stderr, "ioctl failed: IPV6_RECVHOPLIMIT\n");

	if (setsockopt(fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, &no, sizeof(no)) < 0)
		fprintf(stderr, "ioctl failed: IPV6_MULTICAST_LOOP\n");

	return 0;
}

static void
reconnect_socket(struct uloop_timeout *timeout)
{
	struct interface *iface = container_of(timeout, struct interface, reconnect);
	char mcast_addr[16];
	int type = 0;

	if (iface->v6) {
		snprintf(mcast_addr, sizeof(mcast_addr), "%s%%%s", iface->mcast_addr, iface->name);
		type = USOCK_IPV6ONLY;
	} else {
		snprintf(mcast_addr, sizeof(mcast_addr), "%s", iface->mcast_addr);
		type = USOCK_IPV4ONLY;
	}

	iface->fd.fd = usock(USOCK_UDP | USOCK_SERVER | USOCK_NONBLOCK | type, mcast_addr, "5353");
	if (iface->fd.fd < 0) {
		fprintf(stderr, "failed to add listener %s: %s\n", mcast_addr, strerror(errno));
		goto retry;
	}

	if (!iface->v6 && interface_socket_setup4(iface)) {
		iface->fd.fd = -1;
		goto retry;
	}

	if (iface->v6 && interface_socket_setup6(iface)) {
		iface->fd.fd = -1;
		goto retry;
	}

	uloop_fd_add(&iface->fd, ULOOP_READ);
	dns_send_question(iface, "_services._dns-sd._udp.local", TYPE_PTR);
	announce_init(iface);
	return;

retry:
	uloop_timeout_set(timeout, 1000);
}


static void interface_start(struct interface *iface)
{
	iface->fd.cb = read_socket;
	iface->reconnect.cb = reconnect_socket;
	uloop_timeout_set(&iface->reconnect, 100);
}

static void
iface_update_cb(struct vlist_tree *tree, struct vlist_node *node_new,
		struct vlist_node *node_old)
{
	struct interface *iface;

	if (node_old) {
		iface = container_of(node_old, struct interface, node);
		interface_free(iface);
	}

	if (node_new) {
		iface = container_of(node_new, struct interface, node);
		interface_start(iface);
	}
}

static int
get_iface_ipv4(struct interface *iface)
{
	struct sockaddr_in *sin;
	struct ifreq ir;
	int sock, ret = -1;

	if (cfg_proto && (cfg_proto != 4))
		return -1;

	sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0)
		return -1;

	memset(&ir, 0, sizeof(struct ifreq));
	strncpy(ir.ifr_name, iface->name, sizeof(ir.ifr_name));

	ret = ioctl(sock, SIOCGIFADDR, &ir);
	if (ret < 0)
		goto out;

	sin = (struct sockaddr_in *) &ir.ifr_addr;
	memcpy(&iface->v4_addr, &sin->sin_addr, sizeof(iface->v4_addr));
	iface->mcast_addr = MCAST_ADDR;
out:
	close(sock);
	return ret;
}

static int
get_iface_ipv6(struct interface *iface)
{
	struct sockaddr_in6 addr = {AF_INET6, 0, iface->ifindex, IN6ADDR_ANY_INIT, 0};
	socklen_t alen = sizeof(addr);
	int sock, ret = -1;

	if (cfg_proto && (cfg_proto != 6))
		return -1;

	addr.sin6_addr.s6_addr[0] = 0xff;
	addr.sin6_addr.s6_addr[1] = 0x02;
	addr.sin6_addr.s6_addr[15] = 0x01;

	sock = socket(AF_INET6, SOCK_RAW, IPPROTO_ICMPV6);
	connect(sock, (struct sockaddr*)&addr, sizeof(addr));
	ret = getsockname(sock, (struct sockaddr*)&addr, &alen);
	if (!ret) {
		memcpy(&iface->v6_addr, &addr.sin6_addr, sizeof(iface->v6_addr));
		iface->mcast_addr = MCAST_ADDR6;
		iface->v6 = 1;
	}
	close(sock);
	return ret;
}

static int _interface_add(const char *name, int v6)
{
	struct interface *iface;
	char *name_buf;
	char *id_buf;

	iface = calloc_a(sizeof(*iface),
		&name_buf, strlen(name) + 1,
		&id_buf, strlen(name) + 3);

	sprintf(id_buf, "%d_%s", v6, name);
	iface->name = strcpy(name_buf, name);
	iface->id = id_buf;
	iface->ifindex = if_nametoindex(name);
	iface->fd.fd = -1;

	if (iface->ifindex <= 0)
		goto error;

	if (!v6 && get_iface_ipv4(iface))
		goto error;

	if (v6 && get_iface_ipv6(iface))
		goto error;

	vlist_add(&interfaces, &iface->node, iface->id);
	return 0;

error:
	free(iface);
	return -1;
}

int interface_add(const char *name)
{
	int v4 = _interface_add(name, 0);
	int v6 = _interface_add(name, 1);

	return v4 && v6;
}

VLIST_TREE(interfaces, avl_strcmp, iface_update_cb, false, false);
