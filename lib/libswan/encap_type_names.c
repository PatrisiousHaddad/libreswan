/* encap type, for libreswan
 *
 * Copyright (C) 2021 Andrew Cagney
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

#include "encap_type.h"

#include "lswcdefs.h"		/* for ARRAY_REF() */
#include "enum_names.h"

static const char *encap_type_name[] = {
#define S(E) [E-ENCAP_TYPE_TRAP] = #E
	S(ENCAP_TYPE_TRAP),
	S(ENCAP_TYPE_IPSEC),
	S(ENCAP_TYPE_PASS),
	S(ENCAP_TYPE_DROP),
#undef S
};

const struct enum_names encap_type_names = {
	ENCAP_TYPE_TRAP,
	ENCAP_TYPE_DROP,
	ARRAY_REF(encap_type_name),
	.en_prefix = "ENCAP_TYPE_",
};
