/*
  Copyright (c) 2012-2013, Matthias Schiffer <mschiffer@universe-factory.net>
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are met:

    1. Redistributions of source code must retain the above copyright notice,
       this list of conditions and the following disclaimer.
    2. Redistributions in binary form must reproduce the above copyright notice,
       this list of conditions and the following disclaimer in the documentation
       and/or other materials provided with the distribution.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
  DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
  FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
  SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
  OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/


#include "fastd.h"
#include "packet.h"
#include "peer.h"


static inline void add_pktinfo(struct msghdr *msg, const fastd_peer_address_t *local_addr) {
	if (!local_addr)
		return;

	struct cmsghdr *cmsg = (struct cmsghdr*)((char*)msg->msg_control + msg->msg_controllen);

	if (local_addr->sa.sa_family == AF_INET) {
		cmsg->cmsg_level = IPPROTO_IP;
		cmsg->cmsg_type = IP_PKTINFO;
		cmsg->cmsg_len = CMSG_LEN(sizeof(struct in_pktinfo));

		msg->msg_controllen += cmsg->cmsg_len;

		struct in_pktinfo *pktinfo = (struct in_pktinfo*)CMSG_DATA(cmsg);
		pktinfo->ipi_addr = local_addr->in.sin_addr;
	}
	else if (local_addr->sa.sa_family == AF_INET6) {
		cmsg->cmsg_level = IPPROTO_IPV6;
		cmsg->cmsg_type = IPV6_PKTINFO;
		cmsg->cmsg_len = CMSG_LEN(sizeof(struct in6_pktinfo));

		msg->msg_controllen += cmsg->cmsg_len;

		struct in6_pktinfo *pktinfo = (struct in6_pktinfo*)CMSG_DATA(cmsg);
		pktinfo->ipi6_addr = local_addr->in6.sin6_addr;

		if (IN6_IS_ADDR_LINKLOCAL(&local_addr->in6.sin6_addr))
			pktinfo->ipi6_ifindex = local_addr->in6.sin6_scope_id;
	}
}

static void send_type(fastd_context_t *ctx, const fastd_socket_t *sock, const fastd_peer_address_t *local_addr, const fastd_peer_address_t *remote_addr, uint8_t packet_type, fastd_buffer_t buffer) {
	if (!sock)
		exit_bug(ctx, "send: sock == NULL");

	struct msghdr msg = {};
	char cbuf[1024] = {};

	switch (remote_addr->sa.sa_family) {
	case AF_INET:
		msg.msg_name = (void*)&remote_addr->in;
		msg.msg_namelen = sizeof(struct sockaddr_in);
		break;

	case AF_INET6:
		msg.msg_name = (void*)&remote_addr->in6;
		msg.msg_namelen = sizeof(struct sockaddr_in6);
		break;

	default:
		exit_bug(ctx, "unsupported address family");
	}

	struct iovec iov[2] = {
		{ .iov_base = &packet_type, .iov_len = 1 },
		{ .iov_base = buffer.data, .iov_len = buffer.len }
	};

	msg.msg_iov = iov;
	msg.msg_iovlen = buffer.len ? 2 : 1;
	msg.msg_control = cbuf;
	msg.msg_controllen = 0;

	add_pktinfo(&msg, local_addr);

	int ret;
	do {
		ret = sendmsg(sock->fd, &msg, 0);
	} while (ret < 0 && errno == EINTR);

	if (ret < 0)
		pr_warn_errno(ctx, "sendmsg");

	fastd_buffer_free(buffer);
}

void fastd_send(fastd_context_t *ctx, const fastd_socket_t *sock, const fastd_peer_address_t *local_addr, const fastd_peer_address_t *remote_addr, fastd_buffer_t buffer) {
	send_type(ctx, sock, local_addr, remote_addr, PACKET_DATA, buffer);
}

void fastd_send_handshake(fastd_context_t *ctx, const fastd_socket_t *sock, const fastd_peer_address_t *local_addr, const fastd_peer_address_t *remote_addr, fastd_buffer_t buffer) {
	send_type(ctx, sock, local_addr, remote_addr, PACKET_HANDSHAKE, buffer);
}

void fastd_send_all(fastd_context_t *ctx, fastd_peer_t *source_peer, fastd_buffer_t buffer) {
	fastd_peer_t *dest_peer;
	for (dest_peer = ctx->peers; dest_peer; dest_peer = dest_peer->next) {
		if (dest_peer == source_peer || !fastd_peer_is_established(dest_peer))
			continue;

		/* optimization, primarily for TUN mode: don't duplicate the buffer for the last (or only) peer */
		if (!dest_peer->next) {
			ctx->conf->protocol->send(ctx, dest_peer, buffer);
			return;
		}

		ctx->conf->protocol->send(ctx, dest_peer, fastd_buffer_dup(ctx, buffer, ctx->conf->min_encrypt_head_space, ctx->conf->min_encrypt_tail_space));
	}

	fastd_buffer_free(buffer);
}