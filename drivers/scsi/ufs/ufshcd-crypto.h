/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright 2019 Google LLC
 */

#ifndef _UFSHCD_CRYPTO_H
#define _UFSHCD_CRYPTO_H

#ifdef CONFIG_SCSI_UFS_CRYPTO
#include "ufshcd.h"
#include "ufshci.h"

#if IS_ENABLED(CONFIG_QTI_CRYPTO_FDE)
void ufshcd_prepare_lrbp_crypto(struct ufs_hba *hba, struct request *rq,
					      struct ufshcd_lrb *lrbp);

#else
static inline void ufshcd_prepare_lrbp_crypto(struct request *rq,
					      struct ufshcd_lrb *lrbp)
{
	if (!rq || !rq->crypt_keyslot) {
		lrbp->crypto_key_slot = -1;
		return;
	}

	lrbp->crypto_key_slot = blk_ksm_get_slot_idx(rq->crypt_keyslot);
	lrbp->data_unit_num = rq->crypt_ctx->bc_dun[0];
}
#endif

static inline void
ufshcd_prepare_req_desc_hdr_crypto(struct ufshcd_lrb *lrbp, u32 *dword_0,
				   u32 *dword_1, u32 *dword_3)
{
	if (lrbp->crypto_key_slot >= 0) {
#if IS_ENABLED(CONFIG_QTI_CRYPTO_FDE)
		if (!lrbp->crypto_enable)
			return;
#endif
		*dword_0 |= UTP_REQ_DESC_CRYPTO_ENABLE_CMD;
		*dword_0 |= lrbp->crypto_key_slot;
		*dword_1 = lower_32_bits(lrbp->data_unit_num);
		*dword_3 = upper_32_bits(lrbp->data_unit_num);
	}
}

static inline void ufshcd_crypto_clear_prdt(struct ufs_hba *hba,
					    struct ufshcd_lrb *lrbp)
{
	if (!(hba->quirks & UFSHCD_QUIRK_KEYS_IN_PRDT))
		return;

	if (!(scsi_cmd_to_rq(lrbp->cmd)->crypt_ctx))
		return;

	memzero_explicit(lrbp->ucd_prdt_ptr,
			 hba->sg_entry_size * scsi_sg_count(lrbp->cmd));
}

bool ufshcd_crypto_enable(struct ufs_hba *hba);

int ufshcd_hba_init_crypto_capabilities(struct ufs_hba *hba);

void ufshcd_init_crypto(struct ufs_hba *hba);

void ufshcd_crypto_setup_rq_keyslot_manager(struct ufs_hba *hba,
					    struct request_queue *q);

#else /* CONFIG_SCSI_UFS_CRYPTO */

static inline void ufshcd_prepare_lrbp_crypto(struct request *rq,
					      struct ufshcd_lrb *lrbp) { }

static inline void
ufshcd_prepare_req_desc_hdr_crypto(struct ufshcd_lrb *lrbp, u32 *dword_0,
				   u32 *dword_1, u32 *dword_3) { }

static inline void ufshcd_crypto_clear_prdt(struct ufs_hba *hba,
					    struct ufshcd_lrb *lrbp) { }

static inline bool ufshcd_crypto_enable(struct ufs_hba *hba)
{
	return false;
}

static inline int ufshcd_hba_init_crypto_capabilities(struct ufs_hba *hba)
{
	return 0;
}

static inline void ufshcd_init_crypto(struct ufs_hba *hba) { }

static inline void ufshcd_crypto_setup_rq_keyslot_manager(struct ufs_hba *hba,
						struct request_queue *q) { }

#endif /* CONFIG_SCSI_UFS_CRYPTO */

#endif /* _UFSHCD_CRYPTO_H */
