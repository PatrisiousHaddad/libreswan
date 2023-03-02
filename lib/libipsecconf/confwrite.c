/*
 * Libreswan config file writer (confwrite.c)
 * Copyright (C) 2004-2006 Michael Richardson <mcr@xelerance.com>
 * Copyright (C) 2012-2019 Paul Wouters <pwouters@redhat.com>
 * Copyright (C) 2013-2015 Antony Antony <antony@phenome.org>
 * Copyright (C) 2013-2019 D. Hugh Redelmeier <hugh@mimosa.com>
 * Copyright (C) 2013 David McCullough <ucdevel@gmail.com>
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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <assert.h>

#include "constants.h"
#include "lswlog.h"
#include "lmod.h"
#include "ip_address.h"
#include "sparse_names.h"

#include "ipsecconf/confread.h"
#include "ipsecconf/confwrite.h"
#include "ipsecconf/keywords.h"

#include "ipsecconf/keywords.h"
#include "ipsecconf/parser.h"	/* includes parser.tab.h generated by bison; requires keywords.h */

void confwrite_list(FILE *out, char *prefix, int val, const struct keyword_def *k)
{
	char *sep = "";

	for (const struct sparse_name *kev  = k->validenum; kev->name != NULL; kev++) {
		unsigned int mask = kev->value;

		if (mask != 0 && (val & mask) == mask) {
			fprintf(out, "%s%s%s", sep, prefix, kev->name);
			sep = " ";
		}
	}
}

static void confwrite_int(FILE *out,
			  const char *side,
			  unsigned int context,
			  knf options,
			  int_set options_set,
			  ksf strings)
{
	const struct keyword_def *k;

	for (k = ipsec_conf_keywords; k->keyname != NULL; k++) {
		if ((k->validity & KV_CONTEXT_MASK) != context)
			continue;

		/* do not output aliases or things handled elsewhere */
		if (k->validity & (kv_alias | kv_policy | kv_processed))
			continue;

#if 0
		printf("#side: %s  %s validity: %08x & %08x=%08x vs %08x\n",
		       side,
		       k->keyname, k->validity, KV_CONTEXT_MASK,
		       k->validity & KV_CONTEXT_MASK, context);
#endif

		switch (k->type) {
		case kt_string:
		case kt_appendstring:
		case kt_appendlist:
		case kt_filename:
		case kt_dirname:
		case kt_pubkey:

		case kt_percent:
		case kt_ipaddr:
		case kt_subnet:
		case kt_range:
		case kt_idtype:
		case kt_bitstring:
			/* none of these are valid number types */
			break;

		case kt_bool:
		case kt_invertbool:
			/* special enumeration */
			if (options_set[k->field]) {
				fprintf(out, "\t%s%s=%s\n", side,
					k->keyname, options[k->field] ? "yes" : "no");
			}
			break;

		case kt_enum:
		case kt_loose_enum:
			/* special enumeration */
			if (options_set[k->field]) {
				int val = options[k->field];
				fprintf(out, "\t%s%s=", side, k->keyname);

				if (k->type == kt_loose_enum &&
				    val == LOOSE_ENUM_OTHER) {
					fprintf(out, "%s\n",
						strings[k->field]);
				} else {
					for (const struct sparse_name *kev = k->validenum;
					     kev->name != NULL; kev++) {
						/* XXX: INT vs UNSIGNED magic? */
						if ((int)kev->value == val) {
							break;
						}
					}
					fprintf(out, "\n");
				}
			}
			break;

		case kt_list:
			/* special enumeration */
			if (options_set[k->field]) {
				int val = options[k->field];

				if (val != 0) {
					fprintf(out, "\t%s%s=\"", side, k->keyname);
					confwrite_list(out, "", val, k);

					fprintf(out, "\"\n");
				}
			}
			break;

		case kt_lset:
			if (options_set[k->field]) {
				unsigned long val = options[k->field];

				if (val != 0) {
					JAMBUF(buf) {
						jam_lset_short(buf, k->info->names, ",", val);
						fprintf(out, "\t%s%s=\""PRI_SHUNK"\"\n",
							side, k->keyname,
							pri_shunk(jambuf_as_shunk(buf)));
					}
				}
			}
			break;

		case kt_comment:
			break;

		case kt_obsolete:
		case kt_obsolete_quiet:
			break;

		case kt_time: /* special number, but do work later XXX */
		case kt_binary:
		case kt_byte:
		case kt_number:
			if (options_set[k->field])
				fprintf(out, "\t%s%s=%jd\n", side, k->keyname,
					options[k->field]);
		}
	}
}

static void confwrite_str(FILE *out,
			  const char *side,
			  unsigned int context,
			  ksf strings,
			  str_set strings_set)
{
	const struct keyword_def *k;

	for (k = ipsec_conf_keywords; k->keyname != NULL; k++) {
		if ((k->validity & KV_CONTEXT_MASK) != context)
			continue;

		/* do not output aliases or settings handled elsewhere */
		if (k->validity & (kv_alias | kv_policy | kv_processed))
			continue;

		switch (k->type) {
		case kt_appendlist:
			if (strings_set[k->field])
				fprintf(out, "\t%s%s={%s}\n", side, k->keyname,
					strings[k->field]);
			break;

		case kt_string:
		case kt_appendstring:
		case kt_filename:
		case kt_dirname:
			/* these are strings */

			if (strings_set[k->field]) {
				const char *quote =
					strchr(strings[k->field], ' ') == NULL ?
						"" : "\"";

				fprintf(out, "\t%s%s=%s%s%s\n", side, k->keyname,
					quote,
					strings[k->field],
					quote);
			}
			break;

		case kt_pubkey:
		case kt_ipaddr:
		case kt_range:
		case kt_subnet:
		case kt_idtype:
		case kt_bitstring:
			break;

		case kt_bool:
		case kt_invertbool:
		case kt_enum:
		case kt_list:
		case kt_lset:
		case kt_loose_enum:
			/* special enumeration */
			break;

		case kt_time:
		case kt_binary:
		case kt_byte:
			/* special number, not a string */
			break;

		case kt_percent:
		case kt_number:
			break;

		case kt_comment:
			break;

		case kt_obsolete:
		case kt_obsolete_quiet:
			break;
		}
	}
}

static void confwrite_side(FILE *out, struct starter_end *end)
{
	const char *side = end->leftright;
	switch (end->addrtype) {
	case KH_NOTSET:
		/* nothing! */
		break;

	case KH_DEFAULTROUTE:
		fprintf(out, "\t%s=%%defaultroute\n", side);
		break;

	case KH_ANY:
		fprintf(out, "\t%s=%%any\n", side);
		break;

	case KH_IFACE:
		if (end->strings_set[KSCF_IP])
			fprintf(out, "\t%s=%s\n", side, end->strings[KSCF_IP]);
		break;

	case KH_OPPO:
		fprintf(out, "\t%s=%%opportunistic\n", side);
		break;

	case KH_OPPOGROUP:
		fprintf(out, "\t%s=%%opportunisticgroup\n", side);
		break;

	case KH_GROUP:
		fprintf(out, "\t%s=%%group\n", side);
		break;

	case KH_IPHOSTNAME:
		fprintf(out, "\t%s=%s\n", side, end->strings[KSCF_IP]);
		break;

	case KH_IPADDR:
		{
			ipstr_buf as;

			fprintf(out, "\t%s=%s\n", side, ipstr(&end->addr, &as));
		}
		break;
	}

	if (end->strings_set[KSCF_ID] && end->id)
		fprintf(out, "\t%sid=\"%s\"\n",     side, end->id);

	switch (end->nexttype) {
	case KH_NOTSET:
		/* nothing! */
		break;

	case KH_DEFAULTROUTE:
		fprintf(out, "\t%snexthop=%%defaultroute\n", side);
		break;

	case KH_IPADDR:
		{
			ipstr_buf as;

			fprintf(out, "\t%snexthop=%s\n",
				side, ipstr(&end->nexthop, &as));
		}
		break;

	default:
		break;
	}

	if (end->subnet != NULL) {
		fprintf(out, "\t%ssubnet=%s\n", side,
			end->subnet);
	}

	if (cidr_is_specified(end->vti_ip)) {
		cidr_buf as;
		fprintf(out, "\t%svti=%s\n", side,
			str_cidr(&end->vti_ip, &as));
	}

	if (cidr_is_specified(end->ifaceip)) {
		cidr_buf as;
		fprintf(out, "\t%sinterface-ip=%s\n", side,
			str_cidr(&end->ifaceip, &as));
	}

	if (end->pubkey != NULL && end->pubkey[0] != '\0') {
		enum_buf pkb;
		fprintf(out, "\t%s%s=%s\n", side,
			str_enum(&ipseckey_algorithm_config_names, end->pubkey_alg, &pkb),
			end->pubkey);
	}

	if (end->protoport.is_set) {
		protoport_buf buf;
		fprintf(out, "\t%sprotoport=%s\n", side,
			str_protoport(&end->protoport, &buf));
	}

	if (end->certx != NULL)
		fprintf(out, "\t%scert=%s\n", side, end->certx);

	if (end->sourceip != NULL) {
		fprintf(out, "\t%ssourceip=%s\n",
			side, end->sourceip);
	}
	confwrite_int(out, side,
		      kv_conn | kv_leftright,
		      end->options, end->options_set, end->strings);
	confwrite_str(out, side, kv_conn | kv_leftright,
		      end->strings, end->strings_set);
}

static void confwrite_comments(FILE *out, struct starter_conn *conn)
{
	struct starter_comments *sc, *scnext;

	for (sc = conn->comments.tqh_first;
	     sc != NULL;
	     sc = scnext) {
		scnext = sc->link.tqe_next;

		fprintf(out, "\t%s=%s\n",
			sc->x_comment, sc->commentvalue);
	}
}

static void confwrite_conn(FILE *out, struct starter_conn *conn, bool verbose)
{
	/* short-cut for writing out a field (string-valued, indented, on its own line) */
#	define cwf(name, value)	{ fprintf(out, "\t" name "=%s\n", (value)); }

	if (verbose)
		fprintf(out, "# begin conn %s\n", conn->name);

	fprintf(out, "conn %s\n", conn->name);

	if (conn->alsos != NULL) {
		/* handle also= as a comment */

		int alsoplace = 0;

		fprintf(out, "\t#also =");
		while (conn->alsos[alsoplace] != NULL) {
			fprintf(out, " %s", conn->alsos[alsoplace]);
			alsoplace++;
		}
		fprintf(out, "\n");
	}
	confwrite_side(out, &conn->left);
	confwrite_side(out, &conn->right);
	/* fprintf(out, "# confwrite_int:\n"); */
	confwrite_int(out, "", kv_conn,
		      conn->options, conn->options_set, conn->strings);
	/* fprintf(out, "# confwrite_str:\n"); */
	confwrite_str(out, "", kv_conn,
		      conn->strings, conn->strings_set);
	/* fprintf(out, "# confwrite_comments:\n"); */
	confwrite_comments(out, conn);

	if (conn->connalias != NULL)
		cwf("connalias", conn->connalias);

	{
		const char *dsn = "UNKNOWN";

		switch (conn->autostart) {
		case AUTOSTART_IGNORE:
			dsn = "ignore";
			break;

		case AUTOSTART_ADD:
			dsn = "add";
			break;

		case AUTOSTART_ONDEMAND:
			dsn = "ondemand";
			break;

		case AUTOSTART_START:
			dsn = "start";
			break;

		case AUTOSTART_KEEP:
			dsn = "keep";
			break;
		}
		cwf("auto", dsn);
	}

	if (conn->policy != LEMPTY ||
	    conn->prospective_shunt != SHUNT_UNSET) {
		lset_t phase2_policy =
			(conn->policy &
			 (POLICY_AUTHENTICATE | POLICY_ENCRYPT));
		enum shunt_policy shunt_policy = conn->prospective_shunt;
		lset_t ppk_policy = (conn->policy & (POLICY_PPK_ALLOW | POLICY_PPK_INSIST));
		lset_t ike_frag_policy = (conn->policy & POLICY_IKE_FRAG_MASK);
		static const char *const noyes[2 /*bool*/] = {"no", "yes"};
		/* short-cuts for writing out a field that is a policy bit.
		 * cwpbf flips the sense of the bit.
		 */
#		define cwpb(name, p)  { cwf(name, noyes[(conn->policy & (p)) != LEMPTY]); }
#		define cwpbf(name, p)  { cwf(name, noyes[(conn->policy & (p)) == LEMPTY]); }

		switch (shunt_policy) {
		case SHUNT_UNSET:
		case SHUNT_TRAP:
			cwf("type", conn->policy & POLICY_TUNNEL? "tunnel" : "transport");

			cwpb("compress", POLICY_COMPRESS);

			cwpb("pfs", POLICY_PFS);
			cwpbf("ikepad", POLICY_NO_IKEPAD);


			if (conn->left.options[KNCF_AUTH] == k_unset ||
			    conn->right.options[KNCF_AUTH] == k_unset) {
				authby_buf ab;
				cwf("authby", str_authby(conn->authby, &ab));
			}

			{
				const char *p2ps = "UNKNOWN";

				switch (phase2_policy) {
				case POLICY_AUTHENTICATE:
					p2ps = "ah";
					break;

				case POLICY_ENCRYPT:
					p2ps = "esp";
					break;

				case POLICY_ENCRYPT | POLICY_AUTHENTICATE:
					p2ps = "ah+esp";
					break;

				default:
					break;
				}
				cwf("phase2", p2ps);
			}

			/* ikev2= */
			{
				const char *v2ps;
				switch (conn->ike_version) {
				case IKEv1:
					v2ps = "no";
					break;
				case IKEv2:
					v2ps = "yes";
					break;
				default:
					v2ps = "UNKNOWN";
					break;
				}
				cwf("ikev2", v2ps);
			}

			/* ppk= */
			{
				const char *p_ppk = "UNKNOWN";

				switch (ppk_policy) {
				case LEMPTY:
					p_ppk = "no";
					break;
				case POLICY_PPK_ALLOW:
					p_ppk = "permit";
					break;
				case POLICY_PPK_INSIST:
					p_ppk = "insist";
					break;
				}
				cwf("ppk", p_ppk);
			}

			/* esn= */
			{
				const char *esn = "UNKNOWN";

				if ((conn->policy & POLICY_ESN_NO)) {
					if ((conn->policy & POLICY_ESN_YES) == LEMPTY)
						esn = "no";
					else
						esn = "either";
				} else {
						/* both cannot be unset */
						esn = "yes";
					}

				cwf("esn", esn);
			}

			{
				const char *ifp = "UNKNOWN";

				switch (ike_frag_policy) {
				case LEMPTY:
					ifp = "never";
					break;

				case POLICY_IKE_FRAG_ALLOW:
					/* it's the default, do not print anything */
					ifp = NULL;
					break;

				case POLICY_IKE_FRAG_ALLOW | POLICY_IKE_FRAG_FORCE:
					ifp = "force";
					break;
				}
				if (ifp != NULL)
					cwf("ike_frag", ifp);
			}

			break; /* end of case POLICY_SHUNT_TRAP */

		case SHUNT_PASS:
			cwf("type", "passthrough");
			break;

		case SHUNT_DROP:
			cwf("type", "drop");
			break;

		case SHUNT_REJECT:
			cwf("type", "reject");
			break;

		case SHUNT_IPSEC:
			cwf("type", "ipsec"); /* can't happen */
			break;

		case SHUNT_NONE:
			cwf("type", "none"); /* can't happen */
			break;

		case SHUNT_HOLD:
			cwf("type", "hold"); /* can't happen */
			break;

		}

#		undef cwpb
#		undef cwpbf
	}

	if (verbose)
		fprintf(out, "# end conn %s\n\n", conn->name);
#	undef cwf
}

void confwrite(struct starter_config *cfg, FILE *out, bool setup, char *name, bool verbose)
{
	struct starter_conn *conn;

	/* output version number */
	/* fprintf(out, "\nversion 2.0\n\n"); */

	/* output config setup section */
	if (setup) {
		fprintf(out, "config setup\n");
		confwrite_int(out, "",
		      kv_config,
		      cfg->setup.options, cfg->setup.options_set,
		      cfg->setup.strings);
		confwrite_str(out, "",
		      kv_config,
		      cfg->setup.strings, cfg->setup.strings_set);

		fprintf(out, "\n");
	}

	/* output connections */
	for (conn = cfg->conns.tqh_first; conn != NULL;
	     conn = conn->link.tqe_next)
		if (name == NULL || streq(name, conn->name))
			confwrite_conn(out, conn, verbose);
	if (verbose)
		fprintf(out, "# end of config\n");
}
