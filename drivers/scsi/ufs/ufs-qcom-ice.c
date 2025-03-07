// SPDX-License-Identifier: GPL-2.0-only
/*
 * Qualcomm ICE (Inline Crypto Engine) support.
 *
 * Copyright (c) 2014-2021, The Linux Foundation. All rights reserved.
 * Copyright 2019 Google LLC
 */

#include <linux/platform_device.h>
#include <linux/qcom_scm.h>
#include <linux/qtee_shmbridge.h>

#include "ufshcd-crypto.h"
#include <linux/crypto-qti-common.h>
#include "ufs-qcom.h"

#define AES_256_XTS_KEY_SIZE			64

/* QCOM ICE registers */

#define QCOM_ICE_REG_CONTROL			0x0000
#define QCOM_ICE_REG_RESET			0x0004
#define QCOM_ICE_REG_VERSION			0x0008
#define QCOM_ICE_REG_FUSE_SETTING		0x0010
#define QCOM_ICE_REG_PARAMETERS_1		0x0014
#define QCOM_ICE_REG_PARAMETERS_2		0x0018
#define QCOM_ICE_REG_PARAMETERS_3		0x001C
#define QCOM_ICE_REG_PARAMETERS_4		0x0020
#define QCOM_ICE_REG_PARAMETERS_5		0x0024

/* QCOM ICE v3.X only */
#define QCOM_ICE_GENERAL_ERR_STTS		0x0040
#define QCOM_ICE_INVALID_CCFG_ERR_STTS		0x0030
#define QCOM_ICE_GENERAL_ERR_MASK		0x0044

/* QCOM ICE v2.X only */
#define QCOM_ICE_REG_NON_SEC_IRQ_STTS		0x0040
#define QCOM_ICE_REG_NON_SEC_IRQ_MASK		0x0044

#define QCOM_ICE_REG_NON_SEC_IRQ_CLR		0x0048
#define QCOM_ICE_REG_STREAM1_ERROR_SYNDROME1	0x0050
#define QCOM_ICE_REG_STREAM1_ERROR_SYNDROME2	0x0054
#define QCOM_ICE_REG_STREAM2_ERROR_SYNDROME1	0x0058
#define QCOM_ICE_REG_STREAM2_ERROR_SYNDROME2	0x005C
#define QCOM_ICE_REG_STREAM1_BIST_ERROR_VEC	0x0060
#define QCOM_ICE_REG_STREAM2_BIST_ERROR_VEC	0x0064
#define QCOM_ICE_REG_STREAM1_BIST_FINISH_VEC	0x0068
#define QCOM_ICE_REG_STREAM2_BIST_FINISH_VEC	0x006C
#define QCOM_ICE_REG_BIST_STATUS		0x0070
#define QCOM_ICE_REG_BYPASS_STATUS		0x0074
#define QCOM_ICE_REG_ADVANCED_CONTROL		0x1000
#define QCOM_ICE_REG_ENDIAN_SWAP		0x1004
#define QCOM_ICE_REG_TEST_BUS_CONTROL		0x1010
#define QCOM_ICE_REG_TEST_BUS_REG		0x1014

/* BIST ("built-in self-test"?) status flags */
#define QCOM_ICE_BIST_STATUS_MASK		0xF0000000

#define QCOM_ICE_FUSE_SETTING_MASK		0x1
#define QCOM_ICE_FORCE_HW_KEY0_SETTING_MASK	0x2
#define QCOM_ICE_FORCE_HW_KEY1_SETTING_MASK	0x4

#define qcom_ice_writel(host, val, reg)	\
	writel((val), (host)->ice_mmio + (reg))
#define qcom_ice_readl(host, reg)	\
	readl((host)->ice_mmio + (reg))

static bool qcom_ice_supported(struct ufs_qcom_host *host)
{
	struct device *dev = host->hba->dev;
	u32 regval = qcom_ice_readl(host, QCOM_ICE_REG_VERSION);
	int major = regval >> 24;
	int minor = (regval >> 16) & 0xFF;
	int step = regval & 0xFFFF;

	/* For now this driver only supports ICE version 3. */
	if (major < 3) {
		dev_warn(dev, "Unsupported ICE version: v%d.%d.%d\n",
			 major, minor, step);
		return false;
	}

	dev_info(dev, "Found QC Inline Crypto Engine (ICE) v%d.%d.%d\n",
		 major, minor, step);

	/* If fuses are blown, ICE might not work in the standard way. */
	regval = qcom_ice_readl(host, QCOM_ICE_REG_FUSE_SETTING);
	if (regval & (QCOM_ICE_FUSE_SETTING_MASK |
		      QCOM_ICE_FORCE_HW_KEY0_SETTING_MASK |
		      QCOM_ICE_FORCE_HW_KEY1_SETTING_MASK)) {
		dev_warn(dev, "Fuses are blown; ICE is unusable!\n");
		return false;
	}
	return true;
}

int ufs_qcom_ice_init(struct ufs_qcom_host *host)
{
	struct ufs_hba *hba = host->hba;
	struct device *dev = hba->dev;
	struct platform_device *pdev = to_platform_device(dev);
	struct resource *ice_base_res;
#if IS_ENABLED(CONFIG_QTI_HW_KEY_MANAGER)
	struct resource *ice_hwkm_res;
#endif
	int err;

	if (!(ufshcd_readl(hba, REG_CONTROLLER_CAPABILITIES) &
	      MASK_CRYPTO_SUPPORT))
		return 0;

	ice_base_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ufs_ice");
	if (!ice_base_res) {
		dev_warn(dev, "ICE registers not found\n");
		goto disable;
	}

	if (!qcom_scm_ice_available()) {
		dev_warn(dev, "ICE SCM interface not found\n");
		goto disable;
	}

	host->ice_mmio = devm_ioremap_resource(dev, ice_base_res);
	if (IS_ERR(host->ice_mmio)) {
		err = PTR_ERR(host->ice_mmio);
		dev_err(dev, "Failed to map ICE registers; err=%d\n", err);
		return err;
	}

#if IS_ENABLED(CONFIG_QTI_HW_KEY_MANAGER)
	ice_hwkm_res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "ufs_ice_hwkm");
	if (!ice_hwkm_res) {
		dev_warn(dev, "ICE HWKM registers not found\n");
		goto disable;
	}
	host->ice_hwkm_mmio = devm_ioremap_resource(dev, ice_hwkm_res);
	if (IS_ERR(host->ice_hwkm_mmio)) {
		err = PTR_ERR(host->ice_hwkm_mmio);
		dev_err(dev, "Failed to map ICE HWKM registers; err=%d\n", err);
		return err;
	}
#endif

	if (!qcom_ice_supported(host))
		goto disable;

	/*
	 * add support for FDE
	 */
#if IS_ENABLED(CONFIG_QTI_CRYPTO_FDE)
	err = crypto_qti_ice_init_fde_node(dev);
	if (err) {
		dev_err(dev, "Failed to add fde node, err=%d\n", err);
		return err;
	}
#endif

	return 0;

disable:
	dev_warn(dev, "Disabling inline encryption support\n");
	hba->caps &= ~UFSHCD_CAP_CRYPTO;
	return 0;
}

static void qcom_ice_low_power_mode_enable(struct ufs_qcom_host *host)
{
	u32 regval;

	regval = qcom_ice_readl(host, QCOM_ICE_REG_ADVANCED_CONTROL);
	/*
	 * Enable low power mode sequence
	 * [0]-0, [1]-0, [2]-0, [3]-E, [4]-0, [5]-0, [6]-0, [7]-0
	 */
	regval |= 0xF000;
	qcom_ice_writel(host, regval, QCOM_ICE_REG_ADVANCED_CONTROL);
}

static void qcom_ice_optimization_enable(struct ufs_qcom_host *host)
{
	u32 regval;

	/* ICE Optimizations Enable Sequence */
	regval = qcom_ice_readl(host, QCOM_ICE_REG_ADVANCED_CONTROL);
	regval |= 0xD80F100;
	/* ICE HPG requires delay before writing */
	udelay(5);
	qcom_ice_writel(host, regval, QCOM_ICE_REG_ADVANCED_CONTROL);
	udelay(5);
}

int ufs_qcom_ice_enable(struct ufs_qcom_host *host)
{
	if (!(host->hba->caps & UFSHCD_CAP_CRYPTO))
		return 0;

	qcom_ice_low_power_mode_enable(host);
	qcom_ice_optimization_enable(host);
	return ufs_qcom_ice_resume(host);
}

void ufs_qcom_ice_disable(struct ufs_qcom_host *host)
{
	if (!(host->hba->caps & UFSHCD_CAP_CRYPTO))
		return;
	if (host->hba->quirks & UFSHCD_QUIRK_CUSTOM_KEYSLOT_MANAGER)
		return crypto_qti_disable();
}

/* Poll until all BIST bits are reset */
static int qcom_ice_wait_bist_status(struct ufs_qcom_host *host)
{
	int count;
	u32 reg;

	for (count = 0; count < 100; count++) {
		reg = qcom_ice_readl(host, QCOM_ICE_REG_BIST_STATUS);
		if (!(reg & QCOM_ICE_BIST_STATUS_MASK))
			break;
		udelay(50);
	}
	if (reg)
		return -ETIMEDOUT;
	return 0;
}

int ufs_qcom_ice_resume(struct ufs_qcom_host *host)
{
	int err;

	if (!(host->hba->caps & UFSHCD_CAP_CRYPTO))
		return 0;

	err = qcom_ice_wait_bist_status(host);
	if (err) {
		dev_err(host->hba->dev, "BIST status error (%d)\n", err);
		return err;
	}
	return 0;
}

/*
 * Program a key into a QC ICE keyslot, or evict a keyslot.  QC ICE requires
 * vendor-specific SCM calls for this; it doesn't support the standard way.
 */
int ufs_qcom_ice_program_key(struct ufs_hba *hba,
			     const union ufs_crypto_cfg_entry *cfg, int slot)
{
	union ufs_crypto_cap_entry cap;
	union {
		u8 bytes[AES_256_XTS_KEY_SIZE];
		u32 words[AES_256_XTS_KEY_SIZE / sizeof(u32)];
	} key;
	int i;
	int err;
	struct qtee_shm shm;

	err = qtee_shmbridge_allocate_shm(AES_256_XTS_KEY_SIZE, &shm);
	if (err)
		return -ENOMEM;

	if (!(cfg->config_enable & UFS_CRYPTO_CONFIGURATION_ENABLE))
		return qcom_scm_ice_invalidate_key(slot);

	/* Only AES-256-XTS has been tested so far. */
	cap = hba->crypto_cap_array[cfg->crypto_cap_idx];
	if (cap.algorithm_id != UFS_CRYPTO_ALG_AES_XTS ||
	    cap.key_size != UFS_CRYPTO_KEY_SIZE_256) {
		dev_err_ratelimited(hba->dev,
				    "Unhandled crypto capability; algorithm_id=%d, key_size=%d\n",
				    cap.algorithm_id, cap.key_size);
		return -EINVAL;
	}

	memcpy(key.bytes, cfg->crypto_key, AES_256_XTS_KEY_SIZE);

	/*
	 * The SCM call byte-swaps the 32-bit words of the key.  So we have to
	 * do the same, in order for the final key be correct.
	 */
	for (i = 0; i < ARRAY_SIZE(key.words); i++)
		__cpu_to_be32s(&key.words[i]);

	memcpy(shm.vaddr, key.bytes, AES_256_XTS_KEY_SIZE);
	qtee_shmbridge_flush_shm_buf(&shm);

	err = qcom_scm_config_set_ice_key(slot, shm.paddr,
					AES_256_XTS_KEY_SIZE,
				   QCOM_SCM_ICE_CIPHER_AES_256_XTS,
					cfg->data_unit_size, UFS_CE);
	if (err)
		pr_err("%s:SCM call Error: 0x%x slot %d\n",
				__func__, err, slot);

	qtee_shmbridge_inv_shm_buf(&shm);
	qtee_shmbridge_free_shm(&shm);
	memzero_explicit(&key, sizeof(key));

	return err;
}

#if IS_ENABLED(CONFIG_QTI_CRYPTO_FDE)
int ufshcd_crypto_qti_prep_lrbp_crypto(struct ufs_hba *hba,
				       struct request *req,
				       struct ufshcd_lrb *lrbp)
{
	//struct bio_crypt_ctx *bc;
	int ret = 0;
	struct ice_data_setting setting;
	bool bypass = true;
	short key_index = 0;

	lrbp->crypto_enable = false;
	if (!req || !req->bio)
		return 0;

	ret = crypto_qti_ice_config_start(req, &setting);
	if (!ret) {
		key_index = setting.crypto_data.key_index;
		bypass = (rq_data_dir(req) == WRITE) ?
			 setting.encr_bypass : setting.decr_bypass;
		lrbp->crypto_enable = !bypass; //need to check this
		lrbp->crypto_key_slot = key_index;
		lrbp->data_unit_num = req->bio->bi_iter.bi_sector >>
				      ICE_CRYPTO_DATA_UNIT_4_KB;
	}

	return ret;
}
#endif	//IS_ENABLED(CONFIG_QTI_CRYPTO_FDE)
