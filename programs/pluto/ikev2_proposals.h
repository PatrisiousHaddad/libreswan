/* IKEv2 SA (Secure Association) Payloads, for libreswan
 *
 * Copyright (C) 2007 Michael Richardson <mcr@xelerance.com>
 * Copyright (C) 2008-2011 Paul Wouters <paul@xelerance.com>
 * Copyright (C) 2008 Antony Antony <antony@xelerance.com>
 * Copyright (C) 2012,2107 Antony Antony <antony@phenome.org>
 * Copyright (C) 2012-2013,2017 Paul Wouters <pwouters@redhat.com>
 * Copyright (C) 2012 Avesh Agarwal <avagarwa@redhat.com>
 * Copyright (C) 2012-2019 D. Hugh Redelmeier <hugh@mimosa.com>
 * Copyright (C) 2015-2019 Andrew Cagney <cagney@gnu.org>
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

#ifndef IKEV2_SA_PAYLOAD_H
#define IKEV2_SA_PAYLOAD_H

void DBG_log_ikev2_proposal(const char *prefix,
			    const struct ikev2_proposal *proposal);

void llog_v2_proposals(lset_t rc_flags, struct logger *logger,
		       const struct ikev2_proposals *proposals,
		       const char *title);

/*
 * Compare all remote proposals against all local proposals finding
 * and returning the "first" local proposal to match.
 *
 * The need to load all the remote proposals into buffers is avoided
 * by processing them in a single pass.  This is a tradeoff.  Since each
 * remote proposal in turn is compared against all local proposals
 * (and not each local proposal in turn compared against all remote
 * proposals) a local proposal matching only the last remote proposal
 * takes more comparisons.  On the other hand, mallocing and pointer
 * juggling is avoided.
 */

v2_notification_t ikev2_process_sa_payload(const char *what,
					   pb_stream *sa_payload,
					   bool expect_ike,
					   bool expect_spi,
					   bool expect_accepted,
					   bool opportunistic,
					   struct ikev2_proposal **chosen_proposal,
					   const struct ikev2_proposals *local_proposals,
					   struct logger *logger);

bool ikev2_emit_sa_proposals(struct pbs_out *pbs,
			     const struct ikev2_proposals *proposals,
			     const shunk_t local_spi);

bool ikev2_emit_sa_proposal(pb_stream *pbs,
			    const struct ikev2_proposal *proposal,
			    shunk_t local_spi);

bool ikev2_proposal_to_trans_attrs(const struct ikev2_proposal *proposal,
				   struct trans_attrs *ta_out, struct logger *logger);

bool ikev2_proposal_to_proto_info(const struct ikev2_proposal *proposal,
				  struct ipsec_proto_info *proto_info,
				  struct logger *logger);

void free_ikev2_proposals(struct ikev2_proposals **proposals);

void free_ikev2_proposal(struct ikev2_proposal **proposal);

/*
 * On-demand compute and return the IKE proposals for the connection.
 *
 * If the default alg_info_ike includes unknown algorithms those get
 * dropped, which can lead to no proposals.
 *
 * Never returns NULL (see passert).
 */

struct ikev2_proposals *get_v2_ike_proposals(struct connection *c);

struct ikev2_proposals *get_v2_child_proposals(struct connection *c,
					       const char *why,
					       const struct dh_desc *default_dh,
					       struct logger *logger);

const struct ikev2_proposals *get_v2_create_child_proposals(const char *why,
							    struct child_sa *child,
							    const struct dh_desc *default_dh);

/*
 * Return the first valid DH proposal that is supported.
 */
const struct dh_desc *ikev2_proposals_first_dh(const struct ikev2_proposals *proposals,
					       struct logger *logger);

/*
 * Is the modp group in the proposal set?
 *
 * It's the caller's problem to check that it is actually supported.
 */
bool ikev2_proposals_include_modp(const struct ikev2_proposals *proposals,
				  oakley_group_t modp);

void ikev2_copy_cookie_from_sa(const struct ikev2_proposal *accepted_ike_proposal,
			       ike_spi_t *cookie);

#endif
