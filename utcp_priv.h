/*
    utcp.h -- Userspace TCP
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

#ifndef UTCP_PRIV_H
#define UTCP_PRIV_H

#define UTCP_INTERNAL
#include "utcp.h"

#define PREP(l) char pkt[(l) + sizeof struct hdr]; struct hdr *hdr = &pkt;

#define SYN 1
#define ACK 2
#define FIN 4
#define RST 8

struct hdr {
	uint16_t src; // Source port
	uint16_t dst; // Destination port
	uint32_t seq; // Sequence number
	uint32_t ack; // Acknowledgement number
	uint32_t wnd; // Window size
	uint16_t ctl; // Flags (SYN, ACK, FIN, RST)
	uint16_t aux; // other stuff
};

enum state {
	CLOSED,
	LISTEN,
	SYN_SENT,
	SYN_RECEIVED,
	ESTABLISHED,
	FIN_WAIT_1,
	FIN_WAIT_2,
	CLOSE_WAIT,
	CLOSING,
	LAST_ACK,
	TIME_WAIT
};

static const char *strstate[] = {
	[CLOSED] = "CLOSED",
	[LISTEN] = "LISTEN",
	[SYN_SENT] = "SYN_SENT",
	[SYN_RECEIVED] = "SYN_RECEIVED",
	[ESTABLISHED] = "ESTABLISHED",
	[FIN_WAIT_1] = "FIN_WAIT_1",
	[FIN_WAIT_2] = "FIN_WAIT_2",
	[CLOSE_WAIT] = "CLOSE_WAIT",
	[CLOSING] = "CLOSING",
	[LAST_ACK] = "LAST_ACK",
	[TIME_WAIT] = "TIME_WAIT"
};

struct utcp_connection {
	void *priv;
	struct utcp *utcp;
	bool reapable;

	uint16_t src;
	uint16_t dst;
	enum state state;

	// The following two structures form the TCB

	struct {
		uint32_t una;
		uint32_t nxt;
		uint32_t wnd;
		uint32_t iss;
	} snd;

	struct {
		uint32_t nxt;
		uint32_t wnd;
		uint32_t irs;
	} rcv;

	utcp_recv_t recv;

	struct timeval conn_timeout;
	struct timeval rtrx_timeout;

	char *sndbuf;
	uint32_t sndbufsize;
};

struct utcp {
	void *priv;

	utcp_accept_t accept;
	utcp_pre_accept_t pre_accept;
	utcp_send_t send;

	uint16_t mtu;
	int timeout;

	struct utcp_connection **connections;
	int nconnections;
	int nallocated;
};

#endif