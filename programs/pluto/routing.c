/* connection routing, for libreswan
 *
 * Copyright (C) 2023 Andrew Cagney
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

#include "enum_names.h"

#include "defs.h"
#include "routing.h"
#include "connections.h"
#include "pending.h"
#include "log.h"
#include "kernel.h"
#include "kernel_policy.h"
#include "revival.h"
#include "ikev2_ike_sa_init.h"		/* for initiate_v2_IKE_SA_INIT_request() */
#include "pluto_stats.h"
#include "foodgroups.h"			/* for connection_group_{route,unroute}() */
#include "orient.h"
#include "initiated_by.h"
#include "updown.h"
#include "instantiate.h"
#include "connection_event.h"

enum routing_event {
	/* fiddle with the ROUTE bit */
	CONNECTION_ROUTE,
	CONNECTION_UNROUTE,
	/* start/stop a connection */
	CONNECTION_INITIATE, /* also revive */
	/* initiator/responder? */
	CONNECTION_INITIATE_IKE,
	CONNECTION_INITIATE_CHILD,
	CONNECTION_RESPOND_IKE,
	CONNECTION_RESPOND_CHILD,
	CONNECTION_PENDING,
	CONNECTION_RESCHEDULE,
	/* establish a connection (speculative) */
	CONNECTION_ESTABLISH_IKE,
	CONNECTION_ESTABLISH_INBOUND,
	CONNECTION_ESTABLISH_OUTBOUND,
	/* tear down a connection */
	CONNECTION_TEARDOWN_IKE,
	CONNECTION_TEARDOWN_CHILD,
	/* mobike */
	CONNECTION_SUSPEND,
	CONNECTION_RESUME,
#define ROUTING_EVENT_ROOF (CONNECTION_RESUME+1)
};

static const char *routing_event_name[] = {
#define S(E) [E] = #E
	S(CONNECTION_ROUTE),
	S(CONNECTION_UNROUTE),
	S(CONNECTION_INITIATE),
	S(CONNECTION_ESTABLISH_IKE),
	S(CONNECTION_ESTABLISH_INBOUND),
	S(CONNECTION_ESTABLISH_OUTBOUND),
	S(CONNECTION_PENDING),
	S(CONNECTION_RESCHEDULE),
	S(CONNECTION_INITIATE_IKE),
	S(CONNECTION_INITIATE_CHILD),
	S(CONNECTION_RESPOND_IKE),
	S(CONNECTION_RESPOND_CHILD),
	S(CONNECTION_TEARDOWN_IKE),
	S(CONNECTION_TEARDOWN_CHILD),
	S(CONNECTION_SUSPEND),
	S(CONNECTION_RESUME),
#undef S
};

static enum_names routing_event_names = {
	0, ROUTING_EVENT_ROOF-1,
	ARRAY_REF(routing_event_name),
	"CONNECTION_",
	NULL,
};

struct routing_annex {
	struct ike_sa **ike;
	struct child_sa **child;
	enum initiated_by initiated_by;
	const char *story;
	bool (*dispatch_ok)(struct connection *c, struct logger *logger, const struct routing_annex *e);
	void (*post_op)(const struct routing_annex *e);
	where_t where;
};

static bool dispatch(const enum routing_event event,
		     struct connection *c,
		     struct logger *logger,
		     const struct routing_annex *e);

static bool dispatch_1(enum routing_event event,
		       struct connection *c,
		       struct logger *logger,
		       const struct routing_annex *e);

static bool connection_cannot_die(enum routing_event event,
				  struct connection *c,
				  struct logger *logger,
				  const struct routing_annex *e)
{
	struct state *st = (e->child != NULL && (*e->child) != NULL ? &(*e->child)->sa :
			    e->ike != NULL && (*e->ike) != NULL ? &(*e->ike)->sa :
			    NULL);
	const char *subplot = (event == CONNECTION_TEARDOWN_IKE ? e->story :
			       event == CONNECTION_TEARDOWN_CHILD ? e->story :
			       event == CONNECTION_RESCHEDULE ? e->story :
			       "???");
	return scheduled_revival(c, st, subplot, logger);
}

static void jam_sa(struct jambuf *buf, struct state *st, const char **sep)
{
	if (st != NULL) {
		jam_string(buf, (*sep)); (*sep) = " ";
		jam_string(buf, state_sa_short_name(st));
		jam_string(buf, " ");
		jam_so(buf, st->st_serialno);
		if (st == NULL) {
			jam_string(buf, " (deleted)");
		} else {
			jam_string(buf, " (");
			jam_string(buf, st->st_state->short_name);
			jam_string(buf, ")");
		}
	}
}

static void jam_so_update(struct jambuf *buf,
			  enum connection_owner owner,
			  so_serial_t old, so_serial_t new,
			  const char **prefix)
{
	if (old != SOS_NOBODY || new != SOS_NOBODY) {
		jam_string(buf, (*prefix)); (*prefix) = " ";
		jam_enum(buf, &connection_owner_names, owner);
		jam_string(buf, " ");
		jam_so(buf, old);
		if (old != new) {
			jam_string(buf, "->");
			jam_so(buf, new);
		}
	}
}

static void jam_routing(struct jambuf *buf,
			struct connection *c)
{
	jam_string(buf, " ");
	jam_connection_co(buf, c);
	jam(buf, "@%p", c);
	if (never_negotiate(c)) {
		jam_string(buf, "; never-negotiate");
	}
	const char *sep = "; ";
	for (enum connection_owner owner = 0; owner < elemsof(c->owner); owner++) {
		/* same value - no actual update */
		jam_so_update(buf, owner, c->owner[owner], c->owner[owner], &sep);
	}
}

static void jam_routing_annex(struct jambuf *buf, const struct routing_annex *e)
{
	const char *sep = " ";
	if (e->ike != NULL && (*e->ike) != NULL) {
		jam_sa(buf, &(*e->ike)->sa, &sep);
	}
	if (e->child != NULL && (*e->child) != NULL) {
		jam_sa(buf, &(*e->child)->sa, &sep);
	}
	jam_string(buf, sep); sep = " ";
	jam_string(buf, "by=");
	jam_enum_short(buf, &initiated_by_names, e->initiated_by);
	jam_string(buf, ";");
}

static void jam_event(struct jambuf *buf,
		      struct connection *c,
		      const struct routing_annex *e)
{
	jam_routing_annex(buf, e);
	jam_routing(buf, c);
}

static void jam_routing_prefix(struct jambuf *buf,
			       const char *prefix,
			       enum routing_event event,
			       enum routing old_routing,
			       enum routing new_routing,
			       enum connection_kind kind)
{
	jam_string(buf, "routing: ");
	jam_string(buf, prefix);
	jam_string(buf, " ");
	jam_enum_short(buf, &routing_event_names, event);
	jam_string(buf, ", ");
	jam_enum_short(buf, &routing_names, old_routing);
	if (old_routing != new_routing) {
		jam_string(buf, "->");
		jam_enum_short(buf, &routing_names, new_routing);
	}
	jam_string(buf, ", ");
	jam_enum_short(buf, &connection_kind_names, kind);
	jam_string(buf, ";");
}

struct old_routing {
	/* capture what can change */
	const char *ike_name;
	so_serial_t ike_so;
	const char *child_name;
	so_serial_t child_so;
	so_serial_t owner[CONNECTION_OWNER_ROOF];
	enum routing routing;
	unsigned revival_attempt;
};

static struct old_routing ldbg_routing_start(enum routing_event event,
					     struct connection *c,
					     struct logger *logger,
					     const struct routing_annex *e)
{
	struct old_routing old = {
		/* capture what can change */
		.ike_name = (e->ike == NULL || (*e->ike) == NULL ? SOS_NOBODY :
			     state_sa_short_name(&(*e->ike)->sa)),
		.ike_so = (e->ike == NULL || (*e->ike) == NULL ? SOS_NOBODY :
			   (*e->ike)->sa.st_serialno),
		.child_name = (e->child == NULL || (*e->child) == NULL ? SOS_NOBODY :
			       state_sa_short_name(&(*e->child)->sa)),
		.child_so = (e->child == NULL || (*e->child) == NULL ? SOS_NOBODY :
			     (*e->child)->sa.st_serialno),
		.routing = c->child.routing,
		.revival_attempt = c->revival.attempt,
	};
	for (unsigned i = 0; i < elemsof(old.owner); i++) {
		old.owner[i] = c->owner[i];
	}

	if (DBGP(DBG_ROUTING)) {
		/*
		 * XXX: force ADD_PREFIX so that the connection name
		 * is before the interesting stuff.
		 */
		LLOG_JAMBUF(DEBUG_STREAM|ADD_PREFIX, logger, buf) {
			jam_routing_prefix(buf, "start", event,
					   c->child.routing, c->child.routing,
					   c->local->kind);
			jam_event(buf, c, e);
			jam_string(buf, " ");
			jam_where(buf, e->where);
		}
	}
	return old;
}

static void ldbg_routing_stop(enum routing_event event,
			      struct connection *c,
			      struct logger *logger,
			      const struct routing_annex *e,
			      const struct old_routing *old,
			      bool ok)
{
	if (DBGP(DBG_ROUTING)) {
		/*
		 * XXX: force ADD_PREFIX so that the connection name
		 * is before the interesting stuff.
		 */
		LLOG_JAMBUF(DEBUG_STREAM|ADD_PREFIX, logger, buf) {
			jam_routing_prefix(buf, "stop", event,
					   old->routing, c->child.routing,
					   c->local->kind);
			jam(buf, " ok=%s", bool_str(ok));
			/* various SAs */
			const char *sep = "; ";
			for (enum connection_owner owner = 0;
			     owner < elemsof(c->owner); owner++) {
				jam_so_update(buf, owner,
					      old->owner[owner],
					      c->owner[owner], &sep);
			}
			if (old->revival_attempt != c->revival.attempt) {
				jam_string(buf, sep); sep = " ";
				jam(buf, "revival %u->%u",
				    old->revival_attempt,
				    c->revival.attempt);
			}
			jam_string(buf, " ");
			jam_where(buf, e->where);
		}
	}
}

PRINTF_LIKE(2)
void ldbg_routing(struct logger *logger, const char *fmt, ...)
{
	if (DBGP(DBG_ROUTING)) {
		LLOG_JAMBUF(DEBUG_STREAM|ADD_PREFIX, logger, buf) {
			jam_string(buf, "routing:   ");
			va_list ap;
			va_start(ap, fmt);
			jam_va_list(buf, fmt, ap);
			va_end(ap);
		}
	}
}

/*
 * The IKEv1 responder spreads establishing a Child SA over two
 * transactions (messages): INBOUND is established while processing
 * the first Quick Mode request; and OUTBOUND while processing the
 * second Quick Mode request.
 *
 * IKEv2 and IKEv1 initiators all establish the Child SA in one
 * transaction (message).  However, to make error recovery slightly
 * simpler, this single transaction is brown down just like for an
 * IKEv1 responder.
 */

bool connection_establish_inbound(struct child_sa *child, where_t where)
{
	struct connection *cc = child->sa.st_connection;
	struct logger *logger = child->sa.logger;
	struct routing_annex annex = {
		.child = &child,
		.where = where,
		.initiated_by = INITIATED_BY_PEER,
	};
	return dispatch(CONNECTION_ESTABLISH_INBOUND, cc, logger, &annex);
}

bool connection_establish_outbound(struct ike_sa *ike, struct child_sa *child, where_t where)
{
	struct connection *cc = child->sa.st_connection;
	struct logger *logger = child->sa.logger;
	struct routing_annex annex = {
		.child = &child,
		.ike = &ike,
		.where = where,
		.initiated_by = INITIATED_BY_PEER,
	};
	return dispatch(CONNECTION_ESTABLISH_OUTBOUND, cc, logger, &annex);
}

bool connection_establish_child(struct ike_sa *ike, struct child_sa *child, where_t where)
{
	struct connection *cc = child->sa.st_connection;
	struct logger *logger = child->sa.logger;
	struct routing_annex annex = {
		.child = &child,
		.ike = &ike,
		.where = where,
		.initiated_by = INITIATED_BY_PEER,
	};
	/*
	 * The IKEv1 initiator, and IKEv2 initiators and responders
	 * alway establish both inbound and outbound during the same
	 * exchange.
	 *
	 * However, since this can fail part way through, it is broken
	 * down into two transactions.  That way, hopefully, the
	 * outbound code doesn't need to revert changes made by the
	 * inbound code.
	 */
	return (dispatch(CONNECTION_ESTABLISH_INBOUND, cc, logger, &annex) &&
		dispatch(CONNECTION_ESTABLISH_OUTBOUND, cc, logger, &annex));
}

enum shunt_kind routing_shunt_kind(enum routing routing)
{
	switch (routing) {
	case RT_UNROUTED:
		return SHUNT_KIND_NONE;
	case RT_ROUTED_NEVER_NEGOTIATE:
		return SHUNT_KIND_NEVER_NEGOTIATE;
	case RT_ROUTED_ONDEMAND:
		return SHUNT_KIND_ONDEMAND;
	case RT_UNROUTED_BARE_NEGOTIATION:
	case RT_UNROUTED_NEGOTIATION:
	case RT_ROUTED_NEGOTIATION:
		return SHUNT_KIND_NEGOTIATION;
	case RT_UNROUTED_INBOUND:
	case RT_UNROUTED_INBOUND_NEGOTIATION:
	case RT_ROUTED_INBOUND_NEGOTIATION:
		return SHUNT_KIND_NEGOTIATION; /*SHUNT_KIND_IPSEC?*/
	case RT_UNROUTED_TUNNEL:
	case RT_ROUTED_TUNNEL:
		return SHUNT_KIND_IPSEC;
	case RT_UNROUTED_FAILURE:
	case RT_ROUTED_FAILURE:
		return SHUNT_KIND_FAILURE;
	}
	bad_case(routing);
}

enum shunt_kind spd_shunt_kind(const struct spd_route *spd)
{
	return routing_shunt_kind(spd->connection->child.routing);
}

bool routed(const struct connection *c)
{
	enum routing r = c->child.routing;
	switch (r) {
	case RT_ROUTED_ONDEMAND:
	case RT_ROUTED_NEVER_NEGOTIATE:
	case RT_ROUTED_NEGOTIATION:
	case RT_ROUTED_INBOUND_NEGOTIATION:
	case RT_ROUTED_TUNNEL:
	case RT_ROUTED_FAILURE:
		return true;
	case RT_UNROUTED:
	case RT_UNROUTED_BARE_NEGOTIATION:
	case RT_UNROUTED_NEGOTIATION:
	case RT_UNROUTED_FAILURE:
	case RT_UNROUTED_INBOUND:
	case RT_UNROUTED_INBOUND_NEGOTIATION:
	case RT_UNROUTED_TUNNEL:
		return false;
	}
	bad_case(r);
}

bool kernel_policy_installed(const struct connection *c)
{
	switch (c->child.routing) {
	case RT_UNROUTED:
	case RT_UNROUTED_NEGOTIATION:
	case RT_UNROUTED_BARE_NEGOTIATION:
		return false;
	case RT_ROUTED_ONDEMAND:
	case RT_ROUTED_NEGOTIATION:
	case RT_UNROUTED_INBOUND:
	case RT_UNROUTED_INBOUND_NEGOTIATION:
	case RT_ROUTED_NEVER_NEGOTIATE:
	case RT_ROUTED_INBOUND_NEGOTIATION:
	case RT_ROUTED_TUNNEL:
	case RT_UNROUTED_FAILURE:
	case RT_ROUTED_FAILURE:
	case RT_UNROUTED_TUNNEL:
		return true;
	}
	bad_case(c->child.routing);
}

static void set_routing(enum routing_event event UNUSED,
			struct connection *c,
			enum routing new_routing,
			const struct routing_annex *e)
{
	if (e != NULL && e->child != NULL && (*e->child) != NULL) {
		c->newest_routing_sa = (*e->child)->sa.st_serialno;
	} else {
		c->newest_routing_sa =
			c->newest_ipsec_sa =
			SOS_NOBODY;
	}
	c->child.routing = new_routing;
}

static void set_initiated(enum routing_event event UNUSED,
			  struct connection *c,
			  enum routing new_routing,
			  const struct routing_annex *e)
{
	/*
	 * The IKE and Child share the same initiate event but are
	 * dispatched separately.
	 *
	 * A negotiating child connection using an existing IKE SA
	 * doesn't have .established_ike_sa set.  Should it, and
	 * should it set .negotiating_ike_sa.
	 */
	if ((e->child) != NULL && (*e->child) != NULL) {
		PEXPECT((*e->child)->sa.logger, c->newest_routing_sa == SOS_NOBODY);
		c->newest_routing_sa = (*e->child)->sa.st_serialno;
		c->child.routing = new_routing;
		return;
	}

	if ((e->ike) != NULL && (*e->ike) != NULL) {
		PEXPECT((*e->ike)->sa.logger, c->negotiating_ike_sa == SOS_NOBODY);
		c->negotiating_ike_sa = (*e->ike)->sa.st_serialno;
		c->child.routing = new_routing;
		return;
	}

	/*
	 * For instance when the initiated connection is on the
	 * pending queue.  Should such a connection get its owner
	 * updated?  It definitely needs its routing updated so that
	 * pending knows what to change when things progress.
	 */
	ldbg_routing(c->logger, "no initiating IKE or Child SA; assumed to be pending; leaving routing alone");
}

static void set_established_child(enum routing_event event UNUSED,
				  struct connection *c,
				  enum routing routing,
				  const struct routing_annex *e)
{
	struct child_sa *child = (*e->child);
	struct ike_sa *ike = (e->ike != NULL ? (*e->ike) : NULL);
	PEXPECT(child->sa.logger, child->sa.st_connection == c);
	if (ike != NULL) {
		/* by definition */
		PEXPECT(child->sa.logger, ike->sa.st_serialno == child->sa.st_clonedfrom);
		/*
		 * Do we have star-crossed-streams?  When this happens
		 * try to mitigate the damage.
		 */
		for (enum connection_owner owner = IKE_SA_OWNER_FLOOR;
		     owner < IKE_SA_OWNER_ROOF; owner++) {
			if (ike->sa.st_connection == c) {
				if (ike->sa.st_serialno != c->owner[owner]) {
					/* child/ike have crossed streams */
					enum_buf ob;
					llog(RC_LOG_SERIOUS, child->sa.logger,
					     "Child SA with IKE SA "PRI_SO" share their connection, .%s "PRI_SO" should be the IKE SA, updating "PRI_WHERE,
					     pri_so(ike->sa.st_serialno),
					     str_enum(&connection_owner_names, owner, &ob),
					     pri_so(c->owner[owner]),
					     pri_where(e->where));
					c->owner[owner] = ike->sa.st_serialno;
				}
			} else {
				if (c->owner[owner] != SOS_NOBODY) {
					/* child is a cuckoo */
					enum_buf ob;
					llog_pexpect(child->sa.logger, HERE,
						     "Child SA with IKE SA "PRI_SO" do not share their connection, .%s "PRI_SO" should be unset, clearing "PRI_WHERE,
						     pri_so(ike->sa.st_serialno),
						     str_enum(&connection_owner_names, owner, &ob),
						     pri_so(c->owner[owner]),
						     pri_where(e->where));
					c->owner[owner] = SOS_NOBODY;
				}
			}
		}
	}
	c->child.routing = routing;
	c->newest_ipsec_sa = c->newest_routing_sa = (*e->child)->sa.st_serialno;
}

static bool unrouted_to_routed_ondemand(enum routing_event event, struct connection *c, where_t where)
{
	if (!unrouted_to_routed(c, RT_ROUTED_ONDEMAND, where)) {
		return false;
	}
	set_routing(event, c, RT_ROUTED_ONDEMAND, NULL);
	return true;
}

static bool unrouted_to_routed_never_negotiate(enum routing_event event, struct connection *c, where_t where)
{
	if (!unrouted_to_routed(c, RT_ROUTED_NEVER_NEGOTIATE, where)) {
		return false;
	}
	set_routing(event, c, RT_ROUTED_NEVER_NEGOTIATE, NULL);
	return true;
}

static void routed_tunnel_to_routed_ondemand(enum routing_event event,
					     struct child_sa *child,
					     where_t where)
{
	/* currently up and routed */

	struct logger *logger = child->sa.logger;
	struct connection *c = child->sa.st_connection;

	FOR_EACH_ITEM(spd, &c->child.spds) {

		if (is_v1_cisco_split(spd, HERE)) {
			continue;
		}

		struct spd_owner owner = spd_owner(spd, RT_ROUTED_ONDEMAND,
						   logger, where);

		delete_cat_kernel_policies(spd, &owner, logger, where);
		replace_ipsec_with_bare_kernel_policy(child, c, spd, &owner,
						      SHUNT_KIND_ONDEMAND,
						      EXPECT_KERNEL_POLICY_OK,
						      logger, where);
	}

	set_routing(event, child->sa.st_connection, RT_ROUTED_ONDEMAND, NULL);
}

static void routed_tunnel_to_routed_failure(enum routing_event event,
					    struct child_sa *child,
					    where_t where)
{
	/* currently up and routed */

	struct logger *logger = child->sa.logger;
	struct connection *c = child->sa.st_connection;

	FOR_EACH_ITEM(spd, &c->child.spds) {

		if (is_v1_cisco_split(spd, HERE)) {
			continue;
		}

		do_updown(UPDOWN_DOWN, c, spd, child, logger);

		struct spd_owner owner = spd_owner(spd, RT_ROUTED_FAILURE,
						   logger, where);

		delete_cat_kernel_policies(spd, &owner, logger, where);
		replace_ipsec_with_bare_kernel_policy(child, c, spd, &owner,
						      SHUNT_KIND_FAILURE,
						      EXPECT_KERNEL_POLICY_OK,
						      logger, where);
	}

	set_routing(event, child->sa.st_connection, RT_ROUTED_FAILURE, NULL);
}

static void routed_kernel_policy_to_unrouted(enum routing_event event,
					     struct connection *c,
					     lset_t direction,
					     struct logger *logger,
					     where_t where,
					     const char *story)
{
	enum expect_kernel_policy inbound_policy_expectation;
	switch (direction) {
	case DIRECTION_INBOUND|DIRECTION_OUTBOUND:
	case DIRECTION_INBOUND:
		inbound_policy_expectation = EXPECT_KERNEL_POLICY_OK;
		break;
	case DIRECTION_OUTBOUND:
		inbound_policy_expectation = EXPECT_NO_INBOUND;
		break;
	default:
		bad_case(direction);
	}

	FOR_EACH_ITEM(spd, &c->child.spds) {

		if (is_v1_cisco_split(spd, HERE)) {
			continue;
		}

		struct spd_owner owner = spd_owner(spd, RT_UNROUTED,
						   logger, where);

		delete_spd_kernel_policies(spd, &owner,
					   inbound_policy_expectation,
					   logger, where, story);
		do_updown_unroute_spd(spd, &owner, NULL, logger);
	}

	set_routing(event, c, RT_UNROUTED, NULL);
}

static void unrouted_kernel_policy_to_unrouted(enum routing_event event,
					       struct connection *c,
					       lset_t direction,
					       struct logger *logger, where_t where,
					       const char *story)
{
	enum expect_kernel_policy inbound_policy_expectation;
	switch (direction) {
	case DIRECTION_INBOUND|DIRECTION_OUTBOUND:
	case DIRECTION_INBOUND:
		inbound_policy_expectation = EXPECT_KERNEL_POLICY_OK;
		break;
	case DIRECTION_OUTBOUND:
		inbound_policy_expectation = EXPECT_NO_INBOUND;
		break;
	default:
		bad_case(direction);
	}

	FOR_EACH_ITEM(spd, &c->child.spds) {

		if (is_v1_cisco_split(spd, HERE)) {
			continue;
		}

		struct spd_owner owner = spd_owner(spd, RT_UNROUTED,
						   logger, where);

		delete_spd_kernel_policies(spd, &owner,
					   inbound_policy_expectation,
					   logger, where, story);
	}

	set_routing(event, c, RT_UNROUTED, NULL);
}

static void routed_tunnel_to_unrouted(enum routing_event event,
				      struct child_sa *child,
				      where_t where)
{
	/* currently up and routed */

	struct logger *logger = child->sa.logger;
	struct connection *c = child->sa.st_connection;

	FOR_EACH_ITEM(spd, &c->child.spds) {

		if (is_v1_cisco_split(spd, HERE)) {
			continue;
		}

		do_updown(UPDOWN_DOWN, c, spd, child, logger);

		struct spd_owner owner = spd_owner(spd, RT_UNROUTED,
						   logger, where);

		delete_spd_kernel_policies(spd, &owner, EXPECT_KERNEL_POLICY_OK,
					   logger, where, "delete");
		do_updown_unroute_spd(spd, &owner, child, logger);
	}

	set_routing(event, c, RT_UNROUTED, NULL);
}

/*
 * For instance:
 *
 * = an instance with a routed ondemand template
 *
 * = an instance with an unrouted template due to whack?
 *
 * If this is an instance, then presumably the instance instantiate
 * code has figured out how wide the SPDs need to be.
 *
 * OTOH, if this is an unrouted permenant triggered by whack, just
 * replace.
 */

static void unrouted_instance_to_unrouted_negotiation(enum routing_event event UNUSED,
						      struct connection *c, where_t where)
{
	struct logger *logger = c->logger;
#if 0
	/* fails when whack forces the initiate so that the template
	 * is instantiated before it is routed */
	struct connection *t = c->clonedfrom; /* could be NULL */
	PEXPECT(logger, t != NULL && t->child.routing == RT_ROUTED_ONDEMAND);
#endif
	bool oe = is_opportunistic(c);
	const char *reason = (oe ? "replace unrouted opportunistic %trap with broad %pass or %hold" :
			      "replace unrouted %trap with broad %pass or %hold");
	add_spd_kernel_policies(c, KERNEL_POLICY_OP_REPLACE,
				DIRECTION_OUTBOUND,
				SHUNT_KIND_NEGOTIATION,
				logger, where, reason);
}

/*
 * Either C is permanent, or C is an instance that going to be revived
 * - the full set of SPDs need to be changed to negotiation (just
 * instantiated instances do not take this code path).
 */

static void routed_ondemand_to_routed_negotiation(enum routing_event event,
						  struct connection *c,
						  struct logger *logger,
						  const struct routing_annex *e)
{
        PEXPECT(logger, !is_opportunistic(c));
	PASSERT(logger, event == CONNECTION_INITIATE);
	enum routing rt_negotiation = RT_ROUTED_NEGOTIATION;
	FOR_EACH_ITEM(spd, &c->child.spds) {
		struct spd_owner owner = spd_owner(spd, rt_negotiation,
						   logger, HERE);
		if (!replace_spd_kernel_policy(spd, &owner,
					       DIRECTION_OUTBOUND,
					       SHUNT_KIND_NEGOTIATION,
					       logger, e->where,
					       "ondemand->negotiation")) {
			llog(RC_LOG, c->logger,
			     "converting ondemand kernel policy to negotiation");
		}
	}
	/* the state isn't yet known */
	set_routing(event, c, rt_negotiation, e);
}

/*
 * Either C is permanent, or C is an instance that going to be revived
 * - the full set of SPDs need to be changed to ondemand (just
 * instantiated instances do not take this code path).
 */

static void routed_negotiation_to_routed_ondemand(enum routing_event event,
						  struct connection *c,
						  struct logger *logger,
						  where_t where,
						  const char *reason)
{
	FOR_EACH_ITEM(spd, &c->child.spds) {
		struct spd_owner owner = spd_owner(spd, RT_ROUTED_ONDEMAND,
						   logger, HERE);
		if (!replace_spd_kernel_policy(spd, &owner,
					       DIRECTION_OUTBOUND,
					       SHUNT_KIND_ONDEMAND,
					       logger, where, reason)) {
			llog(RC_LOG, logger, "%s failed", reason);
		}
	}
	set_routing(event, c, RT_ROUTED_ONDEMAND, NULL);
}

/*
 * Delete the ROUTED_TUNNEL, and possibly delete the connection.
 */

static void teardown_routed_tunnel(enum routing_event event,
				   struct connection *c,
				   struct child_sa **child,
				   where_t where)
{
	if (scheduled_child_revival(*child, "received Delete/Notify")) {
		routed_tunnel_to_routed_ondemand(event, (*child), where);
		return;
	}

	/*
	 * Should this go back to on-demand?
	 */
	if (is_permanent(c) && c->policy.route) {
		/* it's being stripped of the state, hence SOS_NOBODY */
		routed_tunnel_to_routed_ondemand(event, (*child), where);
		return;
	}

	/*
	 * Is there a failure shunt?
	 */
	if (is_permanent(c) && c->config->failure_shunt != SHUNT_NONE) {
		routed_tunnel_to_routed_failure(event, (*child), where);
		return;
	}

	routed_tunnel_to_unrouted(event, (*child), where);
}

static void unrouted_tunnel_to_routed_ondemand(enum routing_event event,
					       struct child_sa *child,
					       where_t where)
{
	/* currently down and unrouted */

	struct logger *logger = child->sa.logger;
	struct connection *c = child->sa.st_connection;

	FOR_EACH_ITEM(spd, &c->child.spds) {

		if (is_v1_cisco_split(spd, HERE)) {
			continue;
		}

		do_updown(UPDOWN_DOWN, c, spd, child, logger);

		struct spd_owner owner = spd_owner(spd, RT_ROUTED_ONDEMAND,
						   logger, where);

		delete_cat_kernel_policies(spd, &owner, logger, where);
		replace_ipsec_with_bare_kernel_policy(child, c, spd, &owner,
						      SHUNT_KIND_ONDEMAND,
						      EXPECT_KERNEL_POLICY_OK,
						      logger, where);
	}

	do_updown_child(UPDOWN_ROUTE, child);
	set_routing(event, child->sa.st_connection, RT_ROUTED_ONDEMAND, NULL);
}

static void unrouted_tunnel_to_routed_failure(enum routing_event event,
					      struct child_sa *child,
					      where_t where)
{
	/* currently down and unrouted */

	struct logger *logger = child->sa.logger;
	struct connection *c = child->sa.st_connection;

	FOR_EACH_ITEM(spd, &c->child.spds) {

		if (is_v1_cisco_split(spd, HERE)) {
			continue;
		}

		do_updown(UPDOWN_DOWN, c, spd, child, logger);

		struct spd_owner owner = spd_owner(spd, RT_ROUTED_FAILURE,
						   logger, where);

		delete_cat_kernel_policies(spd, &owner, logger, where);
		replace_ipsec_with_bare_kernel_policy(child, c, spd, &owner,
						      SHUNT_KIND_FAILURE,
						      EXPECT_KERNEL_POLICY_OK,
						      logger, where);
	}

	do_updown_child(UPDOWN_ROUTE, child);
	set_routing(event, child->sa.st_connection, RT_ROUTED_FAILURE, NULL);
}

static void unrouted_tunnel_to_unrouted(enum routing_event event,
					struct connection *c,
					struct logger *logger,
					where_t where,
					const char *story)
{
	/* currently down and unrouted */

	FOR_EACH_ITEM(spd, &c->child.spds) {

		if (is_v1_cisco_split(spd, HERE)) {
			continue;
		}

		struct spd_owner owner = spd_owner(spd, RT_UNROUTED,
						   logger, where);

		delete_spd_kernel_policies(spd, &owner, EXPECT_KERNEL_POLICY_OK,
					   logger, where, story);
	}

	/*
	 * update routing; route_owner() will see this and not
	 * think this route is the owner?
	 */
	set_routing(event, c, RT_UNROUTED, NULL);
}

static void teardown_unrouted_tunnel(enum routing_event event,
				     struct connection *c,
				     struct child_sa *child,
				     struct logger *logger,
				     where_t where,
				     const char *story)
{
	if (scheduled_child_revival(child, "received Delete/Notify")) {
		unrouted_tunnel_to_routed_ondemand(event, child, where);
		return;
	}

	/*
	 * Should this go back to on-demand?
	 */
	if (is_permanent(c) && c->policy.route) {
		/* it's being stripped of the state, hence SOS_NOBODY */
		unrouted_tunnel_to_routed_ondemand(event, child, where);
		return;
	}

	/*
	 * Is there a failure shunt?
	 */
	if (is_permanent(c) && c->config->failure_shunt != SHUNT_NONE) {
		unrouted_tunnel_to_routed_failure(event, child, where);
		return;
	}

	unrouted_tunnel_to_unrouted(event, c, logger, where, story);
}

static void routed_inbound_negotiation_to_unrouted(enum routing_event event,
						   struct connection *c,
						   struct child_sa *child,
						   struct logger *logger,
						   where_t where,
						   const char *story)
{
	ldbg_routing(logger, "OOPS: ROUTED_INBOUND has no outbound policy");

	FOR_EACH_ITEM(spd, &c->child.spds) {
		if (is_v1_cisco_split(spd, HERE)) {
			continue;
		}

		struct spd_owner owner = spd_owner(spd, RT_UNROUTED/*ignored*/,
						   logger, where);

		delete_spd_kernel_policies(spd, &owner, EXPECT_KERNEL_POLICY_OK,
					   logger, where, story);
		do_updown_unroute_spd(spd, &owner, child, logger);
	}

	set_routing(event, c, RT_UNROUTED, NULL);
}

static void unrouted_inbound_to_unrouted(enum routing_event event,
					 struct connection *c,
					 struct logger *logger,
					 where_t where,
					 const char *story)
{
	ldbg_routing(logger, "OOPS: UNROUTED_INBOUND doesn't have outbound!");

	FOR_EACH_ITEM(spd, &c->child.spds) {

		if (is_v1_cisco_split(spd, HERE)) {
			continue;
		}

		struct spd_owner owner = spd_owner(spd, RT_UNROUTED,
						   logger, where);

		delete_spd_kernel_policies(spd, &owner,
					   EXPECT_KERNEL_POLICY_OK,
					   logger, where, story);
	}

	set_routing(event, c, RT_UNROUTED, NULL);
}

static void teardown_unrouted_inbound(enum routing_event event,
				      struct connection *c,
				      struct child_sa *child,
				      struct logger *logger,
				      where_t where,
				      const char *story)
{
	if (scheduled_child_revival(child, story)) {
		unrouted_inbound_to_unrouted(event, c, logger, where, story);
		return;
	}

	unrouted_inbound_to_unrouted(event, c, logger, where, story);
}

static void teardown_unrouted_inbound_negotiation(enum routing_event event,
						  struct connection *c,
						  struct child_sa *child,
						  struct logger *logger,
						  where_t where,
						  const char *story)
{
	if (scheduled_child_revival(child, story)) {
		unrouted_kernel_policy_to_unrouted(event, c, DIRECTION_INBOUND,
						   logger, where, story);
		return;
	}

	unrouted_kernel_policy_to_unrouted(event, c, DIRECTION_INBOUND,
					   logger, where, story);
}

static void teardown_routed_negotiation(enum routing_event event,
					struct connection *c,
					struct child_sa *child,
					struct logger *logger,
					where_t where,
					const char *reason)
{
	if (scheduled_child_revival(child, reason)) {
		routed_negotiation_to_routed_ondemand(event, c, logger, where,
						      reason);
		PEXPECT(logger, c->child.routing == RT_ROUTED_ONDEMAND);
		return;
	}

	if (is_instance(c) && is_opportunistic(c)) {
		/*
		 * A failed OE initiator, make shunt bare.
		 */
		orphan_holdpass(c, c->spd, logger);
		/*
		 * Change routing so we don't get cleared out
		 * when state/connection dies.
		 */
		set_routing(event, c, RT_UNROUTED, NULL);
		return;
	}

	if (c->policy.route) {
		routed_negotiation_to_routed_ondemand(event, c, logger, where,
						      "restoring ondemand, connection is routed");
		PEXPECT(logger, c->child.routing == RT_ROUTED_ONDEMAND);
		return;
	}

	/*
	 * Should this instead install a failure shunt?
	 */
	routed_kernel_policy_to_unrouted(event, c, DIRECTION_INBOUND,
					 logger, where, "deleting");
	PEXPECT(logger, c->child.routing == RT_UNROUTED);
}

/*
 * Received a message telling us to delete the connection's Child.SA.
 */

static bool child_dispatch_ok(struct connection *c, struct logger *logger, const struct routing_annex *e)
{
	if ((*e->child)->sa.st_serialno == c->newest_routing_sa) {
		return true;
	}
	ldbg_routing(logger, "Child SA does not match .newest_routing_sa "PRI_SO,
		     pri_so(c->newest_routing_sa));
	return false;
}

static void child_delete_post_op(const struct routing_annex *e)
{
	delete_child_sa(e->child);
}

static void zap_child(struct child_sa **child, const char *story, where_t where)
{
	struct connection *cc = (*child)->sa.st_connection;
	struct logger *logger = clone_logger((*child)->sa.logger, HERE); /* must free */

	struct routing_annex annex = {
		.child = child,
		.where = where,
		/*
		 * Does the child sa own the routing?
		 */
		.dispatch_ok = child_dispatch_ok,
		.post_op = child_delete_post_op,
		.story = story,
	};

	/*
	 * Let state machine figure out how to react.
	 */
	dispatch(CONNECTION_TEARDOWN_CHILD, cc, logger, &annex);

	PEXPECT(logger, (*child) == NULL);
	free_logger(&logger, HERE);
}

void connection_delete_child(struct child_sa **child, where_t where)
{
	zap_child(child, "delete Child SA", where);
}

void connection_timeout_child(struct child_sa **child, where_t where)
{
	zap_child(child, "timeout Child SA", where);
}

/*
 * If there's an established IKE SA and it isn't this one (i.e., not
 * owner) skip the route change.
 *
 * This isn't strong enough.  There could be multiple larval IKE SAs
 * and this check doesn't filter them out.
 */
static bool ike_dispatch_ok(struct connection *c, struct logger *logger, const struct routing_annex *e)
{
	if (c->established_ike_sa == SOS_NOBODY ||
	    c->established_ike_sa == (*e->ike)->sa.st_serialno) {
		return true;
	}

	ldbg_routing(logger, "IKE SA does not match .newest_routing_sa "PRI_SO,
		     pri_so(c->newest_routing_sa));
	return false;
}

static void ike_delete_post_op(const struct routing_annex *e)
{
	delete_ike_sa(e->ike);
}

static void zap_ike(struct ike_sa **ike, const char *story, where_t where)
{
	struct connection *c = (*ike)->sa.st_connection;
	struct logger *logger = clone_logger((*ike)->sa.logger, HERE);
	struct routing_annex annex = {
		.story = story,
		.ike = ike,
		.where = where,
		.dispatch_ok = ike_dispatch_ok,
		.post_op = ike_delete_post_op,
	};

	dispatch(CONNECTION_TEARDOWN_IKE, c, logger, &annex);

	PEXPECT(logger, (*ike) == NULL); /* no logger */
	free_logger(&logger, HERE);
}

void connection_delete_ike(struct ike_sa **ike, where_t where)
{
	zap_ike(ike, "delete IKE SA", where);
}

void connection_timeout_ike(struct ike_sa **ike, where_t where)
{
	zap_ike(ike, "timeout IKE SA", where);
}

/*
 * Stop reviving children trying to use this IKE SA.
 */

void connection_routing_init(struct connection *c)
{
	c->child.routing = RT_UNROUTED;
	for (unsigned i = 0; i < elemsof(c->owner); i++) {
		c->owner[i] = SOS_NOBODY;
	}
}

void state_disowns_connection(struct state *st)
{
	struct connection *c = st->st_connection;
	for (unsigned i = 0; i < elemsof(c->owner); i++) {
		if (c->owner[i] == st->st_serialno) {
#if 0
			/* should already be clear? */
			llog_pexpect(st->logger, HERE,
				     connection_owner_names[i]);
#else
			pdbg(st->logger,
			     "disown .%s",
			     enum_name(&connection_owner_names, i));
#endif
			c->owner[i] = SOS_NOBODY;
		}
	}
}


bool pexpect_connection_is_unrouted(struct connection *c, struct logger *logger, where_t where)
{
	bool ok_to_delete = true;
	if (c->child.routing != RT_UNROUTED) {
		enum_buf rn;
		llog_pexpect(logger, where,
			     "connection "PRI_CO" [%p] still in %s",
			     pri_connection_co(c), c,
			     str_enum_short(&routing_names, c->child.routing, &rn));
		ok_to_delete = false;
	}
	return ok_to_delete;
}

/*
 * Must be unrouted (i.e., all policies have been pulled).
 */
bool pexpect_connection_is_disowned(struct connection *c, struct logger *logger, where_t where)
{
	bool ok_to_delete = true;
	for (unsigned i = 0; i < elemsof(c->owner); i++) {
		if (c->owner[i] != SOS_NOBODY) {
			llog_pexpect(logger, where,
				     "connection "PRI_CO" [%p] is owned by .%s "PRI_SO,
				     pri_connection_co(c), c,
				     enum_name(&connection_owner_names, i),
				     pri_so(c->owner[i]));
			ok_to_delete = false;
		}
	}
	return ok_to_delete;
}

static bool initiated_ike_dispatch_ok(struct connection *c,
				      struct logger *logger,
				      const struct routing_annex *e)
{
	switch (c->child.routing) {
	case RT_UNROUTED:
		pexpect_connection_is_disowned(c, logger, e->where);
		return true;
	case RT_ROUTED_ONDEMAND:
		return true;
	default:
		/*
		 * Ignore stray initiates (presumably due to two
		 * acquires triggering simultaneously) or due to an
		 * initiate being used to force a rekey.
		 */
		{
			enum_buf rb;

			llog(RC_LOG, logger, "connection is already %s",
			     str_enum(&routing_tails, c->child.routing, &rb));
			return false;
		}
	}
}

void connection_initiated_ike(struct ike_sa *ike,
			      enum initiated_by initiated_by,
			      where_t where)
{
	struct connection *c = ike->sa.st_connection;
	struct logger *logger = ike->sa.logger;
	struct routing_annex annex = {
		.ike = &ike,
		.initiated_by = initiated_by,
		.where = where,
		.dispatch_ok = initiated_ike_dispatch_ok,
	};
	dispatch(CONNECTION_INITIATE, c, logger, &annex);
}

static bool initiated_child_dispatch_ok(struct connection *c,
					struct logger *logger,
					const struct routing_annex *e)
{
	switch (c->child.routing) {
	case RT_ROUTED_ONDEMAND:
		return true;
	case RT_UNROUTED:
		/*
		 * An UNROUTED connection (i.e., no Child SA) can
		 * still have an IKE SA, just as long as that IKE SA
		 * matches what is negotiating the connection?
		 *
		 * For instance:
		 *
		 *    up cuckold     -- #1, #2
		 *    up cuckoo      -- #3 (uses #1)
		 *    down cuckold   -- only deletes #2, #1 is in use
		 *
		 * followed by:
		 *
		 *    up cuckold
		 *
		 * will initiate the connection cuckold with IKE SA
		 * still set to #1.
		 *
		 * Have to wonder what happens when there's a replace?
		 */
		for (enum connection_owner owner = CONNECTION_OWNER_FLOOR;
		     owner < CONNECTION_OWNER_ROOF; owner++) {
			if (c->owner[owner] == SOS_NOBODY) {
				continue;
			}
			if (owner >= IKE_SA_OWNER_FLOOR &&
			    owner < IKE_SA_OWNER_ROOF &&
			    c->owner[owner] == (*e->ike)->sa.st_serialno) {
				pdbg(logger, "connection already has IKE SA");
				continue;
			}
			llog_pexpect(logger, e->where,
				     "connection "PRI_CO" [%p] is already owned by .%s "PRI_SO,
				     pri_connection_co(c), c,
				     enum_name(&connection_owner_names, owner),
				     pri_so(c->owner[owner]));
		}
		return true;
	default:
	{
		/*
		 * Ignore stray initiates (presumably due to two
		 * acquires triggering simultaneously) or due to an
		 * initiate being used to force a rekey.
		 */
		enum_buf rb;
		llog(RC_LOG, logger, "connection is already %s",
		     str_enum(&routing_tails, c->child.routing, &rb));
		return false;
	}
	}
}

void connection_initiated_child(struct ike_sa *ike, struct child_sa *child,
				enum initiated_by initiated_by,
				where_t where)
{
	struct connection *cc = child->sa.st_connection;
	struct logger *logger = child->sa.logger;
	struct routing_annex annex = {
		.ike = &ike,
		.child = &child,
		.initiated_by = initiated_by,
		.dispatch_ok = initiated_child_dispatch_ok,
		.where = where,
	};
	dispatch(CONNECTION_INITIATE, cc, logger, &annex);
}

void connection_pending(struct connection *c, enum initiated_by initiated_by, where_t where)
{
	struct routing_annex annex = {
		.initiated_by = initiated_by,
		.where = where,
		.dispatch_ok = initiated_child_dispatch_ok,
	};
	dispatch(CONNECTION_INITIATE, c, c->logger, &annex);
}

static bool unpend_dispatch_ok(struct connection *c,
			       struct logger *logger UNUSED,
			       const struct routing_annex *e UNUSED)
{
	/* skip when any hint of an owner */
	for (unsigned i = 0; i < elemsof(c->owner); i++) {
		if (c->owner[i] != SOS_NOBODY) {
			return false;
		}
	}
	return true;
}

void connection_reschedule(struct connection *c, struct logger *logger, where_t where)
{
	struct routing_annex annex = {
		.story = "re-schedule",
		.where = where,
		.dispatch_ok = unpend_dispatch_ok,
	};

	dispatch(CONNECTION_RESCHEDULE, c, logger, &annex);
}

static void set_established_ike(enum routing_event event UNUSED,
				struct connection *c,
				enum routing routing,
				const struct routing_annex *e)
{
	/* steal both the established and negotiating IKE SAs */
	struct ike_sa *ike = (*e->ike);
	c->negotiating_ike_sa = c->established_ike_sa = ike->sa.st_serialno;
	c->child.routing = routing; /* XXX: but this is IKE!?! */
	ike->sa.st_viable_parent = true;
	linux_audit_conn(&ike->sa, LAK_PARENT_START);
	/* dump new keys */
	if (DBGP(DBG_PRIVATE)) {
		DBG_tcpdump_ike_sa_keys(&ike->sa);
	}
}

void connection_establish_ike(struct ike_sa *ike, where_t where)
{
	struct connection *c = ike->sa.st_connection;
	struct logger *logger = ike->sa.logger;
	struct routing_annex annex = {
		.ike = &ike,
		.where = where,
		.initiated_by = INITIATED_BY_PEER,
	};
	dispatch(CONNECTION_ESTABLISH_IKE, c, logger, &annex);
}

void connection_route(struct connection *c, where_t where)
{
	if (!oriented(c)) {
		llog(RC_ORIENT, c->logger,
		     "we cannot identify ourselves with either end of this connection");
		return;
	}

	if (is_template(c)) {
		if (is_opportunistic(c)) {
			ldbg(c->logger, "template-route-possible: opportunistic");
		} else if (is_group_instance(c)) {
			ldbg(c->logger, "template-route-possible: groupinstance");
		} else if (is_labeled(c)) {
			ldbg(c->logger, "template-route-possible: has sec-label");
		} else if (c->local->config->child.virt != NULL) {
			ldbg(c->logger, "template-route-possible: local is virtual");
		} else if (c->remote->child.has_client) {
			/* see extract_child_end() */
			ldbg(c->logger, "template-route-possible: remote %s.child.has_client==true",
			     c->remote->config->leftright);
		} else {
			policy_buf pb;
			llog(RC_ROUTE, c->logger,
			     "cannot route template policy of %s",
			     str_connection_policies(c, &pb));
			return;
		}
	}

	struct routing_annex annex =  {
		.where = where,
	};
	dispatch(CONNECTION_ROUTE, c, c->logger, &annex);

}

void connection_unroute(struct connection *c, where_t where)
{
	/*
	 * XXX: strip POLICY.ROUTE in whack code, not here (code
	 * expects to be able to route/unroute without loosing the
	 * policy bits).
	 */
	struct routing_annex annex =  {
		.where = where,
	};
	dispatch(CONNECTION_UNROUTE, c, c->logger, &annex);
}

/*
 * "down" / "unroute" the connection but _don't_ delete the kernel
 * state / policy.
 *
 * Presumably the kernel policy (at least) is acting like a trap while
 * mibike migrates things?
 */

void connection_suspend(struct child_sa *child, where_t where)
{
	struct connection *cc = child->sa.st_connection;
	struct logger *logger = child->sa.logger;
	struct routing_annex annex = {
		.child = &child,
		.where = where,
	};
	dispatch(CONNECTION_SUSPEND, cc, logger, &annex);
}

void connection_resume(struct child_sa *child, where_t where)
{
	struct connection *cc = child->sa.st_connection;
	struct logger *logger = child->sa.logger;
	struct routing_annex annex = {
		.child = &child,
		.where = where,
	};
	dispatch(CONNECTION_RESUME, cc, logger, &annex);
}

static bool dispatch_1(enum routing_event event,
		       struct connection *c,
		       struct logger *logger,
		       const struct routing_annex *e)
{
#define XX(CONNECTION_EVENT, CONNECTION_ROUTING, CONNECTION_KIND)	\
	(((CONNECTION_EVENT) *						\
	  CONNECTION_ROUTING_ROOF + CONNECTION_ROUTING) *		\
	 CONNECTION_KIND_ROOF + CONNECTION_KIND)
#define X(EVENT, ROUTING, KIND)				\
	XX(CONNECTION_##EVENT, RT_##ROUTING, CK_##KIND)

	const enum routing routing = c->child.routing;
	const enum connection_kind kind = c->local->kind;

	switch (XX(event, routing, kind)) {

	case X(ROUTE, UNROUTED, GROUP):
		/* caller deals with recursion */
		add_policy(c, policy.route); /* always */
		return true;

	case X(ROUTE, UNROUTED, TEMPLATE):
	case X(ROUTE, UNROUTED, PERMANENT):
		add_policy(c, policy.route); /* always */
		if (never_negotiate(c)) {
			if (!unrouted_to_routed_never_negotiate(event, c, e->where)) {
				/* XXX: why whack only? */
				llog(RC_ROUTE, logger, "could not route");
				return true;
			}
			PEXPECT(logger, c->child.routing == RT_ROUTED_NEVER_NEGOTIATE);
		} else {
			if (!unrouted_to_routed_ondemand(event, c, e->where)) {
				/* XXX: why whack only? */
				llog(RC_ROUTE, logger, "could not route");
				return true;
			}
			PEXPECT(logger, c->child.routing == RT_ROUTED_ONDEMAND);
		}
		return true;

	case X(ESTABLISH_IKE, UNROUTED, INSTANCE):
	case X(ESTABLISH_IKE, UNROUTED, PERMANENT):
	case X(ESTABLISH_IKE, UNROUTED_BARE_NEGOTIATION, INSTANCE):
	case X(ESTABLISH_IKE, UNROUTED_BARE_NEGOTIATION, PERMANENT):
		set_established_ike(event, c, RT_UNROUTED_BARE_NEGOTIATION, e);
		return true;

	case X(ESTABLISH_IKE, ROUTED_NEGOTIATION, INSTANCE):
	case X(ESTABLISH_IKE, ROUTED_NEGOTIATION, PERMANENT):
	case X(ESTABLISH_IKE, ROUTED_ONDEMAND, INSTANCE):
	case X(ESTABLISH_IKE, ROUTED_ONDEMAND, PERMANENT):
	case X(ESTABLISH_IKE, ROUTED_TUNNEL, INSTANCE):
	case X(ESTABLISH_IKE, ROUTED_TUNNEL, PERMANENT):
	case X(ESTABLISH_IKE, UNROUTED_NEGOTIATION, INSTANCE):
	case X(ESTABLISH_IKE, UNROUTED_NEGOTIATION, PERMANENT):
	case X(ESTABLISH_IKE, UNROUTED_INBOUND, INSTANCE):
	case X(ESTABLISH_IKE, UNROUTED_INBOUND, PERMANENT):
		/* unchanged; except to attach IKE */
		set_established_ike(event, c, c->child.routing, e);
		return true;

	case X(INITIATE, ROUTED_ONDEMAND, INSTANCE): /* from revival */
	case X(INITIATE, ROUTED_ONDEMAND, PERMANENT):
		flush_routed_ondemand_revival(c);
		routed_ondemand_to_routed_negotiation(event, c, logger, e);
		PEXPECT(logger, c->child.routing == RT_ROUTED_NEGOTIATION);
		return true;

	case X(INITIATE, UNROUTED, INSTANCE):
		/*
		 * Triggered by whack against the template which is
		 * then instantiated creating this connection.  The
		 * template may or may not be routed.
		 */
		if (c->clonedfrom->child.routing == RT_UNROUTED) {
			/*
			 * Since the template has no policy nor
			 * routing, skip these in the instance.
			 */
			ldbg_routing(logger, "skipping hold as template is unrouted");
			set_routing(event, c, RT_UNROUTED_BARE_NEGOTIATION, e);
			return true;
		}
		if (c->clonedfrom->child.routing == RT_ROUTED_ONDEMAND) {
			/*
			 * Need to override the template's policy with our own
			 * (else things will keep acquiring). I's assumed that
			 * the template's routing is sufficient for now.
			 */
			unrouted_instance_to_unrouted_negotiation(event, c, e->where);
			set_routing(event, c, RT_UNROUTED_NEGOTIATION, e);
			return true;
		}
		break;

	case X(INITIATE, UNROUTED, PERMANENT):
		flush_unrouted_revival(c);
		set_initiated(event, c, RT_UNROUTED_BARE_NEGOTIATION, e);
		return true;

	case X(TEARDOWN_IKE, UNROUTED, INSTANCE):
	case X(TEARDOWN_IKE, UNROUTED, PERMANENT):
		/*
		 * already -routed -policy; presumably the Child SA
		 * deleted the policy earlier.
		 */
		return true;

	case X(TEARDOWN_IKE, ROUTED_ONDEMAND, INSTANCE):		/* ikev2-30-rw-no-rekey */
	case X(TEARDOWN_IKE, ROUTED_ONDEMAND, PERMANENT):		/* ROUTED_NEGOTIATION!?! */
		/*
		 * Happens after all children are killed, and
		 * connection put into routed ondemand.  Just need to
		 * delete IKE.
		 *
		 * ikev2-31-nat-rw-no-rekey:
		 *
		 * The established child unroutes the connection;
		 * followed by this IKE timeout.
		 *
		 * ikev2-child-ipsec-retransmit:
		 *
		 * The UP and established child schedules revival,
		 * putting the connection into ROUTED_ONDEMAND,
		 * followed by this IKE timeout.
		 */
		return true;

	case X(RESCHEDULE, UNROUTED_BARE_NEGOTIATION, INSTANCE):
	case X(RESCHEDULE, UNROUTED, PERMANENT):
	case X(TEARDOWN_CHILD, UNROUTED_BARE_NEGOTIATION, PERMANENT):
	case X(TEARDOWN_CHILD, UNROUTED, PERMANENT): /* permanent+up */
	case X(TEARDOWN_CHILD, UNROUTED_BARE_NEGOTIATION, INSTANCE):
	case X(TEARDOWN_IKE, UNROUTED_BARE_NEGOTIATION, INSTANCE):
	case X(TEARDOWN_IKE, UNROUTED_BARE_NEGOTIATION, PERMANENT):
		if (connection_cannot_die(event, c, logger, e)) {
			set_routing(event, c, RT_UNROUTED, NULL);
			return true;
		}
		set_routing(event, c, RT_UNROUTED, NULL);
		return true;

	case X(TEARDOWN_CHILD, ROUTED_NEGOTIATION, PERMANENT):
		/*
		 * For instance, things fail during IKE_AUTH.
		 */
		teardown_routed_negotiation(event, c, (*e->child), logger,
					    e->where, "delete Child SA");
		return true;

	case X(TEARDOWN_IKE, ROUTED_NEGOTIATION, INSTANCE):
	case X(TEARDOWN_IKE, ROUTED_NEGOTIATION, PERMANENT):
	case X(RESCHEDULE, ROUTED_NEGOTIATION, INSTANCE):
	case X(RESCHEDULE, ROUTED_NEGOTIATION, PERMANENT):
		/*
		 * For instance, this end initiated a Child SA for the
		 * connection while at the same time the peer
		 * initiated an IKE SA delete and/or the exchange
		 * timed out.
		 *
		 * Because the Child SA is larval and, presumably,
		 * there is no earlier child the code below, and not
		 * zap_connection(), will need to deal with revival
		 * et.al.
		 */
		/* ex, permanent+up */
		if (connection_cannot_die(event, c, logger, e)) {
			routed_negotiation_to_routed_ondemand(event, c, logger, e->where,
							      "restoring ondemand, reviving");
			PEXPECT(logger, c->child.routing == RT_ROUTED_ONDEMAND);
			return true;
		}
		if (is_instance(c) && is_opportunistic(c)) {
			/*
			 * A failed OE initiator, make shunt bare.
			 */
			orphan_holdpass(c, c->spd, logger);
			/*
			 * Change routing so we don't get cleared out
			 * when state/connection dies.
			 */
			set_routing(event, c, RT_UNROUTED, NULL);
			return true;
		}
		if (c->policy.route) {
			routed_negotiation_to_routed_ondemand(event, c, logger, e->where,
							      "restoring ondemand, connection is routed");
			PEXPECT(logger, c->child.routing == RT_ROUTED_ONDEMAND);
			return true;
		}
		/* is this reachable? */
		routed_kernel_policy_to_unrouted(event, c, DIRECTION_OUTBOUND,
						 logger, e->where, "deleting");
		PEXPECT(logger, c->child.routing == RT_UNROUTED);
		/* connection lives to fight another day */
		return true;

	case X(TEARDOWN_CHILD, UNROUTED_NEGOTIATION, INSTANCE):
		if (connection_cannot_die(event, c, logger, e)) {
			unrouted_kernel_policy_to_unrouted(event, c, DIRECTION_OUTBOUND,
							   logger, e->where, "fail");
			return true;
		}
		if (is_instance(c) && is_opportunistic(c)) {
			/*
			 * A failed OE initiator, make shunt bare.
			 */
			orphan_holdpass(c, c->spd, logger);
			/*
			 * Change routing so we don't get cleared out
			 * when state/connection dies.
			 */
			set_routing(event, c, RT_UNROUTED, NULL);
			return true;
		}
		unrouted_kernel_policy_to_unrouted(event, c, DIRECTION_OUTBOUND,
						   logger, e->where, "fail");
		return true;

	case X(TEARDOWN_IKE, UNROUTED_NEGOTIATION, INSTANCE):
		if (connection_cannot_die(event, c, logger, e)) {
			unrouted_kernel_policy_to_unrouted(event, c, DIRECTION_OUTBOUND,
							   logger, e->where, e->story);
			return true;
		}
		if (is_opportunistic(c)) {
			/*
			 * A failed OE initiator, make shunt bare.
			 */
			orphan_holdpass(c, c->spd, logger);
			/*
			 * Change routing so we don't get cleared out
			 * when state/connection dies.
			 */
			set_routing(event, c, RT_UNROUTED, NULL);
			return true;
		}
		unrouted_kernel_policy_to_unrouted(event, c, DIRECTION_OUTBOUND,
						   logger, e->where, e->story);
		return true;

	case X(TEARDOWN_IKE, ROUTED_TUNNEL, PERMANENT):
	case X(TEARDOWN_IKE, ROUTED_TUNNEL, INSTANCE):
		PEXPECT(c->logger, (*e->ike)->sa.st_ike_version == IKEv1);
		return true;

	case X(TEARDOWN_CHILD, ROUTED_TUNNEL, INSTANCE):
	case X(TEARDOWN_CHILD, ROUTED_TUNNEL, PERMANENT):
		/* permenant connections are never deleted */
		teardown_routed_tunnel(event, c, e->child, e->where);
		return true;

	case X(TEARDOWN_CHILD, ROUTED_INBOUND_NEGOTIATION, INSTANCE):
	case X(TEARDOWN_CHILD, ROUTED_INBOUND_NEGOTIATION, PERMANENT):
		/* total overkill */
		teardown_routed_tunnel(event, c, e->child, e->where);
		return true;

	case X(TEARDOWN_CHILD, UNROUTED_TUNNEL, INSTANCE):
	case X(TEARDOWN_CHILD, UNROUTED_TUNNEL, PERMANENT):
		teardown_unrouted_tunnel(event, c, (*e->child), logger, e->where, e->story);
		return true;

	case X(TEARDOWN_CHILD, UNROUTED_INBOUND, INSTANCE):
	case X(TEARDOWN_CHILD, UNROUTED_INBOUND, PERMANENT):
		/* ikev1-xfrmi-02-aggr */
		/*
		 * IKEv1 responder mid way through establishing child
		 * gets a timeout.  Full down_routed_tunnel is
		 * overkill - just inbound needs to be pulled.
		 */
		teardown_unrouted_inbound(event, c, (*e->child), logger, e->where, e->story);
		return true;

	case X(TEARDOWN_CHILD, UNROUTED_INBOUND_NEGOTIATION, INSTANCE):
	case X(TEARDOWN_CHILD, UNROUTED_INBOUND_NEGOTIATION, PERMANENT):
		/*
		 * IKEv1 responder mid way through establishing child
		 * gets a timeout.  Full down_routed_tunnel is
		 * overkill - just inbound needs to be pulled.
		 */
		ldbg_routing(logger, "OOPS: UNROUTED_INBOUND_NEGOTIATION isn't routed!");
		teardown_unrouted_inbound_negotiation(event, c, (*e->child), logger, e->where, e->story);
		return true;

	case X(ESTABLISH_INBOUND, UNROUTED_BARE_NEGOTIATION, INSTANCE):
	case X(ESTABLISH_INBOUND, UNROUTED_BARE_NEGOTIATION, PERMANENT):
		if (!install_inbound_ipsec_sa((*e->child), RT_UNROUTED_INBOUND, e->where)) {
			return false;
		}
		set_routing(event, c, RT_UNROUTED_INBOUND, e);
		return true;

	case X(ESTABLISH_INBOUND, ROUTED_INBOUND_NEGOTIATION, PERMANENT):
		/* alias-01 */
		if (!install_inbound_ipsec_sa((*e->child), RT_ROUTED_INBOUND_NEGOTIATION, e->where)) {
			return false;
		}
		set_routing(event, c, RT_ROUTED_INBOUND_NEGOTIATION, e);
		return true;

	case X(ESTABLISH_INBOUND, ROUTED_NEGOTIATION, INSTANCE):
	case X(ESTABLISH_INBOUND, ROUTED_NEGOTIATION, PERMANENT):
		/* addconn-05-bogus-left-interface
		 * algo-ikev2-aes128-sha1-ecp256 et.al. */
		if (!install_inbound_ipsec_sa((*e->child), RT_ROUTED_INBOUND_NEGOTIATION, e->where)) {
			return false;
		}
		set_routing(event, c, RT_ROUTED_INBOUND_NEGOTIATION, e);
		return true;

	case X(ESTABLISH_INBOUND, ROUTED_ONDEMAND, INSTANCE):
	case X(ESTABLISH_INBOUND, ROUTED_ONDEMAND, PERMANENT):
		/*
		 * This transition (ignoring IKEv1 responder) is
		 * immediately followed by an event to replace the
		 * outbound on-demand policy.  Hence, don't bother
		 * updating it to routed-negotiation.
		 */
		flush_routed_ondemand_revival(c);
		if (!install_inbound_ipsec_sa((*e->child), RT_ROUTED_INBOUND_NEGOTIATION, e->where)) {
			return false;
		}
		set_routing(event, c, RT_ROUTED_INBOUND_NEGOTIATION, e);
		return true;

	case X(ESTABLISH_INBOUND, ROUTED_TUNNEL, INSTANCE):
	case X(ESTABLISH_INBOUND, ROUTED_TUNNEL, PERMANENT):
		/*
		 * This happens when there's a re-key where the state
		 * is re-established but not the policy (that is left
		 * untouched).
		 *
		 * For instance ikev2-12-transport-psk and
		 * ikev2-28-rw-server-rekey
		 * ikev1-labeled-ipsec-01-permissive.
		 *
		 * XXX: suspect this is too early - for rekey should
		 * only update after new child establishes?
		 */
		if (!install_inbound_ipsec_sa((*e->child), RT_ROUTED_TUNNEL, e->where)) {
			return false;
		}
		set_routing(event, c, RT_ROUTED_TUNNEL, e);
		return true;

	case X(ESTABLISH_INBOUND, UNROUTED, INSTANCE):
	case X(ESTABLISH_INBOUND, UNROUTED, PERMANENT):
		if (!install_inbound_ipsec_sa((*e->child), RT_UNROUTED_INBOUND, e->where)) {
			return false;
		}
		set_routing(event, c, RT_UNROUTED_INBOUND, e);
		return true;

	case X(ESTABLISH_INBOUND, UNROUTED_INBOUND, PERMANENT):
		/* alias-01 */
		if (!install_inbound_ipsec_sa((*e->child), RT_UNROUTED_INBOUND, e->where)) {
			return false;
		}
		set_routing(event, c, RT_UNROUTED_INBOUND, e);
		return true;

	case X(ESTABLISH_INBOUND, UNROUTED_INBOUND_NEGOTIATION, PERMANENT):
		/* alias-01 */
		if (!install_inbound_ipsec_sa((*e->child), RT_UNROUTED_INBOUND_NEGOTIATION, e->where)) {
			return false;
		}
		set_routing(event, c, RT_UNROUTED_INBOUND_NEGOTIATION, e);
		return true;

	case X(ESTABLISH_INBOUND, UNROUTED_NEGOTIATION, INSTANCE):
		if (!install_inbound_ipsec_sa((*e->child), RT_UNROUTED_INBOUND_NEGOTIATION, e->where)) {
			return false;
		}
		set_routing(event, c, RT_UNROUTED_INBOUND_NEGOTIATION, e);
		return true;

	case X(ESTABLISH_OUTBOUND, ROUTED_INBOUND_NEGOTIATION, INSTANCE):
	case X(ESTABLISH_OUTBOUND, ROUTED_INBOUND_NEGOTIATION, PERMANENT):
		if (!install_outbound_ipsec_sa((*e->child), RT_ROUTED_TUNNEL,
					       (struct do_updown) {
						       .up = true,
						       .route = false,
					       }, e->where)) {
			return false;
		}
		set_established_child(event, c, RT_ROUTED_TUNNEL, e);
		return true;

	case X(ESTABLISH_OUTBOUND, ROUTED_TUNNEL, INSTANCE):
	case X(ESTABLISH_OUTBOUND, ROUTED_TUNNEL, PERMANENT):
		/*
		 * This happens when there's a re-key where the state
		 * is re-established but not the policy (that is left
		 * untouched).
		 *
		 * For instance ikev2-12-transport-psk and
		 * ikev2-28-rw-server-rekey
		 * ikev1-labeled-ipsec-01-permissive.
		 */
		if (!install_outbound_ipsec_sa((*e->child), RT_ROUTED_TUNNEL,
					       (struct do_updown) {
						       .up = false,
						       .route = false,
					       }, e->where)) {
			return false;
		}
		set_established_child(event, c, RT_ROUTED_TUNNEL, e);
		return true;

	case X(ESTABLISH_OUTBOUND, UNROUTED_INBOUND, INSTANCE):
	case X(ESTABLISH_OUTBOUND, UNROUTED_INBOUND, PERMANENT):
	case X(ESTABLISH_OUTBOUND, UNROUTED_INBOUND_NEGOTIATION, INSTANCE):
	case X(ESTABLISH_OUTBOUND, UNROUTED_INBOUND_NEGOTIATION, PERMANENT):
		if (!install_outbound_ipsec_sa((*e->child), RT_ROUTED_TUNNEL,
					       (struct do_updown) {
						       .up = true,
						       .route = true,
					       }, e->where)) {
			return false;
		}
		set_established_child(event, c, RT_ROUTED_TUNNEL, e);
		return true;

	case X(SUSPEND, ROUTED_TUNNEL, PERMANENT):
	case X(SUSPEND, ROUTED_TUNNEL, INSTANCE):
		do_updown_child(UPDOWN_DOWN, (*e->child));
		/*
		 * Update connection's routing so that route_owner()
		 * won't find us.
		 *
		 * Only unroute when no other routed connection shares
		 * the SPD.
		 *
		 * XXX: no-op as SPD is still owned?
		 */
		set_routing(event, c, RT_UNROUTED_TUNNEL, e);
		(*e->child)->sa.st_mobike_del_src_ip = true;
		do_updown_unroute(c, (*e->child));
		(*e->child)->sa.st_mobike_del_src_ip = false;
		return true;

	case X(RESUME, UNROUTED_TUNNEL, PERMANENT):
	case X(RESUME, UNROUTED_TUNNEL, INSTANCE):
		set_routing(event, c, RT_ROUTED_TUNNEL, e);
		do_updown_child(UPDOWN_ROUTE, (*e->child));
		do_updown_child(UPDOWN_UP, (*e->child));
		return true;

	case X(ROUTE, UNROUTED_BARE_NEGOTIATION, PERMANENT):
		if (BROKEN_TRANSITION) {
			/*
			 * XXX: should install routing+policy!
			 */
			add_policy(c, policy.route);
			llog(RC_LOG_SERIOUS, logger,
			     "policy ROUTE added to negotiating connection");
			return true;
		}
		break;

	case X(ROUTE, ROUTED_NEGOTIATION, PERMANENT):
		add_policy(c, policy.route);
		llog(RC_LOG_SERIOUS, logger, "connection already routed");
		return true;

	case X(ROUTE, ROUTED_TUNNEL, PERMANENT):
		add_policy(c, policy.route); /* always */
		llog(RC_LOG, logger, "policy ROUTE added to established connection");
		return true;

	case X(UNROUTE, UNROUTED_BARE_NEGOTIATION, INSTANCE):
	case X(UNROUTE, UNROUTED_BARE_NEGOTIATION, PERMANENT):
		set_routing(event, c, RT_UNROUTED, NULL);
		return true;

	case X(UNROUTE, ROUTED_FAILURE, INSTANCE):
	case X(UNROUTE, ROUTED_FAILURE, PERMANENT):
		routed_kernel_policy_to_unrouted(event, c, DIRECTION_OUTBOUND,
						 logger, e->where, "unroute");
		return true;

	case X(UNROUTE, ROUTED_INBOUND_NEGOTIATION, INSTANCE): /* xauth-pluto-25-lsw299 */
	case X(UNROUTE, ROUTED_INBOUND_NEGOTIATION, PERMANENT): /* ikev1-xfrmi-02-aggr */
	case X(UNROUTE, ROUTED_INBOUND_NEGOTIATION, TEMPLATE): /* xauth-pluto-25-lsw299 xauth-pluto-25-mixed-addresspool */
		routed_inbound_negotiation_to_unrouted(event, c, (*e->child), logger, e->where, e->story);
		return true;

	case X(UNROUTE, ROUTED_NEGOTIATION, INSTANCE):
	case X(UNROUTE, ROUTED_NEGOTIATION, PERMANENT):
		routed_kernel_policy_to_unrouted(event, c, DIRECTION_OUTBOUND,
						 logger, e->where, "unroute");
		return true;

	case X(UNROUTE, ROUTED_NEVER_NEGOTIATE, TEMPLATE):
	case X(UNROUTE, ROUTED_NEVER_NEGOTIATE, PERMANENT):
		routed_kernel_policy_to_unrouted(event, c, DIRECTION_INBOUND|DIRECTION_OUTBOUND,
						 logger, e->where, "unroute");
		return true;

	case X(UNROUTE, ROUTED_ONDEMAND, INSTANCE):
	case X(UNROUTE, ROUTED_ONDEMAND, PERMANENT):
	case X(UNROUTE, ROUTED_ONDEMAND, TEMPLATE):
		if (c->local->kind == CK_INSTANCE ||
		    c->local->kind == CK_PERMANENT) {
			flush_routed_ondemand_revival(c);
		}
		routed_kernel_policy_to_unrouted(event, c, DIRECTION_OUTBOUND,
						 logger, e->where, "unroute");
		return true;

	case X(UNROUTE, ROUTED_TUNNEL, INSTANCE):
	case X(UNROUTE, ROUTED_TUNNEL, PERMANENT):
		llog(RC_RTBUSY, logger, "cannot unroute: route busy");
		return true;

	case X(UNROUTE, UNROUTED, GROUP):
	case X(UNROUTE, UNROUTED, TEMPLATE):
	case X(UNROUTE, UNROUTED, PERMANENT):
	case X(UNROUTE, UNROUTED, INSTANCE):
		ldbg_routing(logger, "already unrouted");
		return true;

	case X(UNROUTE, UNROUTED_NEGOTIATION, INSTANCE):
		unrouted_kernel_policy_to_unrouted(event, c, DIRECTION_OUTBOUND,
						   logger, e->where, "unroute");
		return true;

	case X(UNROUTE, UNROUTED_TUNNEL, INSTANCE):
	case X(UNROUTE, UNROUTED_TUNNEL, PERMANENT):
		unrouted_tunnel_to_unrouted(event, c, logger, e->where, "unroute");
		return true;

/*
 * Labeled IPsec.
 */

	case X(ESTABLISH_IKE, UNROUTED, LABELED_PARENT):
		/*
		 * For SEC_LABELs install a trap for any outgoing
		 * connection so that it will trigger an acquire which
		 * will then negotiate the child.
		 *
		 * Because the is_labeled_parent() connection was
		 * instantiated from the is_labeled_template() the
		 * parent is unrouted.
		 *
		 * There's a chance that the is_labeled_template() and
		 * is_labeled_parent() have overlapping SPDs that
		 * seems to do no harm.
		 */
		if (!unrouted_to_routed_ondemand_sec_label(c, logger, e->where)) {
			llog(RC_ROUTE, logger, "could not route");
			return true;
		}
		set_established_ike(event, c, RT_ROUTED_ONDEMAND, e);
		return true;
	case X(ESTABLISH_IKE, ROUTED_ONDEMAND, LABELED_PARENT):
		/*
		 * Presumably a rekey?
		 */
		set_established_ike(event, c, RT_ROUTED_ONDEMAND, e);
		return true;

	case X(ROUTE, UNROUTED, LABELED_TEMPLATE):
		add_policy(c, policy.route); /* always */
		if (never_negotiate(c)) {
			if (!unrouted_to_routed_never_negotiate(event, c, e->where)) {
				/* XXX: why whack only? */
				llog(WHACK_STREAM|RC_ROUTE, logger, "could not route");
				return true;
			}
			PEXPECT(logger, c->child.routing == RT_ROUTED_NEVER_NEGOTIATE);
			return true;
		}
		if (!unrouted_to_routed_ondemand_sec_label(c, logger, e->where)) {
			llog(RC_ROUTE, logger, "could not route");
			return true;
		}
		set_routing(event, c, RT_ROUTED_ONDEMAND, NULL);
		return true;
	case X(ROUTE, ROUTED_ONDEMAND, LABELED_PARENT):
		/*
		 * ikev2-labeled-ipsec-06-rekey-ike-acquire where the
		 * rekey re-routes the existing routed connection from
		 * IKE AUTH.
		 */
		set_routing(event, c, RT_ROUTED_ONDEMAND, NULL);
		return true;
	case X(ROUTE, UNROUTED_BARE_NEGOTIATION, LABELED_PARENT): /* see above */
	case X(ROUTE, UNROUTED, LABELED_PARENT):
		/*
		 * The CK_LABELED_TEMPLATE connection may have been
		 * routed (i.e., route+ondemand), but not this
		 * CK_LABELED_PARENT - it is still negotiating.
		 *
		 * The negotiating LABELED_PARENT connection should be
		 * in UNROUTED_NEGOTIATION but ACQUIRE doesn't yet go
		 * through that path.
		 *
		 * But what if the two have the same SPDs?  Then the
		 * routing happens twice which seems to be harmless.
		 */
		if (!unrouted_to_routed_ondemand_sec_label(c, logger, e->where)) {
			llog(RC_ROUTE, logger, "could not route");
			return true;
		}
		set_routing(event, c, RT_ROUTED_ONDEMAND, NULL);
		return true;
	case X(UNROUTE, UNROUTED, LABELED_TEMPLATE):
	case X(UNROUTE, UNROUTED, LABELED_PARENT):
		ldbg_routing(logger, "already unrouted");
		return true;
	case X(UNROUTE, ROUTED_ONDEMAND, LABELED_TEMPLATE):
	case X(UNROUTE, ROUTED_ONDEMAND, LABELED_PARENT):
		/* labeled ipsec installs both inbound and outbound */
		routed_kernel_policy_to_unrouted(event, c, DIRECTION_INBOUND|DIRECTION_OUTBOUND,
						 logger, e->where, "unroute");
		return true;
	case X(INITIATE, ROUTED_ONDEMAND, LABELED_PARENT):
	case X(INITIATE, UNROUTED, LABELED_CHILD):
	case X(INITIATE, UNROUTED, LABELED_PARENT):
		return true;
	case X(TEARDOWN_IKE, ROUTED_ONDEMAND, LABELED_PARENT):
		/* labeled ipsec installs both inbound and outbound */
		routed_kernel_policy_to_unrouted(event, c, DIRECTION_INBOUND|DIRECTION_OUTBOUND,
						 logger, e->where, "unroute");
		return true;
	case X(TEARDOWN_IKE, UNROUTED, LABELED_PARENT):
		return true;

/*
 * Labeled IPsec child.
 */

	case X(ESTABLISH_INBOUND, UNROUTED_TUNNEL, LABELED_CHILD):
		/* rekey; already up */
		if (!install_inbound_ipsec_sa((*e->child), RT_UNROUTED_TUNNEL, e->where)) {
			return false;
		}
		set_routing(event, c, RT_UNROUTED_TUNNEL, e);
		return true;
	case X(ESTABLISH_OUTBOUND, UNROUTED_TUNNEL, LABELED_CHILD):
		/* rekey; already up */
		if (!install_outbound_ipsec_sa((*e->child), RT_UNROUTED_TUNNEL,
					       (struct do_updown) {
						       .up = false,
						       .route = false,
					       }, e->where)) {
			return false;
		}
		set_established_child(event, c, RT_UNROUTED_TUNNEL, e);
		return true;

	case X(ESTABLISH_INBOUND, UNROUTED, LABELED_CHILD):
		if (!install_inbound_ipsec_sa((*e->child), RT_UNROUTED_INBOUND, e->where)) {
			return false;
		}
		set_routing(event, c, RT_UNROUTED_INBOUND, e);
		return true;
	case X(ESTABLISH_OUTBOUND, UNROUTED_INBOUND, LABELED_CHILD):
		/* new; not up */
		if (!install_outbound_ipsec_sa((*e->child), RT_UNROUTED_TUNNEL,
					       (struct do_updown) {
						       .up = true,
						       .route = false,
					       }, e->where)) {
			return false;
		}
		set_established_child(event, c, RT_UNROUTED_TUNNEL, e);
		return true;

	case X(UNROUTE, UNROUTED_INBOUND, LABELED_CHILD):
	case X(UNROUTE, UNROUTED_TUNNEL, LABELED_CHILD):
		set_routing(event, c, RT_UNROUTED, NULL);
		return true;
	case X(UNROUTE, UNROUTED, LABELED_CHILD):
		ldbg_routing(logger, "already unrouted");
		return true;
	case X(TEARDOWN_CHILD, UNROUTED_INBOUND, LABELED_CHILD):
	case X(TEARDOWN_CHILD, UNROUTED_TUNNEL, LABELED_CHILD):
		set_routing(event, c, RT_UNROUTED, NULL);
		return true;
	case X(RESCHEDULE, UNROUTED, LABELED_CHILD):
		/* drop it on the floor; ike died */
		return true;

	}

	BARF_JAMBUF((DBGP(DBG_ROUTING) ? PASSERT_FLAGS : PEXPECT_FLAGS),
		    c->logger, /*ignore-exit-code*/0, e->where, buf) {
		jam_routing_prefix(buf, "unhandled", event,
				   c->child.routing, c->child.routing,
				   c->local->kind);
		jam_event(buf, c, e);
	}

	return false;
}

static bool dispatch(enum routing_event event,
	      struct connection *c,
	      struct logger *logger, /* must out-live call */
	      const struct routing_annex *e)
{
	PASSERT(logger, e->where != NULL);
	bool ok = true;

	connection_addref_where(c, logger, HERE);
	{
		struct old_routing old = ldbg_routing_start(event, c, logger, e);
		{
			if (e->dispatch_ok == NULL ||
			    e->dispatch_ok(c, logger, e)) {
				ok = dispatch_1(event, c, logger, e);
			}
			if (ok && e->post_op != NULL) {
				e->post_op(e);
			}
		}
		ldbg_routing_stop(event, c, logger, e, &old, ok);
	}
	connection_delref_where(&c, logger, HERE);

	return ok;
}
