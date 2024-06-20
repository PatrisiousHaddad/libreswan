/* NSS GCM for libreswan
 *
 * Copyright (C) 2014,2016,2018 Andrew Cagney
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

#include <stdio.h>
#include <stdlib.h>

/*
 * Special advise from Bob Relyea - needs to go before any nss include
 *
 */
#define NSS_PKCS11_2_0_COMPAT 1

#include "lswlog.h"
#include "lswnss.h"
#include "prmem.h"
#include "prerror.h"

#include "constants.h"
#include "ike_alg.h"
#include "ike_alg_encrypt_ops.h"
#include "rnd.h"

static bool ike_alg_nss_gcm(const struct encrypt_desc *alg,
			    shunk_t salt,
			    enum ike_alg_iv_source iv_source,
			    chunk_t wire_iv,
			    shunk_t aad,
			    chunk_t text_and_tag,
			    size_t text_len, size_t tag_len,
			    PK11SymKey *sym_key,
			    enum ike_alg_crypt crypt,
			    struct logger *logger)
{
	/* must be contigious */
	PASSERT(logger, text_len + tag_len == text_and_tag.len);

	/* See pk11gcmtest.c */
	bool ok = true;

	switch (iv_source) {
	case USE_IV:
		break;
	case FILL_IV:
		fill_rnd_chunk(wire_iv);
		break;
	default:
		bad_case(iv_source);
	}

	chunk_t iv = clone_hunk_hunk(salt, wire_iv, "IV");

	CK_GCM_PARAMS gcm_params;
	gcm_params.pIv = iv.ptr;
	gcm_params.ulIvLen = iv.len;
	gcm_params.pAAD = (void*)aad.ptr;
	gcm_params.ulAADLen = aad.len;
	gcm_params.ulTagBits = tag_len * 8;

	SECItem param;
	param.type = siBuffer;
	param.data = (void*)&gcm_params;
	param.len = sizeof gcm_params;

	/* Output buffer for transformed data. */
	uint8_t *out_buf = PR_Malloc(text_and_tag.len);
	unsigned int out_len = 0;

	switch (crypt) {
	case ENCRYPT:
	{
		SECStatus rv = PK11_Encrypt(sym_key, alg->nss.mechanism,
					    &param, out_buf, &out_len,
					    text_and_tag.len,
					    text_and_tag.ptr, text_len);
		if (rv != SECSuccess) {
			llog_nss_error(RC_LOG, logger,
				       "AEAD encryption using %s_%u and PK11_Encrypt() failed",
				       alg->common.fqn,
				       PK11_GetKeyLength(sym_key) * BITS_IN_BYTE);
			ok = false;
		} else if (out_len != text_and_tag.len) {
			/* should this be a pexpect fail? */
			llog_nss_error(RC_LOG, logger,
				       "AEAD encryption using %s_%u and PK11_Encrypt() failed (output length of %u not the expected %zd)",
				       alg->common.fqn,
				       PK11_GetKeyLength(sym_key) * BITS_IN_BYTE,
				       out_len, text_and_tag.len);
			ok = false;
		}
		break;
	}
	case DECRYPT:
	{
		SECStatus rv = PK11_Decrypt(sym_key, alg->nss.mechanism, &param,
					    out_buf, &out_len, text_and_tag.len,
					    text_and_tag.ptr, text_and_tag.len);
		if (rv != SECSuccess) {
			llog_nss_error(RC_LOG, logger,
				       "AEAD decryption using %s_%u and PK11_Decrypt() failed",
				       alg->common.fqn,
				       PK11_GetKeyLength(sym_key) * BITS_IN_BYTE);
			ok = false;
		} else if (out_len != text_len) {
			/* should this be a pexpect fail? */
			llog_nss_error(RC_LOG, logger,
				       "AEAD decryption using %s_%u and PK11_Decrypt() failed (output length of %u not the expected %zd)",
				       alg->common.fqn,
				       PK11_GetKeyLength(sym_key) * BITS_IN_BYTE,
				       out_len, text_len);
			ok = false;
		}
		break;
	}
	default:
		bad_case(crypt);
	}

	memcpy(text_and_tag.ptr, out_buf, out_len);
	PR_Free(out_buf);
	free_chunk_content(&iv);

	return ok;
}

static void nss_gcm_check(const struct encrypt_desc *encrypt, struct logger *logger)
{
	const struct ike_alg *alg = &encrypt->common;
	pexpect_ike_alg(logger, alg, encrypt->nss.mechanism > 0);
}

const struct encrypt_ops ike_alg_encrypt_nss_gcm_ops = {
	.backend = "NSS(GCM)",
	.check = nss_gcm_check,
	.do_aead = ike_alg_nss_gcm,
};
