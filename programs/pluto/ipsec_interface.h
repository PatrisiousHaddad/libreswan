/* ipsec-interface= structures, for libreswan
 *
 * Copyright (C) 2018-2020 Antony Antony <antony@phenome.org>
 * Copyright (C) 2023 Brady Johnson <bradyallenjohnson@gmail.com>
 * Copyright (C) Andrew Cagney
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.  See <https://www.gnu.org/licenses/gpl2.txt>.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */

#ifndef IPSEC_INTERFACE_H
#define IPSEC_INTERFACE_H

#include <net/if.h>		/* for IFNAMSIZ */
#include <stdbool.h>

#include "err.h"
#include "ip_cidr.h"
#include "refcnt.h"
#include "ip_endpoint.h"

struct connection;
struct logger;
struct ipsec_interface;	/* forward */

/*
 * The same interface IP can be used by multiple tunnels, with
 * different remote IPs, so they are ref-counted to control removing
 * the IP from the IF.
 */

struct ipsec_interface_address {
	refcnt_t refcnt;
	ip_cidr if_ip;
	bool pluto_added;
	struct ipsec_interface_address *next;
};

struct ipsec_interface_address *alloc_ipsec_interface_address(struct ipsec_interface *ipsec_if, ip_cidr if_ip);
void free_ipsec_interface_address_list(struct ipsec_interface_address *ipsec_ifaddr, struct logger *logger);

struct ipsec_interface {
	char *name;
	uint32_t if_id; /* IFLA_XFRM_IF_ID */
	uint32_t dev_if_id;  /* if_id of device, IFLA_XFRM_LINK */
	struct ipsec_interface_address *if_ips; /* ref-counted IPs on this IF */
	refcnt_t refcnt;
	bool shared;
	bool pluto_added;
	struct ipsec_interface *next;
};

/* Both add_ipsec_interface() return true on success, false otherwise */

diag_t setup_ipsec_interface(struct connection *c, const char *ipsec_interface);
bool add_ipsec_interface(const struct connection *c, struct logger *logger);
void remove_ipsec_interface(const struct connection *c, struct logger *logger);
void ipsec_interface_delref(struct connection *c);
void ipsec_interface_addref(struct connection *c);
struct ipsec_interface *find_ipsec_interface_by_id(uint32_t if_id);
void alloc_ipsec_interface(uint32_t if_id, bool shared, const char *name, struct connection *c);

void shutdown_kernel_ipsec_interface(struct logger *logger);
void check_stale_ipsec_interfaces(struct logger *logger);

/* utilities; may at some point be made static */
typedef struct {
	char buf[IFNAMSIZ+1/*NULL*/+1/*CANARY*/];
} ipsec_interface_id_buf;

size_t jam_ipsec_interface_id(struct jambuf *buf, uint32_t if_id);
char *str_ipsec_interface_id(uint32_t if_id, ipsec_interface_id_buf *buf);

#endif
