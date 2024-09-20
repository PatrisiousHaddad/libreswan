/* Kernel interace to IPsec Interface, for libreswan
 *
 * Copyright (C) 2024 Andrew Cagney
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
 *
 */

#ifndef KERNEL_IPSEC_INTERFACE_H
#define KERNEL_IPSEC_INTERFACE_H

struct ipsec_interface_address;
struct ipsec_interface;
struct verbose;

struct kernel_ipsec_interface {
	const char *name;
	/*
	 * On XFRMi IF_ID 0 is invalid; hence remap ipsec-interface=0
	 * to some other value; is this all about preserving old VTI
	 * code?
	 */
	uint32_t map_if_id_zero;

	/* placeholders */
	int (*ip_addr_del)(const char *if_name,
			   const struct ipsec_interface_address *xfrmi_ipaddr,
			   struct logger *logger);
	bool (*ip_addr_find_on_if)(struct ipsec_interface *xfrmi,
				   ip_cidr *search_ip,
				   struct logger *logger);
	bool (*ip_addr_add)(const char *if_name,
			    const struct ipsec_interface_address *xfrmi_ipaddr,
			    struct logger *logger);

	struct ipsec_interface_address *(*ip_addr_get_all_ips)(const char *if_name,
							       uint32_t if_id,
							       struct logger *logger);

	bool (*ip_link_add)(const char *if_name /*non-NULL*/,
			    const char *dev_name /*non-NULL*/,
			    const uint32_t if_id,
			    struct logger *logger);
	bool (*ip_link_set_up)(const char *if_name,
			       struct logger *logger);
	bool (*ip_link_del)(const char *if_name /*non-NULL*/,
			    const struct logger *logger);

	bool (*find_interface)(const char *if_name, /* optional */
			       uint32_t xfrm_if_id, /* 0 is wildcard */
			       struct verbose verbose);

	void (*check_stale_ipsec_interfaces)(struct logger *logger);
	err_t (*supported)(struct logger *logger);
	void (*shutdown)(struct logger *logger);
};

extern const struct kernel_ipsec_interface kernel_ipsec_interface_xfrm;
extern const struct kernel_ipsec_interface kernel_ipsec_interface_bsd;

#endif
