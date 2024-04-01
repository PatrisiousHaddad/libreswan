/* IKEv2 state machine, for libreswan
 *
 * Copyright (C) 1997 Angelos D. Keromytis.
 * Copyright (C) 1998-2010,2013-2017 D. Hugh Redelmeier <hugh@mimosa.com>
 * Copyright (C) 2007-2008 Michael Richardson <mcr@xelerance.com>
 * Copyright (C) 2009 David McCullough <david_mccullough@securecomputing.com>
 * Copyright (C) 2008-2011 Paul Wouters <paul@xelerance.com>
 * Copyright (C) 2010 Simon Deziel <simon@xelerance.com>
 * Copyright (C) 2010 Tuomo Soini <tis@foobar.fi>
 * Copyright (C) 2011-2012 Avesh Agarwal <avagarwa@redhat.com>
 * Copyright (C) 2012 Paul Wouters <paul@libreswan.org>
 * Copyright (C) 2012-2019 Paul Wouters <pwouters@redhat.com>
 * Copyright (C) 2013 Matt Rogers <mrogers@redhat.com>
 * Copyright (C) 2015-2019 Andrew Cagney
 * Copyright (C) 2016-2018 Antony Antony <appu@phenome.org>
 * Copyright (C) 2017 Sahana Prasad <sahana.prasad07@gmail.com>
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

#include "defs.h"
#include "state.h"
#include "ikev2_states.h"
#include "demux.h"
#include "ikev2.h"
#include "log.h"
#include "connections.h"
#include "ikev2_notify.h"
#include "ikev2_retransmit.h"
#include "ikev2_ike_sa_init.h"
#include "ikev2_ike_auth.h"
#include "ikev2_ike_sa_init.h"
#include "ikev2_ike_intermediate.h"
#include "ikev2_informational.h"
#include "ikev2_cookie.h"
#include "ikev2_redirect.h"
#include "ikev2_eap.h"
#include "ikev2_create_child_sa.h"

struct ikev2_payload_errors {
	bool bad;
	lset_t excessive;
	lset_t missing;
	lset_t unexpected;
	v2_notification_t notification;
};

static void log_v2_payload_errors(struct logger *logger, struct msg_digest *md,
				  const struct ikev2_payload_errors *errors);

static struct ikev2_payload_errors ikev2_verify_payloads(struct msg_digest *md,
							 const struct payload_summary *summary,
							 const struct ikev2_expected_payloads *payloads);

#define S(KIND, STORY, CAT, ...)					\
	const struct finite_state state_v2_##KIND = {			\
		.kind = STATE_V2_##KIND,				\
		.name = "STATE_V2_"#KIND,				\
		/* Not using #KIND + 6 because of clang's -Wstring-plus-int */ \
		.short_name = #KIND,					\
		.story = STORY,						\
		.category = CAT,					\
		.ike_version = IKEv2,					\
		.nr_transitions = elemsof(KIND##_transitions),		\
		.v2.transitions = KIND##_transitions,			\
		##__VA_ARGS__,						\
	}

/*
 * From RFC 5996 syntax: [optional] and {encrypted}
 *
 * Initiator                         Responder
 * -------------------------------------------------------------------
 *
 * IKE_SA_INIT exchange (initial exchange):
 *
 * HDR, SAi1, KEi, Ni            -->
 *                                 <--  HDR, SAr1, KEr, Nr, [CERTREQ]
 *
 * IKE_AUTH exchange (after IKE_SA_INIT exchange):
 *
 * HDR, SK {IDi, [CERT,] [CERTREQ,]
 *        [IDr,] AUTH, SAi2,
 *        TSi, TSr}              -->
 *                                 <--  HDR, SK {IDr, [CERT,] AUTH,
 *                                           SAr2, TSi, TSr}
 * [Parent SA (SAx1) established. Child SA (SAx2) may have been established]
 *
 *
 * Extended IKE_AUTH (see RFC 5996bis 2.6):
 *
 * HDR(A,0), SAi1, KEi, Ni  -->
 *                              <--  HDR(A,0), N(COOKIE)
 * HDR(A,0), N(COOKIE), SAi1,
 *     KEi, Ni  -->
 *                              <--  HDR(A,B), SAr1, KEr,
 *                                       Nr, [CERTREQ]
 * HDR(A,B), SK {IDi, [CERT,]
 *     [CERTREQ,] [IDr,] AUTH,
 *     SAi2, TSi, TSr}  -->
 *                              <--  HDR(A,B), SK {IDr, [CERT,]
 *                                       AUTH, SAr2, TSi, TSr}
 * [Parent SA (SAx1) established. Child SA (SAx2) may have been established]
 *
 *
 * CREATE_CHILD_SA Exchange (new child variant RFC 5996 1.3.1):
 *
 * HDR, SK {SA, Ni, [KEi],
 *            TSi, TSr}  -->
 *                              <--  HDR, SK {SA, Nr, [KEr],
 *                                       TSi, TSr}
 *
 *
 * CREATE_CHILD_SA Exchange (rekey child variant RFC 5996 1.3.3):
 *
 * HDR, SK {N(REKEY_SA), SA, Ni, [KEi],
 *     TSi, TSr}   -->
 *                    <--  HDR, SK {SA, Nr, [KEr],
 *                             TSi, TSr}
 *
 *
 * CREATE_CHILD_SA Exchange (rekey parent SA variant RFC 5996 1.3.2):
 *
 * HDR, SK {SA, Ni, KEi} -->
 *                            <--  HDR, SK {SA, Nr, KEr}
 */

/* Short forms for building payload type sets */

#define P(N) LELEM(ISAKMP_NEXT_v2##N)

/*
 * IKEv2 State transitions (aka microcodes).
 *
 * This table contains all possible state transitions, some of which
 * involve a message.
 *
 * During initialization this table parsed populating the
 * corresponding IKEv2 finite states.  While not the most efficient,
 * it seems to work.
 */

#define req_clear_payloads message_payloads.required   /* required unencrypted payloads (allows just one) for received packet */
#define opt_clear_payloads message_payloads.optional   /* optional unencrypted payloads (none or one) for received packet */
#define req_enc_payloads   encrypted_payloads.required /* required encrypted payloads (allows just one) for received packet */
#define opt_enc_payloads   encrypted_payloads.optional /* optional encrypted payloads (none or one) for received packet */

/*
 * IKEv2 IKE SA initiator, while the the SA_INIT packet is being
 * constructed, are in state.  Only once the packet has been sent out
 * does it transition to STATE_V2_IKE_SA_INIT_I and start being counted as
 * half-open.
 */

const struct v2_state_transition initiate_v2_IKE_SA_INIT_transition = {
	/* no state:   --> I1
	 * HDR, SAi1, KEi, Ni -->
	 */
	.story      = "initiating IKE_SA_INIT",
	.from = { &state_v2_IKE_SA_INIT_I0, },
	.to = &state_v2_IKE_SA_INIT_I,
	.exchange   = ISAKMP_v2_IKE_SA_INIT,
	.processor  = NULL, /* XXX: should be set */
	.llog_success = llog_v2_success_exchange_sent_to,
	.timeout_event = EVENT_RETRANSMIT,
};

static const struct v2_state_transition IKE_SA_INIT_I0_transitions[] = {
};

S(IKE_SA_INIT_I0, "waiting for KE to finish", CAT_IGNORE);

/*
 * Count I1 as half-open too because with ondemand, a plaintext packet
 * (that is spoofed) will trigger an outgoing IKE SA.
 */

static const struct v2_state_transition IKE_SA_INIT_I_transitions[] = {

	/* STATE_V2_IKE_SA_INIT_I: R1B --> I1B
	 *                     <--  HDR, N
	 * HDR, N, SAi1, KEi, Ni -->
	 */

	{ .story      = "received anti-DDOS COOKIE response; resending IKE_SA_INIT request with cookie payload added",
	  .from = { &state_v2_IKE_SA_INIT_I, },
	  .to = &state_v2_IKE_SA_INIT_I0,
	  .exchange   = ISAKMP_v2_IKE_SA_INIT,
	  .recv_role  = MESSAGE_RESPONSE,
	  .message_payloads.required = P(N),
	  .message_payloads.notification = v2N_COOKIE,
	  .processor  = process_v2_IKE_SA_INIT_response_v2N_COOKIE,
	  .llog_success = ldbg_v2_success,
	  .timeout_event = EVENT_v2_DISCARD, },

	{ .story      = "received INVALID_KE_PAYLOAD response; resending IKE_SA_INIT with new KE payload",
	  .from = { &state_v2_IKE_SA_INIT_I, },
	  .to = &state_v2_IKE_SA_INIT_I0,
	  .exchange   = ISAKMP_v2_IKE_SA_INIT,
	  .recv_role  = MESSAGE_RESPONSE,
	  .message_payloads.required = P(N),
	  .message_payloads.notification = v2N_INVALID_KE_PAYLOAD,
	  .processor  = process_v2_IKE_SA_INIT_response_v2N_INVALID_KE_PAYLOAD,
	  .llog_success = ldbg_v2_success,
	  .timeout_event = EVENT_v2_DISCARD, },

	{ .story      = "received REDIRECT response; resending IKE_SA_INIT request to new destination",
	  .from = { &state_v2_IKE_SA_INIT_I, },
	  .to = &state_v2_IKE_SA_INIT_I0, /* XXX: never happens STF_SUSPEND */
	  .exchange   = ISAKMP_v2_IKE_SA_INIT,
	  .recv_role  = MESSAGE_RESPONSE,
	  .message_payloads.required = P(N),
	  .message_payloads.notification = v2N_REDIRECT,
	  .processor  = process_v2_IKE_SA_INIT_response_v2N_REDIRECT,
	  .llog_success = ldbg_v2_success,
	  .timeout_event = EVENT_v2_DISCARD,
	},

	/* STATE_V2_IKE_SA_INIT_I: R1 --> I2
	 *                     <--  HDR, SAr1, KEr, Nr, [CERTREQ]
	 * HDR, SK {IDi, [CERT,] [CERTREQ,]
	 *      [IDr,] AUTH, SAi2,
	 *      TSi, TSr}      -->
	 */
	{ .story      = "Initiator: process IKE_SA_INIT reply, initiate IKE_AUTH or IKE_INTERMEDIATE",
	  .from = { &state_v2_IKE_SA_INIT_I, },
	  .to = &state_v2_IKE_SA_INIT_IR, /* next exchange does IKE_AUTH | IKE_INTERMEDIATE */
	  .exchange   = ISAKMP_v2_IKE_SA_INIT,
	  .recv_role  = MESSAGE_RESPONSE,
	  .req_clear_payloads = P(SA) | P(KE) | P(Nr),
	  .opt_clear_payloads = P(CERTREQ),
	  .processor  = process_v2_IKE_SA_INIT_response,
	  .llog_success = llog_v2_IKE_SA_INIT_success,
	  .timeout_event = EVENT_v2_DISCARD, /* timeout set by next transition */
	},

};

S(IKE_SA_INIT_I, "sent IKE_SA_INIT request", CAT_HALF_OPEN_IKE_SA);

const struct v2_state_transition initiate_v2_IKE_AUTH_transition = {
	.story      = "initiating IKE_AUTH",
	.from = { &state_v2_IKE_SA_INIT_IR, &state_v2_IKE_INTERMEDIATE_IR, },
	.to = &state_v2_IKE_AUTH_I,
	.exchange   = ISAKMP_v2_IKE_AUTH,
	.processor  = initiate_v2_IKE_AUTH_request,
	.llog_success = llog_v2_success_exchange_sent_to,
	.timeout_event = EVENT_RETRANSMIT,
};

static const struct v2_state_transition IKE_SA_INIT_IR_transitions[] = {
};

S(IKE_SA_INIT_IR, "processed IKE_SA_INIT response, preparing IKE_INTERMEDIATE or IKE_AUTH request", CAT_OPEN_IKE_SA);

/*
 * All IKEv1 MAIN modes except the first (half-open) and last ones are
 * not authenticated.
 */

static const struct v2_state_transition IKE_AUTH_I_transitions[] = {

	/* STATE_V2_IKE_AUTH_I: R2 -->
	 *                     <--  HDR, SK {IDr, [CERT,] AUTH,
	 *                               SAr2, TSi, TSr}
	 * [Parent SA established]
	 */

	/*
	 * This pair of state transitions should be merged?
	 */

	{ .story      = "Initiator: process IKE_AUTH response",
	  .from = { &state_v2_IKE_AUTH_I, },
	  .to = &state_v2_ESTABLISHED_IKE_SA,
	  .flags = { .release_whack = true, },
	  .exchange   = ISAKMP_v2_IKE_AUTH,
	  .recv_role  = MESSAGE_RESPONSE,
	  .req_clear_payloads = P(SK),
	  .req_enc_payloads = P(IDr) | P(AUTH),
	  .opt_enc_payloads = P(CERT) | P(CP) | P(SA) | P(TSi) | P(TSr),
	  .processor  = process_v2_IKE_AUTH_response,
	  .llog_success = ldbg_v2_success,/* logged mid transition */
	  .timeout_event = EVENT_v2_REPLACE,
	},

	{ .story      = "Initiator: processing IKE_AUTH failure response",
	  .from = { &state_v2_IKE_AUTH_I, },
	  .to = &state_v2_IKE_AUTH_I,
	  .exchange   = ISAKMP_v2_IKE_AUTH,
	  .recv_role  = MESSAGE_RESPONSE,
	  .message_payloads = { .required = P(SK), },
	  /* .encrypted_payloads = { .required = P(N), }, */
	  .processor  = process_v2_IKE_AUTH_failure_response,
	  .llog_success = llog_v2_success_state_story,
	},

};

S(IKE_AUTH_I, "sent IKE_AUTH request", CAT_OPEN_IKE_SA, .v2.secured = true);

static const struct v2_state_transition IKE_SA_INIT_R0_transitions[] = {

	/* no state: none I1 --> R1
	 *                <-- HDR, SAi1, KEi, Ni
	 * HDR, SAr1, KEr, Nr, [CERTREQ] -->
	 */
	{ .story      = "Respond to IKE_SA_INIT",
	  .from = { &state_v2_IKE_SA_INIT_R0, },
	  .to = &state_v2_IKE_SA_INIT_R,
	  .exchange   = ISAKMP_v2_IKE_SA_INIT,
	  .recv_role  = MESSAGE_REQUEST,
	  .req_clear_payloads = P(SA) | P(KE) | P(Ni),
	  .processor  = process_v2_IKE_SA_INIT_request,
	  .llog_success = llog_v2_IKE_SA_INIT_success,
	  .timeout_event = EVENT_v2_DISCARD, },

};

S(IKE_SA_INIT_R0, "processing IKE_SA_INIT request", CAT_HALF_OPEN_IKE_SA);

static const struct v2_state_transition IKE_SA_INIT_R_transitions[] = {

	/* STATE_V2_PARENT_R1: I2 --> R2
	 *                  <-- HDR, SK {IDi, [CERT,] [CERTREQ,]
	 *                             [IDr,] AUTH, SAi2,
	 *                             TSi, TSr}
	 * HDR, SK {IDr, [CERT,] AUTH,
	 *      SAr2, TSi, TSr} -->
	 *
	 * [Parent SA established]
	 */

	{ .story      = "Responder: process IKE_INTERMEDIATE request",
	  .from = { &state_v2_IKE_SA_INIT_R, },
	  .to = &state_v2_IKE_INTERMEDIATE_R,
	  .exchange   = ISAKMP_v2_IKE_INTERMEDIATE,
	  .recv_role  = MESSAGE_REQUEST,
	  .req_clear_payloads = P(SK),
	  .req_enc_payloads = LEMPTY,
	  .opt_enc_payloads = LEMPTY,
	  .processor  = process_v2_IKE_INTERMEDIATE_request,
	  .llog_success = llog_v2_success_exchange_processed,
	  .timeout_event = EVENT_v2_DISCARD, },

	{ .story      = "Responder: process IKE_AUTH request",
	  .from = { &state_v2_IKE_SA_INIT_R, },
	  .to = &state_v2_ESTABLISHED_IKE_SA,
	  .flags = { .release_whack = true, },
	  .exchange   = ISAKMP_v2_IKE_AUTH,
	  .recv_role  = MESSAGE_REQUEST,
	  .req_clear_payloads = P(SK),
	  .req_enc_payloads = P(IDi) | P(AUTH),
	  .opt_enc_payloads = P(CERT) | P(CERTREQ) | P(IDr) | P(CP) | P(SA) | P(TSi) | P(TSr),
	  .processor  = process_v2_IKE_AUTH_request,
	  .llog_success = ldbg_v2_success,
	  .timeout_event = EVENT_v2_REPLACE, },

	{ .story      = "Responder: process IKE_AUTH(EAP) request",
	  .from = { &state_v2_IKE_SA_INIT_R, },
	  .to = &state_v2_IKE_AUTH_EAP_R,
	  .exchange   = ISAKMP_v2_IKE_AUTH,
	  .recv_role  = MESSAGE_REQUEST,
	  .req_clear_payloads = P(SK),
	  .req_enc_payloads = P(IDi),
	  .opt_enc_payloads = P(CERTREQ) | P(IDr) | P(CP) | P(SA) | P(TSi) | P(TSr),
	  .processor  = process_v2_IKE_AUTH_request_EAP_start,
	  .llog_success = llog_v2_success_state_story,
	  .timeout_event = EVENT_v2_DISCARD, },

};

S(IKE_SA_INIT_R, "sent IKE_SA_INIT response, waiting for IKE_INTERMEDIATE or IKE_AUTH request", CAT_HALF_OPEN_IKE_SA, .v2.secured = true);

/*
 * IKE_INTERMEDIATE
 */

const struct v2_state_transition initiate_v2_IKE_INTERMEDIATE_transition = {
	.story      = "initiating IKE_INTERMEDIATE",
	.from = { &state_v2_IKE_SA_INIT_IR, &state_v2_IKE_INTERMEDIATE_IR, },
	.to = &state_v2_IKE_INTERMEDIATE_I,
	.exchange   = ISAKMP_v2_IKE_INTERMEDIATE,
	.processor  = initiate_v2_IKE_INTERMEDIATE_request,
	.llog_success = llog_v2_success_exchange_sent_to,
	.timeout_event = EVENT_RETRANSMIT,
};

static const struct v2_state_transition IKE_INTERMEDIATE_R_transitions[] = {

	{ .story      = "processing IKE_INTERMEDIATE request",
	  .from = { &state_v2_IKE_INTERMEDIATE_R, },
	  .to = &state_v2_IKE_INTERMEDIATE_R,
	  .exchange   = ISAKMP_v2_IKE_INTERMEDIATE,
	  .recv_role  = MESSAGE_REQUEST,
	  .req_clear_payloads = P(SK),
	  .req_enc_payloads = LEMPTY,
	  .opt_enc_payloads = LEMPTY,
	  .processor  = process_v2_IKE_INTERMEDIATE_request,
	  .llog_success = llog_v2_success_exchange_processed,
	  .timeout_event = EVENT_v2_DISCARD, },

	{ .story      = "processing IKE_AUTH(EAP) request",
	  .from = { &state_v2_IKE_INTERMEDIATE_R, },
	  .to = &state_v2_IKE_AUTH_EAP_R,
	  .exchange   = ISAKMP_v2_IKE_AUTH,
	  .recv_role  = MESSAGE_REQUEST,
	  .req_clear_payloads = P(SK),
	  .req_enc_payloads = P(IDi),
	  .opt_enc_payloads = P(CERTREQ) | P(IDr) | P(CP) | P(SA) | P(TSi) | P(TSr),
	  .processor  = process_v2_IKE_AUTH_request_EAP_start,
	  .llog_success = llog_v2_success_state_story,
	  .timeout_event = EVENT_v2_DISCARD, },

	{ .story      = "processing IKE_AUTH request",
	  .from = { &state_v2_IKE_INTERMEDIATE_R, },
	  .to = &state_v2_ESTABLISHED_IKE_SA,
	  .flags = { .release_whack = true, },
	  .exchange   = ISAKMP_v2_IKE_AUTH,
	  .recv_role  = MESSAGE_REQUEST,
	  .req_clear_payloads = P(SK),
	  .req_enc_payloads = P(IDi) | P(AUTH),
	  .opt_enc_payloads = P(CERT) | P(CERTREQ) | P(IDr) | P(CP) | P(SA) | P(TSi) | P(TSr),
	  .processor  = process_v2_IKE_AUTH_request,
	  .llog_success = ldbg_v2_success,
	  .timeout_event = EVENT_v2_REPLACE, },

};

S(IKE_INTERMEDIATE_R, "sent IKE_INTERMEDIATE response, waiting for IKE_INTERMEDIATE or IKE_AUTH request", CAT_OPEN_IKE_SA, .v2.secured = true);

static const struct v2_state_transition IKE_INTERMEDIATE_I_transitions[] = {
	{ .story      = "processing IKE_INTERMEDIATE response",
	  .from = { &state_v2_IKE_INTERMEDIATE_I, },
	  .to = &state_v2_IKE_INTERMEDIATE_IR,
	  .exchange   = ISAKMP_v2_IKE_INTERMEDIATE,
	  .recv_role  = MESSAGE_RESPONSE,
	  .req_clear_payloads = P(SK),
	  .opt_clear_payloads = LEMPTY,
	  .processor  = process_v2_IKE_INTERMEDIATE_response,
	  .llog_success = llog_v2_success_exchange_processed,
	  .timeout_event = EVENT_v2_DISCARD, },
};

S(IKE_INTERMEDIATE_I, "sent IKE_INTERMEDIATE request, waiting for response", CAT_OPEN_IKE_SA, .v2.secured = true);

const struct v2_state_transition IKE_INTERMEDIATE_IR_transitions[] = {
};

S(IKE_INTERMEDIATE_IR, "processed IKE_INTERMEDIATE response, initiating IKE_INTERMEDIATE or IKE_AUTH", CAT_OPEN_IKE_SA, .v2.secured = true);

/*
 * EAP
 */

static const struct v2_state_transition IKE_AUTH_EAP_R_transitions[] = {

	{ .story      = "Responder: process IKE_AUTH/EAP, continue EAP",
	  .from = { &state_v2_IKE_AUTH_EAP_R, },
	  .to = &state_v2_IKE_AUTH_EAP_R,
	  .exchange   = ISAKMP_v2_IKE_AUTH,
	  .recv_role  = MESSAGE_REQUEST,
	  .req_clear_payloads = P(SK),
	  .req_enc_payloads = P(EAP),
	  .processor  = process_v2_IKE_AUTH_request_EAP_continue,
	  .llog_success = llog_v2_success_state_story,
	  .timeout_event = EVENT_v2_DISCARD, },

	{ .story      = "Responder: process final IKE_AUTH/EAP",
	  .from = { &state_v2_IKE_AUTH_EAP_R, },
	  .to = &state_v2_ESTABLISHED_IKE_SA,
	  .flags = { .release_whack = true, },
	  .exchange   = ISAKMP_v2_IKE_AUTH,
	  .recv_role  = MESSAGE_REQUEST,
	  .req_clear_payloads = P(SK),
	  .req_enc_payloads = P(AUTH),
	  .processor  = process_v2_IKE_AUTH_request_EAP_final,
	  .llog_success = llog_v2_success_state_story,
	  .timeout_event = EVENT_v2_REPLACE, },

};

S(IKE_AUTH_EAP_R, "sent IKE_AUTH(EAP) response, waiting for IKE_AUTH(EAP) request", CAT_OPEN_IKE_SA, .v2.secured = true);

/*
 * CREATE_CHILD_SA exchanges.
 */

static const struct v2_state_transition REKEY_IKE_I0_transitions[] = {

	/*
	 * Child transitions when rekeying an IKE SA using
	 * CREATE_CHILD_SA.
	 *
	 *   Initiator                         Responder
	 *   --------------------------------------------------------
	 *   HDR, SK {SA, Ni, KEi} -->
	 *                                <--  HDR, SK {SA, Nr, KEr}
	 *
	 * See also IKE SA's state transitions, below, that will
	 * eventually drive these nested state transitions (currently
	 * these are fudged).
	 */

	{ .story      = "initiate rekey IKE_SA (CREATE_CHILD_SA)",
	  .from = { &state_v2_REKEY_IKE_I0, },
	  .to = &state_v2_REKEY_IKE_I1,
	  .exchange   = ISAKMP_v2_CREATE_CHILD_SA,
	  .processor  = initiate_v2_CREATE_CHILD_SA_rekey_ike_request,
	  .llog_success = llog_v2_success_state_story,
	  .timeout_event = EVENT_RETRANSMIT, },

};

S(REKEY_IKE_I0, "STATE_V2_REKEY_IKE_I0", CAT_IGNORE);

static const struct v2_state_transition REKEY_IKE_R0_transitions[] = {

	{ .story      = "process rekey IKE SA request (CREATE_CHILD_SA)",
	  .from = { &state_v2_REKEY_IKE_R0, },
	  .to = &state_v2_ESTABLISHED_IKE_SA,
	  .flags = { .release_whack = true, },
	  .exchange   = ISAKMP_v2_CREATE_CHILD_SA,
	  .recv_role  = MESSAGE_REQUEST,
	  .req_clear_payloads = P(SK),
	  .req_enc_payloads = P(SA) | P(Ni) | P(KE),
	  .opt_enc_payloads = P(N),
	  .processor  = process_v2_CREATE_CHILD_SA_rekey_ike_request,
	  .llog_success = ldbg_v2_success,
	  .timeout_event = EVENT_v2_REPLACE, },

};

/* isn't this an ipsec state */

S(REKEY_IKE_R0, "STATE_V2_REKEY_IKE_R0", CAT_OPEN_IKE_SA, .v2.secured = true);

static const struct v2_state_transition REKEY_IKE_I1_transitions[] = {
	{ .story      = "process rekey IKE SA response (CREATE_CHILD_SA)",
	  .from = { &state_v2_REKEY_IKE_I1, },
	  .to = &state_v2_ESTABLISHED_IKE_SA,
	  .flags = { .release_whack = true, },
	  .exchange   = ISAKMP_v2_CREATE_CHILD_SA,
	  .recv_role  = MESSAGE_RESPONSE,
	  .req_clear_payloads = P(SK),
	  .req_enc_payloads = P(SA) | P(Ni) |  P(KE),
	  .opt_enc_payloads = P(N),
	  .processor  = process_v2_CREATE_CHILD_SA_rekey_ike_response,
	  .llog_success = ldbg_v2_success,
	  .timeout_event = EVENT_v2_REPLACE, },
};

S(REKEY_IKE_I1, "sent CREATE_CHILD_SA request to rekey IKE SA", CAT_OPEN_CHILD_SA, .v2.secured = true);

static const struct v2_state_transition REKEY_CHILD_I0_transitions[] = {

	/*
	 * Child transitions when rekeying a Child SA using
	 * CREATE_CHILD_SA.
	 *
	 *   Initiator                         Responder
	 *   ---------------------------------------------------------
	 *   HDR, SK {N(REKEY_SA), SA, Ni, [KEi,]
	 *            TSi, TSr}  -->
	 *
	 * See also IKE SA's state transitions, below, that will
	 * eventually drive these nested state transitions (currently
	 * these are fudged).
	 */

	{ .story      = "initiate rekey Child SA (CREATE_CHILD_SA)",
	  .from = { &state_v2_REKEY_CHILD_I0, },
	  .to = &state_v2_REKEY_CHILD_I1,
	  .exchange   = ISAKMP_v2_CREATE_CHILD_SA,
	  .processor  = initiate_v2_CREATE_CHILD_SA_rekey_child_request,
	  .llog_success = llog_v2_success_state_story,
	  .timeout_event = EVENT_RETRANSMIT, },

};

S(REKEY_CHILD_I0, "STATE_V2_REKEY_CHILD_I0", CAT_IGNORE);

static const struct v2_state_transition REKEY_CHILD_R0_transitions[] = {

	{ .story      = "process rekey Child SA request (CREATE_CHILD_SA)",
	  .from = { &state_v2_REKEY_CHILD_R0, },
	  .to = &state_v2_ESTABLISHED_CHILD_SA,
	  .flags = { .release_whack = true, },
	  .exchange   = ISAKMP_v2_CREATE_CHILD_SA,
	  .recv_role  = MESSAGE_REQUEST,
	  .message_payloads.required = P(SK),
	  .encrypted_payloads.required = P(SA) | P(Ni) | P(TSi) | P(TSr),
	  .encrypted_payloads.optional = P(KE) | P(N) | P(CP),
	  .encrypted_payloads.notification = v2N_REKEY_SA,
	  .processor  = process_v2_CREATE_CHILD_SA_rekey_child_request,
	  .llog_success = ldbg_v2_success,
	  .timeout_event = EVENT_v2_REPLACE, },

};

S(REKEY_CHILD_R0, "STATE_V2_REKEY_CHILD_R0", CAT_OPEN_CHILD_SA, .v2.secured = true);

static const struct v2_state_transition REKEY_CHILD_I1_transitions[] = {
	{ .story      = "process rekey Child SA response (CREATE_CHILD_SA)",
	  .from = { &state_v2_REKEY_CHILD_I1, },
	  .to = &state_v2_ESTABLISHED_CHILD_SA,
	  .flags = { .release_whack = true, },
	  .exchange   = ISAKMP_v2_CREATE_CHILD_SA,
	  .recv_role  = MESSAGE_RESPONSE,
	  .message_payloads.required = P(SK),
	  .encrypted_payloads.required = P(SA) | P(Ni) | P(TSi) | P(TSr),
	  .encrypted_payloads.optional = P(KE) | P(N) | P(CP),
	  .processor  = process_v2_CREATE_CHILD_SA_child_response,
	  /* .processor  = process_v2_CREATE_CHILD_SA_rekey_child_response, */
	  .llog_success = ldbg_v2_success,
	  .timeout_event = EVENT_v2_REPLACE, },
};

S(REKEY_CHILD_I1, "sent CREATE_CHILD_SA request to rekey IPsec SA", CAT_OPEN_CHILD_SA, .v2.secured = true);

static const struct v2_state_transition NEW_CHILD_I0_transitions[] = {

	/*
	 * Child transitions when creating a new Child SA using
	 * CREATE_CHILD_SA.
	 *
	 *   Initiator                         Responder
	 *   ----------------------------------------------------------
	 *   HDR, SK {SA, Ni, [KEi,]
	 *            TSi, TSr}  -->
	 *
	 * See also IKE SA's state transitions, below, that will
	 * eventually drive these nested state transitions (currently
	 * these are fudged).
	 */

	{ .story      = "initiate create Child SA (CREATE_CHILD_SA)",
	  .from = { &state_v2_NEW_CHILD_I0, },
	  .to = &state_v2_NEW_CHILD_I1,
	  .exchange   = ISAKMP_v2_CREATE_CHILD_SA,
	  .processor  = initiate_v2_CREATE_CHILD_SA_new_child_request,
	  .llog_success = llog_v2_success_state_story,
	  .timeout_event = EVENT_RETRANSMIT, },

};

S(NEW_CHILD_I0, "STATE_V2_NEW_CHILD_I0", CAT_IGNORE);

static const struct v2_state_transition NEW_CHILD_R0_transitions[] = {

	{ .story      = "process create Child SA request (CREATE_CHILD_SA)",
	  .from = { &state_v2_NEW_CHILD_R0, },
	  .to = &state_v2_ESTABLISHED_CHILD_SA,
	  .flags = { .release_whack = true, },
	  .exchange   = ISAKMP_v2_CREATE_CHILD_SA,
	  .recv_role  = MESSAGE_REQUEST,
	  .req_clear_payloads = P(SK),
	  .req_enc_payloads = P(SA) | P(Ni) | P(TSi) | P(TSr),
	  .opt_enc_payloads = P(KE) | P(N) | P(CP),
	  .processor  = process_v2_CREATE_CHILD_SA_new_child_request,
	  .llog_success = ldbg_v2_success,
	  .timeout_event = EVENT_v2_REPLACE, },

};

S(NEW_CHILD_R0, "STATE_V2_NEW_CHILD_R0", CAT_OPEN_CHILD_SA, .v2.secured = true);

static const struct v2_state_transition NEW_CHILD_I1_transitions[] = {
	{ .story      = "process create Child SA response (CREATE_CHILD_SA)",
	  .from = { &state_v2_NEW_CHILD_I1, },
	  .to = &state_v2_ESTABLISHED_CHILD_SA,
	  .flags = { .release_whack = true, },
	  .exchange   = ISAKMP_v2_CREATE_CHILD_SA,
	  .recv_role  = MESSAGE_RESPONSE,
	  .req_clear_payloads = P(SK),
	  .req_enc_payloads = P(SA) | P(Ni) | P(TSi) | P(TSr),
	  .opt_enc_payloads = P(KE) | P(N) | P(CP),
	  .processor  = process_v2_CREATE_CHILD_SA_child_response,
	  /* .processor  = process_v2_CREATE_CHILD_SA_new_child_response, */
	  .llog_success = ldbg_v2_success,
	  .timeout_event = EVENT_v2_REPLACE, },
};

S(NEW_CHILD_I1, "sent CREATE_CHILD_SA request for new IPsec SA", CAT_OPEN_CHILD_SA, .v2.secured = true);

/*
 * IKEv2 established states.
 */

static const struct v2_state_transition ESTABLISHED_IKE_SA_transitions[] = {

	/*
	 * IKE SA's CREATE_CHILD_SA exchange to rekey IKE SA.
	 *
	 * Note the lack of a TS (traffic selectors) payload.  Since
	 * rekey and new Child SA exchanges contain TS they won't
	 * match.
	 *
	 *   Initiator                         Responder
	 *   --------------------------------------------------------
	 *   HDR, SK {SA, Ni, KEi} -->
	 *                                <--  HDR, SK {SA, Nr, KEr}
	 *
	 * XXX: see ikev2_create_child_sa.c for initiator state.
	 */

	{ .story      = "process rekey IKE SA request (CREATE_CHILD_SA)",
	  .from = { &state_v2_ESTABLISHED_IKE_SA, },
	  .to = &state_v2_ESTABLISHED_IKE_SA,
	  .flags = { .release_whack = true, },
	  .exchange   = ISAKMP_v2_CREATE_CHILD_SA,
	  .recv_role  = MESSAGE_REQUEST,
	  .req_clear_payloads = P(SK),
	  .req_enc_payloads = P(SA) | P(Ni) | P(KE),
	  .opt_enc_payloads = P(N),
	  .processor  = process_v2_CREATE_CHILD_SA_rekey_ike_request,
	  .llog_success = ldbg_v2_success,
	  .timeout_event = EVENT_RETAIN },

	{ .story      = "process rekey IKE SA response (CREATE_CHILD_SA)",
	  .from = { &state_v2_ESTABLISHED_IKE_SA, },
	  .to = &state_v2_ESTABLISHED_IKE_SA,
	  .exchange   = ISAKMP_v2_CREATE_CHILD_SA,
	  .recv_role  = MESSAGE_RESPONSE,
	  .req_clear_payloads = P(SK),
	  .req_enc_payloads = P(SA) | P(Ni) |  P(KE),
	  .opt_enc_payloads = P(N),
	  .processor  = process_v2_CREATE_CHILD_SA_rekey_ike_response,
	  .llog_success = ldbg_v2_success,
	  .timeout_event = EVENT_RETAIN, },

	/*
	 * IKE SA's CREATE_CHILD_SA request to rekey a Child SA.
	 *
	 * This transition expects both TS (traffic selectors) and
	 * N(REKEY_SA)) payloads.  The rekey Child SA request will
	 * match this, the new Child SA will not and match the weaker
	 * transition that follows.
	 *
	 *   Initiator                         Responder
	 *   ---------------------------------------------------------
	 *   HDR, SK {N(REKEY_SA), SA, Ni, [KEi,]
	 *            TSi, TSr}  -->
	 *
	 * XXX: see ikev2_create_child_sa.c for initiator state.
	 */

	{ .story      = "process rekey Child SA request (CREATE_CHILD_SA)",
	  .from = { &state_v2_ESTABLISHED_IKE_SA, },
	  .to = &state_v2_ESTABLISHED_IKE_SA,
	  .flags = { .release_whack = true, },
	  .exchange   = ISAKMP_v2_CREATE_CHILD_SA,
	  .recv_role  = MESSAGE_REQUEST,
	  .message_payloads.required = P(SK),
	  .encrypted_payloads.required = P(SA) | P(Ni) | P(TSi) | P(TSr),
	  .encrypted_payloads.optional = P(KE) | P(N) | P(CP),
	  .encrypted_payloads.notification = v2N_REKEY_SA,
	  .processor  = process_v2_CREATE_CHILD_SA_rekey_child_request,
	  .llog_success = ldbg_v2_success,
	  .timeout_event = EVENT_RETAIN, },

	/*
	 * IKE SA's CREATE_CHILD_SA request to create a new Child SA.
	 *
	 * Note the presence of just TS (traffic selectors) payloads.
	 * Earlier rules will have weeded out both rekey IKE (no TS
	 * payload) and rekey Child (has N(REKEY_SA)) leaving just
	 * create new Child SA.
	 *
	 *   Initiator                         Responder
	 *   ----------------------------------------------------------
	 *   HDR, SK {SA, Ni, [KEi,]
	 *            TSi, TSr}  -->
	 *
	 * XXX: see ikev2_create_child_sa.c for initiator state.
	 */

	{ .story      = "process create Child SA request (CREATE_CHILD_SA)",
	  .from = { &state_v2_ESTABLISHED_IKE_SA, },
	  .to = &state_v2_ESTABLISHED_IKE_SA,
	  .flags = { .release_whack = true, },
	  .exchange   = ISAKMP_v2_CREATE_CHILD_SA,
	  .recv_role  = MESSAGE_REQUEST,
	  .req_clear_payloads = P(SK),
	  .req_enc_payloads = P(SA) | P(Ni) | P(TSi) | P(TSr),
	  .opt_enc_payloads = P(KE) | P(N) | P(CP),
	  .processor  = process_v2_CREATE_CHILD_SA_new_child_request,
	  .llog_success = ldbg_v2_success,
	  .timeout_event = EVENT_RETAIN, },

	/*
	 * IKE SA's CREATE_CHILD_SA response to rekey or create a Child SA
	 *
	 * Both rekey and new Child SA share a common transition.  It
	 * isn't immediately possible to differentiate between them.
	 * Instead .st_v2_larval_initiator_sa is used.
	 *
	 *                                <--  HDR, SK {SA, Nr, [KEr,]
	 *                                              TSi, TSr}
	 */

	{ .story      = "process Child SA response (new or rekey) (CREATE_CHILD_SA)",
	  .from = { &state_v2_ESTABLISHED_IKE_SA, },
	  .to = &state_v2_ESTABLISHED_IKE_SA,
	  .flags = { .release_whack = true, },
	  .exchange   = ISAKMP_v2_CREATE_CHILD_SA,
	  .recv_role  = MESSAGE_RESPONSE,
	  .message_payloads.required = P(SK),
	  .encrypted_payloads.required = P(SA) | P(Ni) | P(TSi) | P(TSr),
	  .encrypted_payloads.optional = P(KE) | P(N) | P(CP),
	  .processor  = process_v2_CREATE_CHILD_SA_child_response,
	  .llog_success = ldbg_v2_success,
	  .timeout_event = EVENT_RETAIN, },

	{ .story      = "process CREATE_CHILD_SA failure response (new or rekey Child SA, rekey IKE SA)",
	  .from = { &state_v2_ESTABLISHED_IKE_SA, },
	  .to = &state_v2_ESTABLISHED_IKE_SA,
	  .flags = { .release_whack = true, },
	  .exchange   = ISAKMP_v2_CREATE_CHILD_SA,
	  .recv_role  = MESSAGE_RESPONSE,
	  .message_payloads = { .required = P(SK), },
	  .processor  = process_v2_CREATE_CHILD_SA_failure_response,
	  .llog_success = ldbg_v2_success,
	  .timeout_event = EVENT_RETAIN, /* no timeout really */
	},

	/* Informational Exchange */

	/* RFC 5996 1.4 "The INFORMATIONAL Exchange"
	 *
	 * HDR, SK {[N,] [D,] [CP,] ...}  -->
	 *   <--  HDR, SK {[N,] [D,] [CP], ...}
	*
	 * A liveness exchange is a special empty message.
	 *
	 * XXX: since these just generate an empty response, they
	 * might as well have a dedicated liveness function.
	 *
	 * XXX: rather than all this transition duplication, the
	 * established states should share common transition stored
	 * outside of this table.
	 */

	{ .story      = "Informational Request (liveness probe)",
	  .from = { &state_v2_ESTABLISHED_IKE_SA, },
	  .to = &state_v2_ESTABLISHED_IKE_SA,
	  .exchange   = ISAKMP_v2_INFORMATIONAL,
	  .recv_role  = MESSAGE_REQUEST,
	  .message_payloads.required = P(SK),
	  .processor  = process_v2_INFORMATIONAL_request,
	  .llog_success = ldbg_v2_success,
	  .timeout_event = EVENT_RETAIN, },

	{ .story      = "Informational Response (liveness probe)",
	  .from = { &state_v2_ESTABLISHED_IKE_SA, },
	  .to = &state_v2_ESTABLISHED_IKE_SA,
	  .flags = { .release_whack = true, },
	  .exchange   = ISAKMP_v2_INFORMATIONAL,
	  .recv_role  = MESSAGE_RESPONSE,
	  .message_payloads.required = P(SK),
	  .processor  = process_v2_INFORMATIONAL_response,
	  .llog_success = ldbg_v2_success,
	  .timeout_event = EVENT_RETAIN, },

	{ .story      = "Informational Request",
	  .from = { &state_v2_ESTABLISHED_IKE_SA, },
	  .to = &state_v2_ESTABLISHED_IKE_SA,
	  .exchange   = ISAKMP_v2_INFORMATIONAL,
	  .recv_role  = MESSAGE_REQUEST,
	  .req_clear_payloads = P(SK),
	  .opt_enc_payloads = P(N) | P(D) | P(CP),
	  .processor  = process_v2_INFORMATIONAL_request,
	  .llog_success = ldbg_v2_success,
	  .timeout_event = EVENT_RETAIN, },

	{ .story      = "Informational Response",
	  .from = { &state_v2_ESTABLISHED_IKE_SA, },
	  .to = &state_v2_ESTABLISHED_IKE_SA,
	  .exchange   = ISAKMP_v2_INFORMATIONAL,
	  .recv_role  = MESSAGE_RESPONSE,
	  .req_clear_payloads = P(SK),
	  .opt_enc_payloads = P(N) | P(D) | P(CP),
	  .processor  = process_v2_INFORMATIONAL_response,
	  .llog_success = ldbg_v2_success,
	  .timeout_event = EVENT_RETAIN, },

};

S(ESTABLISHED_IKE_SA, "established IKE SA", CAT_ESTABLISHED_IKE_SA, .v2.secured = true);

static const struct v2_state_transition IKE_SA_DELETE_transitions[] = {

	{ .story      = "IKE_SA_DEL: process INFORMATIONAL response",
	  .from = { &state_v2_IKE_SA_DELETE, },
	  .to = &state_v2_IKE_SA_DELETE,
	  .exchange   = ISAKMP_v2_INFORMATIONAL,
	  .recv_role  = MESSAGE_RESPONSE,
	  .req_clear_payloads = P(SK),
	  .opt_enc_payloads = P(N) | P(D) | P(CP),
	  .processor  = IKE_SA_DEL_process_v2_INFORMATIONAL_response,
	  .llog_success = ldbg_v2_success,
	  .timeout_event = EVENT_RETAIN, },

#undef req_clear_payloads
#undef opt_clear_payloads
#undef req_enc_payloads
#undef opt_enc_payloads

};

static const struct v2_state_transition ESTABLISHED_CHILD_SA_transitions[] = {
};

S(ESTABLISHED_CHILD_SA, "established Child SA", CAT_ESTABLISHED_CHILD_SA);

/* ??? better story needed for these */
S(IKE_SA_DELETE, "STATE_IKESA_DEL", CAT_ESTABLISHED_IKE_SA, .v2.secured = true);

static const struct v2_state_transition CHILD_SA_DELETE_transitions[] = {
};

S(CHILD_SA_DELETE, "STATE_CHILDSA_DEL", CAT_INFORMATIONAL);

#undef S

static const struct finite_state *v2_states[] = {
#define S(KIND, ...) [STATE_V2_##KIND - STATE_IKEv2_FLOOR] = &state_v2_##KIND
	S(IKE_SA_INIT_I0),
	S(IKE_SA_INIT_I),
	S(IKE_SA_INIT_R0),
	S(IKE_SA_INIT_R),
	S(IKE_SA_INIT_IR),
	S(IKE_INTERMEDIATE_I),
	S(IKE_INTERMEDIATE_R),
	S(IKE_INTERMEDIATE_IR),
	S(IKE_AUTH_EAP_R),
	S(IKE_AUTH_I),
	S(NEW_CHILD_I0),
	S(NEW_CHILD_I1),
	S(NEW_CHILD_R0),
	S(REKEY_CHILD_I0),
	S(REKEY_CHILD_I1),
	S(REKEY_CHILD_R0),
	S(REKEY_IKE_I0),
	S(REKEY_IKE_I1),
	S(REKEY_IKE_R0),
	S(ESTABLISHED_IKE_SA),
	S(ESTABLISHED_CHILD_SA),
	S(IKE_SA_DELETE),
	S(CHILD_SA_DELETE),
#undef S
};

/* Short forms for building payload type sets */

#define P(N) LELEM(ISAKMP_NEXT_v2##N)

/* From RFC 5996:
 *
 * 3.10 "Notify Payload": N payload may appear in any message
 *
 *      During the initial exchange (SA_INIT) (i.e., DH has been
 *      established) the notify payload can't be encrypted.  For all
 *      other exchanges it should be part of the SK (encrypted)
 *      payload (but beware the DH failure exception).
 *
 * 3.11 "Delete Payload": multiple D payloads may appear in an
 *	Informational exchange
 *
 * 3.12 "Vendor ID Payload": (multiple) may appear in any message
 *
 *      During the initial exchange (SA_INIT) (i.e., DH has been
 *      established) the vendor payload can't be encrypted.  For all
 *      other exchanges it should be part of the SK (encrypted)
 *      payload (but beware the DH failure exception).
 *
 * 3.15 "Configuration Payload":
 * 1.4 "The INFORMATIONAL Exchange": (multiple) Configuration Payloads
 *	may appear in an Informational exchange
 * 2.19 "Requesting an Internal Address on a Remote Network":
 *	In all cases, the CP payload MUST be inserted before the SA payload.
 *	In variations of the protocol where there are multiple IKE_AUTH
 *	exchanges, the CP payloads MUST be inserted in the messages
 *	containing the SA payloads.
 */

static const lset_t everywhere_payloads = P(N) | P(V);	/* can appear in any packet */
static const lset_t repeatable_payloads = P(N) | P(D) | P(CP) | P(V) | P(CERT) | P(CERTREQ);	/* if one can appear, many can appear */

struct ikev2_payload_errors ikev2_verify_payloads(struct msg_digest *md,
						  const struct payload_summary *summary,
						  const struct ikev2_expected_payloads *payloads)
{
	/*
	 * Convert SKF onto SK for the comparison (but only when it is
	 * on its own).
	 */
	lset_t seen = summary->present;
	if ((seen & (P(SKF)|P(SK))) == P(SKF)) {
		seen &= ~P(SKF);
		seen |= P(SK);
	}

	lset_t req_payloads = payloads->required;
	lset_t opt_payloads = payloads->optional;

	struct ikev2_payload_errors errors = {
		.bad = false,
		.excessive = summary->repeated & ~repeatable_payloads,
		.missing = req_payloads & ~seen,
		.unexpected = seen & ~req_payloads & ~opt_payloads & ~everywhere_payloads,
	};

	if ((errors.excessive | errors.missing | errors.unexpected) != LEMPTY) {
		errors.bad = true;
	}

	if (payloads->notification != v2N_NOTHING_WRONG) {
		enum v2_pd v2_pd = v2_pd_from_notification(payloads->notification);
		if (md->pd[v2_pd] == NULL) {
			errors.bad = true;
			errors.notification = payloads->notification;
		}
	}

	return errors;
}

static const struct v2_state_transition *v2_state_transition(struct logger *logger,
							     const struct finite_state *state,
							     struct msg_digest *md,
							     bool check_secured_payloads,
							     bool *secured_payload_failed)
{
	struct ikev2_payload_errors message_payload_status = { .bad = false };
	struct ikev2_payload_errors encrypted_payload_status = { .bad = false };

	LDBGP_JAMBUF(DBG_BASE, logger, buf) {
		jam(buf, "looking for transition from %s matching ",
		    state->short_name);
		jam_msg_digest(buf, md);
		if (!check_secured_payloads) {
			jam(buf, " (ignoring secured payloads)");
		}
	}

	for (unsigned i = 0; i < state->nr_transitions; i++) {
		const struct v2_state_transition *transition = &state->v2.transitions[i];

		dbg("  trying: %s", transition->story);

		/* message type? */
		if (transition->exchange != md->hdr.isa_xchg) {
			enum_buf xb;
			dbg("    exchange type does not match %s",
			    str_enum_short(&ikev2_exchange_names, transition->exchange, &xb));
			continue;
		}

		/* role? */
		if (transition->recv_role != v2_msg_role(md)) {
			dbg("     message role does not match %s",
			    (transition->recv_role == MESSAGE_REQUEST ? "request" :
			     transition->recv_role == MESSAGE_RESPONSE ? "response" :
			     "no-message"));
			continue;
		}

		/* message payloads */
		if (!pexpect(md->message_payloads.parsed)) {
			return NULL;
		}
		struct ikev2_payload_errors message_payload_errors
			= ikev2_verify_payloads(md, &md->message_payloads,
						&transition->message_payloads);
		if (message_payload_errors.bad) {
			dbg("    message payloads do not match");
			/* save error for last pattern!?! */
			message_payload_status = message_payload_errors;
			continue;
		}

		/*
		 * If the transition isn't secured (there is no SK or
		 * SKF payload) then checking is complete and things
		 * have matched.
		 */
		if (!state->v2.secured) {
			pexpect((transition->message_payloads.required & P(SK)) == LEMPTY);
			dbg("    unsecured message matched");
			return transition;
		}

		/*
		 * Sniff test; is there at least one plausible payload
		 * without looking at the SK payload?
		 */
		if (!check_secured_payloads) {
			dbg("    matching by ignoring secured payloads");
			return transition;
		}

		/* SK{} payloads */
		if (!pexpect(md->encrypted_payloads.parsed) ||
		    !pexpect(state->v2.secured)) {
			return NULL;
		}
		struct ikev2_payload_errors encrypted_payload_errors
			= ikev2_verify_payloads(md, &md->encrypted_payloads,
						&transition->encrypted_payloads);
		if (encrypted_payload_errors.bad) {
			dbg("    secured payloads do not match");
			/* save error for last pattern!?! */
			encrypted_payload_status = encrypted_payload_errors;
			continue;
		}

		dbg("    secured message matched");
		return transition;
	}

	/*
	 * Always log an error.
	 *
	 * Does the order of message_payload vs secured_payload
	 * matter?  Probably not: all the state transitions for a
	 * secured state have the same message payload set so either
	 * they all match or they all fail.
	 */
	if (message_payload_status.bad) {
		/*
		 * A very messed up message - none of the state
		 * transitions recognized it!.
		 */
		log_v2_payload_errors(logger, md,
				      &message_payload_status);
	} else if (encrypted_payload_status.bad) {
		log_v2_payload_errors(logger, md,
				      &encrypted_payload_status);
		/*
		 * Notify caller so that evasive action can be taken.
		 */
		passert(secured_payload_failed != NULL);
		*secured_payload_failed = true;
	} else {
		llog(RC_LOG/*RC_LOG_SERIOUS*/, logger,
		     "no useful state microcode entry found for incoming packet");
	}
	return NULL;

}

bool sniff_v2_state_transition(struct logger *logger, const struct finite_state *state,
			       struct msg_digest *md)
{
	bool secured_payload_failed; /* ignored */
	return v2_state_transition(logger, state, md, /*check-secured-payloads?*/false,
				   &secured_payload_failed) != NULL;
}

const struct v2_state_transition *find_v2_state_transition(struct logger *logger,
							   const struct finite_state *state,
							   struct msg_digest *md,
							   bool *secured_payload_failed)
{
	return v2_state_transition(logger, state, md, /*check-secured-payloads?*/true,
				   secured_payload_failed);
}

/*
 * report problems - but less so when OE
 */

void log_v2_payload_errors(struct logger *logger, struct msg_digest *md,
			   const struct ikev2_payload_errors *errors)
{
	enum stream log_stream;
	if (suppress_log(logger)) {
		if (DBGP(DBG_BASE)) {
			log_stream = DEBUG_STREAM;
		} else {
			/*
			 * presumably the responder so tone things
			 * down
			 */
			return;
		}
	} else {
		log_stream = ALL_STREAMS;
	}

	LLOG_JAMBUF(RC_LOG_SERIOUS | log_stream, logger, buf) {
		const enum isakmp_xchg_type ix = md->hdr.isa_xchg;
		jam(buf, "dropping unexpected ");
		jam_enum_short(buf, &ikev2_exchange_names, ix);
		jam(buf, " message");
		/* we want to print and log the first notify payload */
		struct payload_digest *ntfy = md->chain[ISAKMP_NEXT_v2N];
		if (ntfy != NULL) {
			jam(buf, " containing ");
			jam_enum_short(buf, &v2_notification_names,
				       ntfy->payload.v2n.isan_type);
			if (ntfy->next != NULL) {
				jam(buf, "...");
			}
			jam(buf, " notification");
		}
		if (md->message_payloads.parsed) {
			jam(buf, "; message payloads: ");
			jam_lset_short(buf, &ikev2_payload_names, ",",
				       md->message_payloads.present);
		}
		if (md->encrypted_payloads.parsed) {
			jam(buf, "; encrypted payloads: ");
			jam_lset_short(buf, &ikev2_payload_names, ",",
				       md->encrypted_payloads.present);
		}
		if (errors->missing != LEMPTY) {
			jam(buf, "; missing payloads: ");
			jam_lset_short(buf, &ikev2_payload_names, ",",
				       errors->missing);
		}
		if (errors->unexpected != LEMPTY) {
			jam(buf, "; unexpected payloads: ");
			jam_lset_short(buf, &ikev2_payload_names, ",",
				       errors->unexpected);
		}
		if (errors->excessive != LEMPTY) {
			jam(buf, "; excessive payloads: ");
			jam_lset_short(buf, &ikev2_payload_names, ",",
				       errors->excessive);
		}
		if (errors->notification != v2N_NOTHING_WRONG) {
			jam(buf, "; missing notification ");
			jam_enum_short(buf, &v2_notification_names,
				       errors->notification);
		}
	}
}

void init_ikev2_states(struct logger *logger)
{
	ldbg(logger, "checking IKEv2 state table");
	/* XXX: debug this using <<--selftest --debug-all --stderrlog>> */

	/*
	 * Fill in FINITE_STATES[].
	 *
	 * This is a hack until each finite-state is a separate object
	 * with corresponding edges (aka microcodes).
	 *
	 * XXX: Long term goal is to have a constant FINITE_STATES[]
	 * contain constant pointers and this static writeable array
	 * to just go away.
	 */
	for (enum state_kind kind = STATE_IKEv2_FLOOR; kind < STATE_IKEv2_ROOF; kind++) {
		/* fill in using static struct */
		const struct finite_state *fs = v2_states[kind - STATE_IKEv2_FLOOR];
		if (fs == NULL) {
			llog_passert(logger, HERE, "entry %d is NULL", kind);
		}
		passert(fs->kind == kind);
		passert(finite_states[kind] == NULL);
		finite_states[kind] = fs;
	}

	/*
	 * Iterate over the state transitions filling in missing bits
	 * and checking for consistency.
	 *
	 * XXX: this misses magic state transitions, such as
	 * v2_liveness_probe, that are not directly attached to a
	 * state.
	 */

	for (enum state_kind kind = STATE_IKEv2_FLOOR; kind < STATE_IKEv2_ROOF; kind++) {
		/* fill in using static struct */
		const struct finite_state *from = finite_states[kind];

		if (DBGP(DBG_BASE)) {
			LLOG_JAMBUF(DEBUG_STREAM, logger, buf) {
				jam(buf, "  ");
				jam_finite_state(buf, from);
			}
		}

		passert(from != NULL);
		passert(from->kind == kind);
		passert(from->ike_version == IKEv2);

		for (unsigned u = 0; u < from->nr_transitions; u++) {
			const struct v2_state_transition *t = &from->v2.transitions[u];

			bool found_from = false;
			FOR_EACH_ELEMENT(f, t->from) {
				if (*f == from) {
					found_from = true;
				}
			}
			passert(found_from);

			const struct finite_state *to = t->to;
			passert(to != NULL);
			passert(to->kind >= STATE_IKEv2_FLOOR);
			passert(to->kind < STATE_IKEv2_ROOF);
			passert(to->ike_version == IKEv2);

			if (DBGP(DBG_BASE)) {

				LLOG_JAMBUF(DEBUG_STREAM, logger, buf) {
					jam_string(buf, "    -> ");
					jam_string(buf, to->short_name);
					jam_string(buf, "; ");
					jam_enum_short(buf, &event_type_names, t->timeout_event);
				}

				LLOG_JAMBUF(DEBUG_STREAM, logger, buf) {
					jam_string(buf, "       ");
					switch (t->recv_role) {
					case NO_MESSAGE:
						/* reverse polarity */
						jam_string(buf, "initiate");
						break;
					case MESSAGE_REQUEST:
						jam_string(buf, "respond");
						break;
					case MESSAGE_RESPONSE:
						jam_string(buf, "response");
						break;
					default:
						bad_case(t->recv_role);
					}
					jam_string(buf, ": ");
					jam_enum_short(buf, &ikev2_exchange_names, t->exchange);
					jam_string(buf, "; ");
					jam_string(buf, "payloads: ");
					FOR_EACH_THING(payloads, &t->message_payloads, &t->encrypted_payloads) {
						if (payloads->required == LEMPTY &&
						    payloads->optional == LEMPTY) {
							continue;
						}
						bool encrypted = (payloads == &t->encrypted_payloads);
						/* assumes SK is last!!! */
						if (encrypted) {
							jam_string(buf, " {");
						}
						const char *sep = "";
						FOR_EACH_THING(payload, &payloads->required, &payloads->optional) {
							if (*payload == LEMPTY) continue;
							bool optional = (payload == &payloads->optional);
							jam_string(buf, sep); sep = " ";
							if (optional) jam(buf, "[");
							jam_lset_short(buf, &ikev2_payload_names, optional ? "] [" : " ", *payload);
							if (optional) jam(buf, "]");
						}
						if (payloads->notification != 0) {
							jam(buf, " N(");
							jam_enum_short(buf, &v2_notification_names, payloads->notification);
							jam(buf, ")");
						}
						if (encrypted) {
							jam(buf, "}");
						}
					}
				}

				DBG_log("       story: %s", t->story);
			}

			/*
			 * Check that the NOTIFY -> PBS ->
			 * MD.pbs[]!=NULL will work.
			 */
			if (t->message_payloads.notification != v2N_NOTHING_WRONG) {
				passert(v2_pd_from_notification(t->message_payloads.notification) != PD_v2_INVALID);
			}
			if (t->encrypted_payloads.notification != v2N_NOTHING_WRONG) {
				passert(v2_pd_from_notification(t->encrypted_payloads.notification) != PD_v2_INVALID);
			}

			passert(t->exchange != 0);

			/*
			 * Check that all transitions from a secured
			 * state require an SK payload.
			 */
			passert(t->recv_role == NO_MESSAGE ||
				LIN(P(SK), t->message_payloads.required) == from->v2.secured);

			/*
			 * Check that only IKE_SA_INIT transitions are
			 * from an unsecured state.
			 */
			if (t->recv_role != 0) {
				passert((t->exchange == ISAKMP_v2_IKE_SA_INIT) == !from->v2.secured);
			}

			/*
			 * Check that everything has either a success story,
			 * or suppressed logging.
			 */
			passert(t->llog_success != NULL);

		}

	}

}
