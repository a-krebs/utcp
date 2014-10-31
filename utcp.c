/*
    utcp.c -- Userspace TCP
    Copyright (C) 2014 Guus Sliepen <guus@tinc-vpn.org>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#define _GNU_SOURCE

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/socket.h>

#include "utcp_priv.h"

#ifndef EBADMSG
#define EBADMSG         104
#endif

#ifndef SHUT_RDWR
#define SHUT_RDWR 2
#endif

#ifdef poll
#undef poll
#endif

#ifndef timersub
#define timersub(a, b, r) do {\
	(r)->tv_sec = (a)->tv_sec - (b)->tv_sec;\
	(r)->tv_usec = (a)->tv_usec - (b)->tv_usec;\
	if((r)->tv_usec < 0)\
		(r)->tv_sec--, (r)->tv_usec += 1000000;\
} while (0)
#endif

#ifdef UTCP_DEBUG
#include <stdarg.h>

static void debug(const char *format, ...) {
	va_list ap;
	va_start(ap, format);
	vfprintf(stderr, format, ap);
	va_end(ap);
}

static void print_packet(struct utcp *utcp, const char *dir, const void *pkt, size_t len) {
	struct hdr hdr;
	if(len < sizeof hdr) {
		debug("%p %s: short packet (%zu bytes)\n", utcp, dir, len);
		return;
	}

	memcpy(&hdr, pkt, sizeof hdr);
	fprintf (stderr, "%p %s: len=%zu, src=%u dst=%u seq=%u ack=%u wnd=%u ctl=", utcp, dir, len, hdr.src, hdr.dst, hdr.seq, hdr.ack, hdr.wnd);
	if(hdr.ctl & SYN)
		debug("SYN");
	if(hdr.ctl & RST)
		debug("RST");
	if(hdr.ctl & FIN)
		debug("FIN");
	if(hdr.ctl & ACK)
		debug("ACK");

	if(len > sizeof hdr) {
		debug(" data=");
		for(int i = sizeof hdr; i < len; i++) {
			const char *data = pkt;
			debug("%c", data[i] >= 32 ? data[i] : '.');
		}
	}

	debug("\n");
}
#else
#define debug(...)
#define print_packet(...)
#endif

static void set_state(struct utcp_connection *c, enum state state) {
	c->state = state;
	if(state == ESTABLISHED)
		timerclear(&c->conn_timeout);
	debug("%p new state: %s\n", c->utcp, strstate[state]);
}

static inline void list_connections(struct utcp *utcp) {
	debug("%p has %d connections:\n", utcp, utcp->nconnections);
	for(int i = 0; i < utcp->nconnections; i++)
		debug("  %u -> %u state %s\n", utcp->connections[i]->src, utcp->connections[i]->dst, strstate[utcp->connections[i]->state]);
}

static int32_t seqdiff(uint32_t a, uint32_t b) {
	return a - b;
}

// Connections are stored in a sorted list.
// This gives O(log(N)) lookup time, O(N log(N)) insertion time and O(N) deletion time.

static int compare(const void *va, const void *vb) {
	assert(va && vb);

	const struct utcp_connection *a = *(struct utcp_connection **)va;
	const struct utcp_connection *b = *(struct utcp_connection **)vb;

	assert(a && b);
	assert(a->src && b->src);

	int c = (int)a->src - (int)b->src;
	if(c)
		return c;
	c = (int)a->dst - (int)b->dst;
	return c;
}

static struct utcp_connection *find_connection(const struct utcp *utcp, uint16_t src, uint16_t dst) {
	if(!utcp->nconnections)
		return NULL;
	struct utcp_connection key = {
		.src = src,
		.dst = dst,
	}, *keyp = &key;
	struct utcp_connection **match = bsearch(&keyp, utcp->connections, utcp->nconnections, sizeof *utcp->connections, compare);
	return match ? *match : NULL;
}

static void free_connection(struct utcp_connection *c) {
	struct utcp *utcp = c->utcp;
	struct utcp_connection **cp = bsearch(&c, utcp->connections, utcp->nconnections, sizeof *utcp->connections, compare);

	assert(cp);

	int i = cp - utcp->connections;
	memmove(cp, cp + 1, (utcp->nconnections - i - 1) * sizeof *cp);
	utcp->nconnections--;

	free(c->sndbuf);
	free(c);
}

static struct utcp_connection *allocate_connection(struct utcp *utcp, uint16_t src, uint16_t dst) {
	// Check whether this combination of src and dst is free

	if(src) {
		if(find_connection(utcp, src, dst)) {
			errno = EADDRINUSE;
			return NULL;
		}
	} else { // If src == 0, generate a random port number with the high bit set
		if(utcp->nconnections >= 32767) {
			errno = ENOMEM;
			return NULL;
		}
		src = rand() | 0x8000;
		while(find_connection(utcp, src, dst))
			src++;
	}

	// Allocate memory for the new connection

	if(utcp->nconnections >= utcp->nallocated) {
		if(!utcp->nallocated)
			utcp->nallocated = 4;
		else
			utcp->nallocated *= 2;
		struct utcp_connection **new_array = realloc(utcp->connections, utcp->nallocated * sizeof *utcp->connections);
		if(!new_array)
			return NULL;
		utcp->connections = new_array;
	}

	struct utcp_connection *c = calloc(1, sizeof *c);
	if(!c)
		return NULL;

	c->sndbufsize = DEFAULT_SNDBUFSIZE;
	c->maxsndbufsize = DEFAULT_MAXSNDBUFSIZE;
	c->sndbuf = malloc(c->sndbufsize);
	if(!c->sndbuf) {
		free(c);
		return NULL;
	}

	// Fill in the details

	c->src = src;
	c->dst = dst;
	c->snd.iss = rand();
	c->snd.una = c->snd.iss;
	c->snd.nxt = c->snd.iss + 1;
	c->rcv.wnd = utcp->mtu;
	c->snd.last = c->snd.nxt;
	c->snd.cwnd = utcp->mtu;
	c->utcp = utcp;

	// Add it to the sorted list of connections

	utcp->connections[utcp->nconnections++] = c;
	qsort(utcp->connections, utcp->nconnections, sizeof *utcp->connections, compare);

	return c;
}

struct utcp_connection *utcp_connect(struct utcp *utcp, uint16_t dst, utcp_recv_t recv, void *priv) {
	struct utcp_connection *c = allocate_connection(utcp, 0, dst);
	if(!c)
		return NULL;

	c->recv = recv;
	c->priv = priv;

	struct hdr hdr;

	hdr.src = c->src;
	hdr.dst = c->dst;
	hdr.seq = c->snd.iss;
	hdr.ack = 0;
	hdr.wnd = c->rcv.wnd;
	hdr.ctl = SYN;
	hdr.aux = 0;

	set_state(c, SYN_SENT);

	print_packet(utcp, "send", &hdr, sizeof hdr);
	utcp->send(utcp, &hdr, sizeof hdr);

	gettimeofday(&c->conn_timeout, NULL);
	c->conn_timeout.tv_sec += utcp->timeout;

	return c;
}

void utcp_accept(struct utcp_connection *c, utcp_recv_t recv, void *priv) {
	if(c->reapable || c->state != SYN_RECEIVED) {
		debug("Error: accept() called on invalid connection %p in state %s\n", c, strstate[c->state]);
		return;
	}

	debug("%p accepted, %p %p\n", c, recv, priv);
	c->recv = recv;
	c->priv = priv;
	set_state(c, ESTABLISHED);
}

static void ack(struct utcp_connection *c, bool sendatleastone) {
	int32_t left = seqdiff(c->snd.last, c->snd.nxt);
	int32_t cwndleft = c->snd.cwnd - seqdiff(c->snd.nxt, c->snd.una);
	char *data = c->sndbuf + seqdiff(c->snd.nxt, c->snd.una);

	assert(left >= 0);

	if(cwndleft <= 0)
		cwndleft = 0;

	if(cwndleft < left)
		left = cwndleft;

	if(!left && !sendatleastone)
		return;

	struct {
		struct hdr hdr;
		char data[];
	} *pkt;

	pkt = malloc(sizeof pkt->hdr + c->utcp->mtu);
	if(!pkt->data)
		return;

	pkt->hdr.src = c->src;
	pkt->hdr.dst = c->dst;
	pkt->hdr.ack = c->rcv.nxt;
	pkt->hdr.wnd = c->snd.wnd;
	pkt->hdr.ctl = ACK;
	pkt->hdr.aux = 0;

	do {
		uint32_t seglen = left > c->utcp->mtu ? c->utcp->mtu : left;
		pkt->hdr.seq = c->snd.nxt;

		memcpy(pkt->data, data, seglen);

		c->snd.nxt += seglen;
		data += seglen;
		left -= seglen;

		if(c->state != ESTABLISHED && !left && seglen) {
			switch(c->state) {
			case FIN_WAIT_1:
			case CLOSING:
				seglen--;
				pkt->hdr.ctl |= FIN;
				break;
			default:
				break;
			}
		}

		print_packet(c->utcp, "send", pkt, sizeof pkt->hdr + seglen);
		c->utcp->send(c->utcp, pkt, sizeof pkt->hdr + seglen);
	} while(left);

	free(pkt);
}

ssize_t utcp_send(struct utcp_connection *c, const void *data, size_t len) {
	if(c->reapable) {
		debug("Error: send() called on closed connection %p\n", c);
		errno = EBADF;
		return -1;
	}

	switch(c->state) {
	case CLOSED:
	case LISTEN:
	case SYN_SENT:
	case SYN_RECEIVED:
		debug("Error: send() called on unconnected connection %p\n", c);
		errno = ENOTCONN;
		return -1;
	case ESTABLISHED:
	case CLOSE_WAIT:
		break;
	case FIN_WAIT_1:
	case FIN_WAIT_2:
	case CLOSING:
	case LAST_ACK:
	case TIME_WAIT:
		debug("Error: send() called on closing connection %p\n", c);
		errno = EPIPE;
		return -1;
	}

	// Add data to send buffer

	if(!len)
		return 0;

	if(!data) {
		errno = EFAULT;
		return -1;
	}

	uint32_t bufused = seqdiff(c->snd.nxt, c->snd.una);

	/* Check our send buffer.
	 * - If it's big enough, just put the data in there.
	 * - If not, decide whether to enlarge if possible.
	 * - Cap len so it doesn't overflow our buffer.
	 */

	if(len > c->sndbufsize - bufused && c->sndbufsize < c->maxsndbufsize) {
		uint32_t newbufsize;
		if(c->sndbufsize > c->maxsndbufsize / 2)
			newbufsize = c->maxsndbufsize;
		else
			newbufsize = c->sndbufsize * 2;
		if(bufused + len > newbufsize) {
			if(bufused + len > c->maxsndbufsize)
				newbufsize = c->maxsndbufsize;
			else
				newbufsize = bufused + len;
		}
		char *newbuf = realloc(c->sndbuf, newbufsize);
		if(newbuf) {
			c->sndbuf = newbuf;
			c->sndbufsize = newbufsize;
		}
	}

	if(len > c->sndbufsize - bufused)
		len = c->sndbufsize - bufused;

	if(!len) {
		errno == EWOULDBLOCK;
		return 0;
	}

	memcpy(c->sndbuf + bufused, data, len);
	c->snd.last += len;

	ack(c, false);
	return len;
}

static void swap_ports(struct hdr *hdr) {
	uint16_t tmp = hdr->src;
	hdr->src = hdr->dst;
	hdr->dst = tmp;
}

ssize_t utcp_recv(struct utcp *utcp, const void *data, size_t len) {
	if(!utcp) {
		errno = EFAULT;
		return -1;
	}

	if(!len)
		return 0;

	if(!data) {
		errno = EFAULT;
		return -1;
	}

	print_packet(utcp, "recv", data, len);

	// Drop packets smaller than the header

	struct hdr hdr;
	if(len < sizeof hdr) {
		errno = EBADMSG;
		return -1;
	}

	// Make a copy from the potentially unaligned data to a struct hdr

	memcpy(&hdr, data, sizeof hdr);
	data += sizeof hdr;
	len -= sizeof hdr;

	// Drop packets with an unknown CTL flag

	if(hdr.ctl & ~(SYN | ACK | RST | FIN)) {
		errno = EBADMSG;
		return -1;
	}

	// Try to match the packet to an existing connection

	struct utcp_connection *c = find_connection(utcp, hdr.dst, hdr.src);

	// Is it for a new connection?

	if(!c) {
		// Ignore RST packets

		if(hdr.ctl & RST)
			return 0;

		// Is it a SYN packet and are we LISTENing?

		if(hdr.ctl & SYN && !(hdr.ctl & ACK) && utcp->accept) {
			// If we don't want to accept it, send a RST back
			if((utcp->pre_accept && !utcp->pre_accept(utcp, hdr.dst))) {
				len = 1;
				goto reset;
			}

			// Try to allocate memory, otherwise send a RST back
			c = allocate_connection(utcp, hdr.dst, hdr.src);
			if(!c) {
				len = 1;
				goto reset;
			}

			// Return SYN+ACK, go to SYN_RECEIVED state
			c->snd.wnd = hdr.wnd;
			c->rcv.irs = hdr.seq;
			c->rcv.nxt = c->rcv.irs + 1;
			set_state(c, SYN_RECEIVED);

			hdr.dst = c->dst;
			hdr.src = c->src;
			hdr.ack = c->rcv.irs + 1;
			hdr.seq = c->snd.iss;
			hdr.ctl = SYN | ACK;
			print_packet(c->utcp, "send", &hdr, sizeof hdr);
			utcp->send(utcp, &hdr, sizeof hdr);
		} else {
			// No, we don't want your packets, send a RST back
			len = 1;
			goto reset;
		}

		return 0;
	}

	debug("%p state %s\n", c->utcp, strstate[c->state]);

	// In case this is for a CLOSED connection, ignore the packet.
	// TODO: make it so incoming packets can never match a CLOSED connection.

	if(c->state == CLOSED)
		return 0;

	// It is for an existing connection.

	// 1. Drop invalid packets.

	// 1a. Drop packets that should not happen in our current state.

	switch(c->state) {
	case SYN_SENT:
	case SYN_RECEIVED:
	case ESTABLISHED:
	case FIN_WAIT_1:
	case FIN_WAIT_2:
	case CLOSE_WAIT:
	case CLOSING:
	case LAST_ACK:
	case TIME_WAIT:
		break;
	default:
		abort();
	}

	// 1b. Drop packets with a sequence number not in our receive window.

	bool acceptable;

	if(c->state == SYN_SENT)
		acceptable = true;

	// TODO: handle packets overlapping c->rcv.nxt.
#if 0
	// Only use this when accepting out-of-order packets.
	else if(len == 0)
		if(c->rcv.wnd == 0)
			acceptable = hdr.seq == c->rcv.nxt;
		else
			acceptable = (seqdiff(hdr.seq, c->rcv.nxt) >= 0 && seqdiff(hdr.seq, c->rcv.nxt + c->rcv.wnd) < 0);
	else
		if(c->rcv.wnd == 0)
			// We don't accept data when the receive window is zero.
			acceptable = false;
		else
			// Both start and end of packet must be within the receive window
			acceptable = (seqdiff(hdr.seq, c->rcv.nxt) >= 0 && seqdiff(hdr.seq, c->rcv.nxt + c->rcv.wnd) < 0)
				|| (seqdiff(hdr.seq + len + 1, c->rcv.nxt) >= 0 && seqdiff(hdr.seq + len - 1, c->rcv.nxt + c->rcv.wnd) < 0);
#else
	if(c->state != SYN_SENT)
		acceptable = hdr.seq == c->rcv.nxt;
#endif

	if(!acceptable) {
		debug("Packet not acceptable, %u  <= %u + %zu < %u\n", c->rcv.nxt, hdr.seq, len, c->rcv.nxt + c->rcv.wnd);
		// Ignore unacceptable RST packets.
		if(hdr.ctl & RST)
			return 0;
		// Otherwise, send an ACK back in the hope things improve.
		goto ack;
	}

	c->snd.wnd = hdr.wnd; // TODO: move below

	// 1c. Drop packets with an invalid ACK.
	// ackno should not roll back, and it should also not be bigger than snd.nxt.

	if(hdr.ctl & ACK && (seqdiff(hdr.ack, c->snd.nxt) > 0 || seqdiff(hdr.ack, c->snd.una) < 0)) {
		debug("Packet ack seqno out of range, %u %u %u\n", hdr.ack, c->snd.una, c->snd.nxt);
		// Ignore unacceptable RST packets.
		if(hdr.ctl & RST)
			return 0;
		goto reset;
	}

	// 2. Handle RST packets

	if(hdr.ctl & RST) {
		switch(c->state) {
		case SYN_SENT:
			if(!(hdr.ctl & ACK))
				return 0;
			// The peer has refused our connection.
			set_state(c, CLOSED);
			errno = ECONNREFUSED;
			if(c->recv)
				c->recv(c, NULL, 0);
			return 0;
		case SYN_RECEIVED:
			if(hdr.ctl & ACK)
				return 0;
			// We haven't told the application about this connection yet. Silently delete.
			free_connection(c);
			return 0;
		case ESTABLISHED:
		case FIN_WAIT_1:
		case FIN_WAIT_2:
		case CLOSE_WAIT:
			if(hdr.ctl & ACK)
				return 0;
			// The peer has aborted our connection.
			set_state(c, CLOSED);
			errno = ECONNRESET;
			if(c->recv)
				c->recv(c, NULL, 0);
			return 0;
		case CLOSING:
		case LAST_ACK:
		case TIME_WAIT:
			if(hdr.ctl & ACK)
				return 0;
			// As far as the application is concerned, the connection has already been closed.
			// If it has called utcp_close() already, we can immediately free this connection.
			if(c->reapable) {
				free_connection(c);
				return 0;
			}
			// Otherwise, immediately move to the CLOSED state.
			set_state(c, CLOSED);
			return 0;
		default:
			abort();
		}
	}

	// 3. Advance snd.una

	uint32_t advanced = seqdiff(hdr.ack, c->snd.una);
	uint32_t prevrcvnxt = c->rcv.nxt;

	if(advanced) {
		int32_t data_acked = advanced;

		switch(c->state) {
			case SYN_SENT:
			case SYN_RECEIVED:
				data_acked--;
				break;
			// TODO: handle FIN as well.
			default:
				break;
		}

		assert(data_acked >= 0);

		int32_t bufused = seqdiff(c->snd.last, c->snd.una);
		assert(data_acked <= bufused);

		// Make room in the send buffer.
		// TODO: try to avoid memmoving too much. Circular buffer?
		uint32_t left = bufused - data_acked;
		if(data_acked && left)
			memmove(c->sndbuf, c->sndbuf + data_acked, left);

		c->snd.una = hdr.ack;

		c->dupack = 0;
		c->snd.cwnd += utcp->mtu;
		if(c->snd.cwnd > c->maxsndbufsize)
			c->snd.cwnd = c->maxsndbufsize;

		// Check if we have sent a FIN that is now ACKed.
		switch(c->state) {
		case FIN_WAIT_1:
			if(c->snd.una == c->snd.last)
				set_state(c, FIN_WAIT_2);
			break;
		case CLOSING:
			if(c->snd.una == c->snd.last) {
				gettimeofday(&c->conn_timeout, NULL);
				c->conn_timeout.tv_sec += 60;
				set_state(c, TIME_WAIT);
			}
			break;
		default:
			break;
		}
	} else {
		if(!len) {
			c->dupack++;
			if(c->dupack >= 3) {
				debug("Triplicate ACK\n");
				//TODO: Resend one packet and go to fast recovery mode. See RFC 6582.
				//abort();
			}
		}
	}

	// 4. Update timers

	if(advanced) {
		timerclear(&c->conn_timeout); // It will be set anew in utcp_timeout() if c->snd.una != c->snd.nxt.
		if(c->snd.una == c->snd.nxt)
			timerclear(&c->rtrx_timeout);
	}

	// 5. Process SYN stuff

	if(hdr.ctl & SYN) {
		switch(c->state) {
		case SYN_SENT:
			// This is a SYNACK. It should always have ACKed the SYN.
			if(!advanced)
				goto reset;
			c->rcv.irs = hdr.seq;
			c->rcv.nxt = hdr.seq;
			set_state(c, ESTABLISHED);
			// TODO: notify application of this somehow.
			break;
		case SYN_RECEIVED:
		case ESTABLISHED:
		case FIN_WAIT_1:
		case FIN_WAIT_2:
		case CLOSE_WAIT:
		case CLOSING:
		case LAST_ACK:
		case TIME_WAIT:
			// Ehm, no. We should never receive a second SYN.
			goto reset;
		default:
			abort();
		}

		// SYN counts as one sequence number
		c->rcv.nxt++;
	}

	// 6. Process new data

	if(c->state == SYN_RECEIVED) {
		// This is the ACK after the SYNACK. It should always have ACKed the SYNACK.
		if(!advanced)
			goto reset;

		// Are we still LISTENing?
		if(utcp->accept)
			utcp->accept(c, c->src);

		if(c->state != ESTABLISHED) {
			set_state(c, CLOSED);
			c->reapable = true;
			goto reset;
		}
	}

	if(len) {
		switch(c->state) {
		case SYN_SENT:
		case SYN_RECEIVED:
			// This should never happen.
			abort();
		case ESTABLISHED:
		case FIN_WAIT_1:
		case FIN_WAIT_2:
			break;
		case CLOSE_WAIT:
		case CLOSING:
		case LAST_ACK:
		case TIME_WAIT:
			// Ehm no, We should never receive more data after a FIN.
			goto reset;
		default:
			abort();
		}

		ssize_t rxd;

		if(c->recv) {
			rxd = c->recv(c, data, len);
			if(rxd != len) {
				// TODO: once we have a receive buffer, handle the application not accepting all data.
				fprintf(stderr, "c->recv(%p, %p, %zu) returned %zd\n", c, data, len, rxd);
				abort();
			}
			if(rxd < 0)
				rxd = 0;
			else if(rxd > len)
				rxd = len; // Bad application, bad!
		} else {
			rxd = len;
		}

		c->rcv.nxt += len;
	}

	// 7. Process FIN stuff

	if(hdr.ctl & FIN) {
		switch(c->state) {
		case SYN_SENT:
		case SYN_RECEIVED:
			// This should never happen.
			abort();
		case ESTABLISHED:
			set_state(c, CLOSE_WAIT);
			break;
		case FIN_WAIT_1:
			set_state(c, CLOSING);
			break;
		case FIN_WAIT_2:
			gettimeofday(&c->conn_timeout, NULL);
			c->conn_timeout.tv_sec += 60;
			set_state(c, TIME_WAIT);
			break;
		case CLOSE_WAIT:
		case CLOSING:
		case LAST_ACK:
		case TIME_WAIT:
			// Ehm, no. We should never receive a second FIN.
			goto reset;
		default:
			abort();
		}

		// FIN counts as one sequence number
		c->rcv.nxt++;
		len++;

		// Inform the application that the peer closed the connection.
		if(c->recv) {
			errno = 0;
			c->recv(c, NULL, 0);
		}
	}

	// Now we send something back if:
	// - we advanced rcv.nxt (ie, we got some data that needs to be ACKed)
	//   -> sendatleastone = true
	// - or we got an ack, so we should maybe send a bit more data
	//   -> sendatleastone = false

ack:
	ack(c, prevrcvnxt != c->rcv.nxt);
	return 0;

reset:
	swap_ports(&hdr);
	hdr.wnd = 0;
	if(hdr.ctl & ACK) {
		hdr.seq = hdr.ack;
		hdr.ctl = RST;
	} else {
		hdr.ack = hdr.seq + len;
		hdr.seq = 0;
		hdr.ctl = RST | ACK;
	}
	print_packet(utcp, "send", &hdr, sizeof hdr);
	utcp->send(utcp, &hdr, sizeof hdr);
	return 0;

}

int utcp_shutdown(struct utcp_connection *c, int dir) {
	debug("%p shutdown %d\n", c ? c->utcp : NULL, dir);
	if(!c) {
		errno = EFAULT;
		return -1;
	}

	if(c->reapable) {
		debug("Error: shutdown() called on closed connection %p\n", c);
		errno = EBADF;
		return -1;
	}

	// TODO: handle dir

	switch(c->state) {
	case CLOSED:
		return 0;
	case LISTEN:
	case SYN_SENT:
		set_state(c, CLOSED);
		return 0;

	case SYN_RECEIVED:
	case ESTABLISHED:
		set_state(c, FIN_WAIT_1);
		break;
	case FIN_WAIT_1:
	case FIN_WAIT_2:
		return 0;
	case CLOSE_WAIT:
		set_state(c, CLOSING);
		break;

	case CLOSING:
	case LAST_ACK:
	case TIME_WAIT:
		return 0;
	}

	c->snd.last++;

	ack(c, false);
	return 0;
}

int utcp_close(struct utcp_connection *c) {
	if(utcp_shutdown(c, SHUT_RDWR))
		return -1;
	c->reapable = true;
	return 0;
}

int utcp_abort(struct utcp_connection *c) {
	if(!c) {
		errno = EFAULT;
		return -1;
	}

	if(c->reapable) {
		debug("Error: abort() called on closed connection %p\n", c);
		errno = EBADF;
		return -1;
	}

	c->reapable = true;

	switch(c->state) {
	case CLOSED:
		return 0;
	case LISTEN:
	case SYN_SENT:
	case CLOSING:
	case LAST_ACK:
	case TIME_WAIT:
		set_state(c, CLOSED);
		return 0;

	case SYN_RECEIVED:
	case ESTABLISHED:
	case FIN_WAIT_1:
	case FIN_WAIT_2:
	case CLOSE_WAIT:
		set_state(c, CLOSED);
		break;
	}

	// Send RST

	struct hdr hdr;

	hdr.src = c->src;
	hdr.dst = c->dst;
	hdr.seq = c->snd.nxt;
	hdr.ack = 0;
	hdr.wnd = 0;
	hdr.ctl = RST;

	print_packet(c->utcp, "send", &hdr, sizeof hdr);
	c->utcp->send(c->utcp, &hdr, sizeof hdr);
	return 0;
}

static void retransmit(struct utcp_connection *c) {
	if(c->state == CLOSED || c->snd.nxt == c->snd.una)
		return;

	struct utcp *utcp = c->utcp;

	struct {
		struct hdr hdr;
		char data[];
	} *pkt;

	pkt = malloc(sizeof pkt->hdr + c->utcp->mtu);
	if(!pkt)
		return;

	pkt->hdr.src = c->src;
	pkt->hdr.dst = c->dst;

	switch(c->state) {
		case LISTEN:
			// TODO: this should not happen
			break;

		case SYN_SENT:
			pkt->hdr.seq = c->snd.iss;
			pkt->hdr.ack = 0;
			pkt->hdr.wnd = c->rcv.wnd;
			pkt->hdr.ctl = SYN;
			print_packet(c->utcp, "rtrx", pkt, sizeof pkt->hdr);
			utcp->send(utcp, pkt, sizeof pkt->hdr);
			break;

		case SYN_RECEIVED:
			pkt->hdr.seq = c->snd.nxt;
			pkt->hdr.ack = c->rcv.nxt;
			pkt->hdr.ctl = SYN | ACK;
			print_packet(c->utcp, "rtrx", pkt, sizeof pkt->hdr);
			utcp->send(utcp, pkt, sizeof pkt->hdr);
			break;

		case ESTABLISHED:
		case FIN_WAIT_1:
			pkt->hdr.seq = c->snd.una;
			pkt->hdr.ack = c->rcv.nxt;
			pkt->hdr.ctl = ACK;
			uint32_t len = seqdiff(c->snd.nxt, c->snd.una);
			if(c->state == FIN_WAIT_1)
				len--;
			if(len > utcp->mtu)
				len = utcp->mtu;
			else {
				if(c->state == FIN_WAIT_1)
					pkt->hdr.ctl |= FIN;
			}
			memcpy(pkt->data, c->sndbuf, len);
			print_packet(c->utcp, "rtrx", pkt, sizeof pkt->hdr + len);
			utcp->send(utcp, pkt, sizeof pkt->hdr + len);
			break;

		default:
			// TODO: implement
			abort();
	}

	free(pkt);
}

/* Handle timeouts.
 * One call to this function will loop through all connections,
 * checking if something needs to be resent or not.
 * The return value is the time to the next timeout in milliseconds,
 * or maybe a negative value if the timeout is infinite.
 */
int utcp_timeout(struct utcp *utcp) {
	struct timeval now;
	gettimeofday(&now, NULL);
	struct timeval next = {now.tv_sec + 3600, now.tv_usec};

	for(int i = 0; i < utcp->nconnections; i++) {
		struct utcp_connection *c = utcp->connections[i];
		if(!c)
			continue;

		if(c->state == CLOSED) {
			if(c->reapable) {
				debug("Reaping %p\n", c);
				free_connection(c);
				i--;
			}
			continue;
		}

		if(timerisset(&c->conn_timeout) && timercmp(&c->conn_timeout, &now, <)) {
			errno = ETIMEDOUT;
			c->state = CLOSED;
			if(c->recv)
				c->recv(c, NULL, 0);
			continue;
		}

		if(timerisset(&c->rtrx_timeout) && timercmp(&c->rtrx_timeout, &now, <)) {
			retransmit(c);
		}

		if(c->poll && c->sndbufsize < c->maxsndbufsize / 2 && (c->state == ESTABLISHED || c->state == CLOSE_WAIT))
			c->poll(c, c->maxsndbufsize - c->sndbufsize);

		if(timerisset(&c->conn_timeout) && timercmp(&c->conn_timeout, &next, <))
			next = c->conn_timeout;

		if(c->snd.nxt != c->snd.una) {
			c->rtrx_timeout = now;
			c->rtrx_timeout.tv_sec++;
		} else {
			timerclear(&c->rtrx_timeout);
		}

		if(timerisset(&c->rtrx_timeout) && timercmp(&c->rtrx_timeout, &next, <))
			next = c->rtrx_timeout;
	}

	struct timeval diff;
	timersub(&next, &now, &diff);
	if(diff.tv_sec < 0)
		return 0;
	return diff.tv_sec * 1000 + diff.tv_usec / 1000;
}

struct utcp *utcp_init(utcp_accept_t accept, utcp_pre_accept_t pre_accept, utcp_send_t send, void *priv) {
	struct utcp *utcp = calloc(1, sizeof *utcp);
	if(!utcp)
		return NULL;

	if(!send) {
		errno = EFAULT;
		return NULL;
	}

	utcp->accept = accept;
	utcp->pre_accept = pre_accept;
	utcp->send = send;
	utcp->priv = priv;
	utcp->mtu = 1000;
	utcp->timeout = 60;

	return utcp;
}

void utcp_exit(struct utcp *utcp) {
	if(!utcp)
		return;
	for(int i = 0; i < utcp->nconnections; i++) {
		if(!utcp->connections[i]->reapable)
			debug("Warning, freeing unclosed connection %p\n", utcp->connections[i]);
		free(utcp->connections[i]->sndbuf);
		free(utcp->connections[i]);
	}
	free(utcp->connections);
	free(utcp);
}

uint16_t utcp_get_mtu(struct utcp *utcp) {
	return utcp->mtu;
}

void utcp_set_mtu(struct utcp *utcp, uint16_t mtu) {
	// TODO: handle overhead of the header
	utcp->mtu = mtu;
}

int utcp_get_user_timeout(struct utcp *u) {
	return u->timeout;
}

void utcp_set_user_timeout(struct utcp *u, int timeout) {
	u->timeout = timeout;
}

size_t utcp_get_sndbuf(struct utcp_connection *c) {
	return c->maxsndbufsize;
}

size_t utcp_get_sndbuf_free(struct utcp_connection *c) {
	return c->maxsndbufsize - c->sndbufsize;
}

void utcp_set_sndbuf(struct utcp_connection *c, size_t size) {
	c->maxsndbufsize = size;
	if(c->maxsndbufsize != size)
		c->maxsndbufsize = -1;
}

bool utcp_get_nodelay(struct utcp_connection *c) {
	return c->nodelay;
}

void utcp_set_nodelay(struct utcp_connection *c, bool nodelay) {
	c->nodelay = nodelay;
}

bool utcp_get_keepalive(struct utcp_connection *c) {
	return c->keepalive;
}

void utcp_set_keepalive(struct utcp_connection *c, bool keepalive) {
	c->keepalive = keepalive;
}

size_t utcp_get_outq(struct utcp_connection *c) {
	return seqdiff(c->snd.nxt, c->snd.una);
}

void utcp_set_recv_cb(struct utcp_connection *c, utcp_recv_t recv) {
	c->recv = recv;
}

void utcp_set_poll_cb(struct utcp_connection *c, utcp_poll_t poll) {
	c->poll = poll;
}
