/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2013, 2016 by Delphix. All rights reserved.
 * Copyright 2013 Saso Kiselkov. All rights reserved.
 */

#include <sys/zfs_context.h>
#include <sys/spa.h>
#include <sys/spa_impl.h>
#include <sys/zio.h>
#include <sys/zio_checksum.h>
#include <sys/zil.h>
#include <sys/abd.h>
#include <zfs_fletcher.h>


//JW
#include "/home/kau/zfs_ctxt_wait/include/hr_calclock.h"

/*
 * Checksum vectors.
 *
 * In the SPA, everything is checksummed.  We support checksum vectors
 * for three distinct reasons:
 *
 *   1. Different kinds of data need different levels of protection.
 *	For SPA metadata, we always want a very strong checksum.
 *	For user data, we let users make the trade-off between speed
 *	and checksum strength.
 *
 *   2. Cryptographic hash and MAC algorithms are an area of active research.
 *	It is likely that in future hash functions will be at least as strong
 *	as current best-of-breed, and may be substantially faster as well.
 *	We want the ability to take advantage of these new hashes as soon as
 *	they become available.
 *
 *   3. If someone develops hardware that can compute a strong hash quickly,
 *	we want the ability to take advantage of that hardware.
 *
 * Of course, we don't want a checksum upgrade to invalidate existing
 * data, so we store the checksum *function* in eight bits of the bp.
 * This gives us room for up to 256 different checksum functions.
 *
 * When writing a block, we always checksum it with the latest-and-greatest
 * checksum function of the appropriate strength.  When reading a block,
 * we compare the expected checksum against the actual checksum, which we
 * compute via the checksum function specified by BP_GET_CHECKSUM(bp).
 *
 * SALTED CHECKSUMS
 *
 * To enable the use of less secure hash algorithms with dedup, we
 * introduce the notion of salted checksums (MACs, really).  A salted
 * checksum is fed both a random 256-bit value (the salt) and the data
 * to be checksummed.  This salt is kept secret (stored on the pool, but
 * never shown to the user).  Thus even if an attacker knew of collision
 * weaknesses in the hash algorithm, they won't be able to mount a known
 * plaintext attack on the DDT, since the actual hash value cannot be
 * known ahead of time.  How the salt is used is algorithm-specific
 * (some might simply prefix it to the data block, others might need to
 * utilize a full-blown HMAC).  On disk the salt is stored in a ZAP
 * object in the MOS (DMU_POOL_CHECKSUM_SALT).
 *
 * CONTEXT TEMPLATES
 *
 * Some hashing algorithms need to perform a substantial amount of
 * initialization work (e.g. salted checksums above may need to pre-hash
 * the salt) before being able to process data.  Performing this
 * redundant work for each block would be wasteful, so we instead allow
 * a checksum algorithm to do the work once (the first time it's used)
 * and then keep this pre-initialized context as a template inside the
 * spa_t (spa_cksum_tmpls).  If the zio_checksum_info_t contains
 * non-NULL ci_tmpl_init and ci_tmpl_free callbacks, they are used to
 * construct and destruct the pre-initialized checksum context.  The
 * pre-initialized context is then reused during each checksum
 * invocation and passed to the checksum function.
 */

/*ARGSUSED*/
static void
abd_checksum_off(abd_t *abd, uint64_t size,
    const void *ctx_template, zio_cksum_t *zcp)
{
	ZIO_SET_CHECKSUM(zcp, 0, 0, 0, 0);
}

/*ARGSUSED*/
void
abd_fletcher_2_native(abd_t *abd, uint64_t size,
    const void *ctx_template, zio_cksum_t *zcp)
{
	fletcher_init(zcp);
	(void) abd_iterate_func(abd, 0, size,
	    fletcher_2_incremental_native, zcp);
}

/*ARGSUSED*/
void
abd_fletcher_2_byteswap(abd_t *abd, uint64_t size,
    const void *ctx_template, zio_cksum_t *zcp)
{
	fletcher_init(zcp);
	(void) abd_iterate_func(abd, 0, size,
	    fletcher_2_incremental_byteswap, zcp);
}

unsigned long long abd_c_t=0, abd_c_c=0;
static inline void
abd_fletcher_4_impl(abd_t *abd, uint64_t size, zio_abd_checksum_data_t *acdp)
{
	fletcher_4_abd_ops.acf_init(acdp);
hrtime_t abd_c_local[2];
abd_c_local[0] = gethrtime();
//JW0124: removing checksum function
	abd_iterate_func(abd, 0, size, fletcher_4_abd_ops.acf_iter, acdp);
abd_c_local[1] = gethrtime();
calclock(abd_c_local, &abd_c_t, &abd_c_c);
	fletcher_4_abd_ops.acf_fini(acdp);
}

/*ARGSUSED*/
void
abd_fletcher_4_native(abd_t *abd, uint64_t size,
    const void *ctx_template, zio_cksum_t *zcp)
{
	fletcher_4_ctx_t ctx;

	zio_abd_checksum_data_t acd = {
		.acd_byteorder	= ZIO_CHECKSUM_NATIVE,
		.acd_zcp 	= zcp,
		.acd_ctx	= &ctx
	};

	abd_fletcher_4_impl(abd, size, &acd);

}

/*ARGSUSED*/
void
abd_fletcher_4_byteswap(abd_t *abd, uint64_t size,
    const void *ctx_template, zio_cksum_t *zcp)
{
	fletcher_4_ctx_t ctx;

	zio_abd_checksum_data_t acd = {
		.acd_byteorder	= ZIO_CHECKSUM_BYTESWAP,
		.acd_zcp 	= zcp,
		.acd_ctx	= &ctx
	};

	abd_fletcher_4_impl(abd, size, &acd);
}

zio_checksum_info_t zio_checksum_table[ZIO_CHECKSUM_FUNCTIONS] = {
	{{NULL, NULL}, NULL, NULL, 0, "inherit"},
	{{NULL, NULL}, NULL, NULL, 0, "on"},
	{{abd_checksum_off,		abd_checksum_off},
	    NULL, NULL, 0, "off"},
	{{abd_checksum_SHA256,		abd_checksum_SHA256},
	    NULL, NULL, ZCHECKSUM_FLAG_METADATA | ZCHECKSUM_FLAG_EMBEDDED,
	    "label"},
	{{abd_checksum_SHA256,		abd_checksum_SHA256},
	    NULL, NULL, ZCHECKSUM_FLAG_METADATA | ZCHECKSUM_FLAG_EMBEDDED,
	    "gang_header"},
	{{abd_fletcher_2_native,	abd_fletcher_2_byteswap},
	    NULL, NULL, ZCHECKSUM_FLAG_EMBEDDED, "zilog"},
	{{abd_fletcher_2_native,	abd_fletcher_2_byteswap},
	    NULL, NULL, 0, "fletcher2"},
	{{abd_fletcher_4_native,	abd_fletcher_4_byteswap},
	    NULL, NULL, ZCHECKSUM_FLAG_METADATA, "fletcher4"},
	{{abd_checksum_SHA256,		abd_checksum_SHA256},
	    NULL, NULL, ZCHECKSUM_FLAG_METADATA | ZCHECKSUM_FLAG_DEDUP |
	    ZCHECKSUM_FLAG_NOPWRITE, "sha256"},
	{{abd_fletcher_4_native,	abd_fletcher_4_byteswap},
	    NULL, NULL, ZCHECKSUM_FLAG_EMBEDDED, "zilog2"},
	{{abd_checksum_off,		abd_checksum_off},
	    NULL, NULL, 0, "noparity"},
	{{abd_checksum_SHA512_native,	abd_checksum_SHA512_byteswap},
	    NULL, NULL, ZCHECKSUM_FLAG_METADATA | ZCHECKSUM_FLAG_DEDUP |
	    ZCHECKSUM_FLAG_NOPWRITE, "sha512"},
	{{abd_checksum_skein_native,	abd_checksum_skein_byteswap},
	    abd_checksum_skein_tmpl_init, abd_checksum_skein_tmpl_free,
	    ZCHECKSUM_FLAG_METADATA | ZCHECKSUM_FLAG_DEDUP |
	    ZCHECKSUM_FLAG_SALTED | ZCHECKSUM_FLAG_NOPWRITE, "skein"},
	{{abd_checksum_edonr_native,	abd_checksum_edonr_byteswap},
	    abd_checksum_edonr_tmpl_init, abd_checksum_edonr_tmpl_free,
	    ZCHECKSUM_FLAG_METADATA | ZCHECKSUM_FLAG_SALTED |
	    ZCHECKSUM_FLAG_NOPWRITE, "edonr"},
};

/*
 * The flag corresponding to the "verify" in dedup=[checksum,]verify
 * must be cleared first, so callers should use ZIO_CHECKSUM_MASK.
 */
spa_feature_t
zio_checksum_to_feature(enum zio_checksum cksum)
{
	VERIFY((cksum & ~ZIO_CHECKSUM_MASK) == 0);

	switch (cksum) {
	case ZIO_CHECKSUM_SHA512:
		return (SPA_FEATURE_SHA512);
	case ZIO_CHECKSUM_SKEIN:
		return (SPA_FEATURE_SKEIN);
	case ZIO_CHECKSUM_EDONR:
		return (SPA_FEATURE_EDONR);
	default:
		return (SPA_FEATURE_NONE);
	}
}

enum zio_checksum
zio_checksum_select(enum zio_checksum child, enum zio_checksum parent)
{
	ASSERT(child < ZIO_CHECKSUM_FUNCTIONS);
	ASSERT(parent < ZIO_CHECKSUM_FUNCTIONS);
	ASSERT(parent != ZIO_CHECKSUM_INHERIT && parent != ZIO_CHECKSUM_ON);

	if (child == ZIO_CHECKSUM_INHERIT)
		return (parent);

	if (child == ZIO_CHECKSUM_ON)
		return (ZIO_CHECKSUM_ON_VALUE);

	return (child);
}

enum zio_checksum
zio_checksum_dedup_select(spa_t *spa, enum zio_checksum child,
    enum zio_checksum parent)
{
	ASSERT((child & ZIO_CHECKSUM_MASK) < ZIO_CHECKSUM_FUNCTIONS);
	ASSERT((parent & ZIO_CHECKSUM_MASK) < ZIO_CHECKSUM_FUNCTIONS);
	ASSERT(parent != ZIO_CHECKSUM_INHERIT && parent != ZIO_CHECKSUM_ON);

	if (child == ZIO_CHECKSUM_INHERIT)
		return (parent);

	if (child == ZIO_CHECKSUM_ON)
		return (spa_dedup_checksum(spa));

	if (child == (ZIO_CHECKSUM_ON | ZIO_CHECKSUM_VERIFY))
		return (spa_dedup_checksum(spa) | ZIO_CHECKSUM_VERIFY);

	ASSERT((zio_checksum_table[child & ZIO_CHECKSUM_MASK].ci_flags &
	    ZCHECKSUM_FLAG_DEDUP) ||
	    (child & ZIO_CHECKSUM_VERIFY) || child == ZIO_CHECKSUM_OFF);

	return (child);
}

/*
 * Set the external verifier for a gang block based on <vdev, offset, txg>,
 * a tuple which is guaranteed to be unique for the life of the pool.
 */
static void
zio_checksum_gang_verifier(zio_cksum_t *zcp, const blkptr_t *bp)
{
	const dva_t *dva = BP_IDENTITY(bp);
	uint64_t txg = BP_PHYSICAL_BIRTH(bp);

	ASSERT(BP_IS_GANG(bp));

	ZIO_SET_CHECKSUM(zcp, DVA_GET_VDEV(dva), DVA_GET_OFFSET(dva), txg, 0);
}

/*
 * Set the external verifier for a label block based on its offset.
 * The vdev is implicit, and the txg is unknowable at pool open time --
 * hence the logic in vdev_uberblock_load() to find the most recent copy.
 */
static void
zio_checksum_label_verifier(zio_cksum_t *zcp, uint64_t offset)
{
	ZIO_SET_CHECKSUM(zcp, offset, 0, 0, 0);
}

/*
 * Calls the template init function of a checksum which supports context
 * templates and installs the template into the spa_t.
 */
static void
zio_checksum_template_init(enum zio_checksum checksum, spa_t *spa)
{
	zio_checksum_info_t *ci = &zio_checksum_table[checksum];

	if (ci->ci_tmpl_init == NULL)
		return;
	if (spa->spa_cksum_tmpls[checksum] != NULL)
		return;

	VERIFY(ci->ci_tmpl_free != NULL);
	mutex_enter(&spa->spa_cksum_tmpls_lock);
	if (spa->spa_cksum_tmpls[checksum] == NULL) {
		spa->spa_cksum_tmpls[checksum] =
		    ci->ci_tmpl_init(&spa->spa_cksum_salt);
		VERIFY(spa->spa_cksum_tmpls[checksum] != NULL);
	}
	mutex_exit(&spa->spa_cksum_tmpls_lock);
}

/*
 * Generate the checksum.
 */
unsigned long long eea_t=0, eea_c=0;
unsigned long long eeb_t=0, eeb_c=0;
unsigned long long eec_t=0, eec_c=0;
unsigned long long eed_t=0, eed_c=0;
unsigned long long eee_t=0, eee_c=0;
unsigned long long eef_t=0, eef_c=0;

void
zio_checksum_compute(zio_t *zio, enum zio_checksum checksum,
    abd_t *abd, uint64_t size)
{
hrtime_t eea_local[2];
eea_local[0] = gethrtime();
	static const uint64_t zec_magic = ZEC_MAGIC;
	blkptr_t *bp = zio->io_bp;
	uint64_t offset = zio->io_offset;
	zio_checksum_info_t *ci = &zio_checksum_table[checksum];
	zio_cksum_t cksum;
	spa_t *spa = zio->io_spa;

//JW0628
	//zio_cksum_t	blk_cksum_tmp;


	ASSERT((uint_t)checksum < ZIO_CHECKSUM_FUNCTIONS);
	ASSERT(ci->ci_func[0] != NULL);

	zio_checksum_template_init(checksum, spa);

eea_local[1] = gethrtime();
calclock(eea_local, &eea_t, &eea_c);

hrtime_t eeb_local[2];
eeb_local[0] = gethrtime();
//dprintf("[%u][aaa]\n", zio->id);

	if (ci->ci_flags & ZCHECKSUM_FLAG_EMBEDDED) {
		zio_eck_t eck;
		size_t eck_offset;

hrtime_t eec_local[2];
eec_local[0] = gethrtime();
		if (checksum == ZIO_CHECKSUM_ZILOG2) {
			//dprintf("[aa_1]\n");
			zil_chain_t zilc;
			abd_copy_to_buf(&zilc, abd, sizeof (zil_chain_t));
			size = P2ROUNDUP_TYPED(zilc.zc_nused, ZIL_MIN_BLKSZ,
			    uint64_t);
			eck = zilc.zc_eck;
			eck_offset = offsetof(zil_chain_t, zc_eck);
		} else {
			//dprintf("[aa_2]\n");
			eck_offset = size - sizeof (zio_eck_t);
			abd_copy_to_buf_off(&eck, abd, eck_offset,
			    sizeof (zio_eck_t));
		}
eec_local[1] = gethrtime();
calclock(eec_local, &eec_t, &eec_c);


hrtime_t eed_local[2];
eed_local[0] = gethrtime();
//dprintf("[%u][bbb]\n", zio->id);
		if (checksum == ZIO_CHECKSUM_GANG_HEADER) {
			zio_checksum_gang_verifier(&eck.zec_cksum, bp);
			abd_copy_from_buf_off(abd, &eck.zec_cksum,
			    eck_offset + offsetof(zio_eck_t, zec_cksum),
			    sizeof (zio_cksum_t));
		} else if (checksum == ZIO_CHECKSUM_LABEL) {
			zio_checksum_label_verifier(&eck.zec_cksum, offset);
			abd_copy_from_buf_off(abd, &eck.zec_cksum,
			    eck_offset + offsetof(zio_eck_t, zec_cksum),
			    sizeof (zio_cksum_t));
		} else {
			//JW0628
			bp->blk_cksum = eck.zec_cksum;

			//blk_cksum_tmp = eck.zec_cksum;
		}
eed_local[1] = gethrtime();
calclock(eed_local, &eed_t, &eed_c);


hrtime_t eee_local[2];
eee_local[0] = gethrtime();
//dprintf("[%u][ccc]\n", zio->id);
		abd_copy_from_buf_off(abd, &zec_magic,
		    eck_offset + offsetof(zio_eck_t, zec_magic),
		    sizeof (zec_magic));
//JW: calling abd_fletcher_4_native function
		ci->ci_func[0](abd, size, spa->spa_cksum_tmpls[checksum],
		    &cksum);

		abd_copy_from_buf_off(abd, &cksum,
		    eck_offset + offsetof(zio_eck_t, zec_cksum),
		    sizeof (zio_cksum_t));
eee_local[1] = gethrtime();
calclock(eee_local, &eee_t, &eee_c);


	} else {
hrtime_t eef_local[2];
eef_local[0] = gethrtime();
//dprintf("[%u][ddd]\n", zio->id);
//JW: calling abd_fletcher_4_native function
		//JW0628	
		ci->ci_func[0](abd, size, spa->spa_cksum_tmpls[checksum],
		    &bp->blk_cksum);

		//ci->ci_func[0](abd, size, spa->spa_cksum_tmpls[checksum],
		//    &blk_cksum_tmp);
	
eef_local[1] = gethrtime();
calclock(eef_local, &eef_t, &eef_c);


	}
eeb_local[1] = gethrtime();
calclock(eeb_local, &eeb_t, &eeb_c);
//dprintf("[%u][eee]\n", zio->id);
}

int
zio_checksum_error_impl(spa_t *spa, const blkptr_t *bp,
    enum zio_checksum checksum, abd_t *abd, uint64_t size, uint64_t offset,
    zio_bad_cksum_t *info)
{
	zio_checksum_info_t *ci = &zio_checksum_table[checksum];
	zio_cksum_t actual_cksum, expected_cksum;
	zio_eck_t eck;
	int byteswap;

	if (checksum >= ZIO_CHECKSUM_FUNCTIONS || ci->ci_func[0] == NULL){
		return (SET_ERROR(EINVAL));
	}
	zio_checksum_template_init(checksum, spa);

	if (ci->ci_flags & ZCHECKSUM_FLAG_EMBEDDED) {
		zio_cksum_t verifier;
		size_t eck_offset;

		if (checksum == ZIO_CHECKSUM_ZILOG2) {
			zil_chain_t zilc;
			uint64_t nused;

			abd_copy_to_buf(&zilc, abd, sizeof (zil_chain_t));

			eck = zilc.zc_eck;
			eck_offset = offsetof(zil_chain_t, zc_eck) +
			    offsetof(zio_eck_t, zec_cksum);

			if (eck.zec_magic == ZEC_MAGIC) {
				nused = zilc.zc_nused;
			} else if (eck.zec_magic == BSWAP_64(ZEC_MAGIC)) {
				nused = BSWAP_64(zilc.zc_nused);
			} else {
				return (SET_ERROR(ECKSUM));
			}

			if (nused > size) {
				return (SET_ERROR(ECKSUM));
			}

			size = P2ROUNDUP_TYPED(nused, ZIL_MIN_BLKSZ, uint64_t);
		} else {
			eck_offset = size - sizeof (zio_eck_t);
			abd_copy_to_buf_off(&eck, abd, eck_offset,
			    sizeof (zio_eck_t));
			eck_offset += offsetof(zio_eck_t, zec_cksum);
		}

		if (checksum == ZIO_CHECKSUM_GANG_HEADER)
			zio_checksum_gang_verifier(&verifier, bp);
		else if (checksum == ZIO_CHECKSUM_LABEL)
			zio_checksum_label_verifier(&verifier, offset);
		else
			verifier = bp->blk_cksum;

		byteswap = (eck.zec_magic == BSWAP_64(ZEC_MAGIC));

		if (byteswap)
			byteswap_uint64_array(&verifier, sizeof (zio_cksum_t));
	
		expected_cksum = eck.zec_cksum;

		abd_copy_from_buf_off(abd, &verifier, eck_offset,
		    sizeof (zio_cksum_t));

		ci->ci_func[byteswap](abd, size,
		    spa->spa_cksum_tmpls[checksum], &actual_cksum);

		abd_copy_from_buf_off(abd, &expected_cksum, eck_offset,
		    sizeof (zio_cksum_t));

		if (byteswap) {
			byteswap_uint64_array(&expected_cksum,
			    sizeof (zio_cksum_t));
		}
	} else {
		byteswap = BP_SHOULD_BYTESWAP(bp);
//#ifdef _KERNEL
//		printk(KERN_WARNING "[bb] %d %d", byteswap, SET_ERROR(ECKSUM));
//#endif
	
		expected_cksum = bp->blk_cksum;
		ci->ci_func[byteswap](abd, size,
		    spa->spa_cksum_tmpls[checksum], &actual_cksum);
	}

	if (info != NULL) {
		info->zbc_expected = expected_cksum;
		info->zbc_actual = actual_cksum;
		info->zbc_checksum_name = ci->ci_name;
		info->zbc_byteswapped = byteswap;
		info->zbc_injected = 0;
		info->zbc_has_cksum = 1;
	}

	if (!ZIO_CHECKSUM_EQUAL(actual_cksum, expected_cksum)){
		return (SET_ERROR(ECKSUM));
	}
	return (0);
}

int
zio_checksum_error(zio_t *zio, zio_bad_cksum_t *info)
{
	blkptr_t *bp = zio->io_bp;
	uint_t checksum = (bp == NULL ? zio->io_prop.zp_checksum :
	    (BP_IS_GANG(bp) ? ZIO_CHECKSUM_GANG_HEADER : BP_GET_CHECKSUM(bp)));
	int error;
	uint64_t size = (bp == NULL ? zio->io_size :
	    (BP_IS_GANG(bp) ? SPA_GANGBLOCKSIZE : BP_GET_PSIZE(bp)));
	uint64_t offset = zio->io_offset;
	abd_t *data = zio->io_abd;
	spa_t *spa = zio->io_spa;
	
	error = zio_checksum_error_impl(spa, bp, checksum, data, size,
	    offset, info);

	if (zio_injection_enabled && error == 0 && zio->io_error == 0) {
		error = zio_handle_fault_injection(zio, ECKSUM);
		if (error != 0)
			info->zbc_injected = 1;
	}

	return (error);
}

/*
 * Called by a spa_t that's about to be deallocated. This steps through
 * all of the checksum context templates and deallocates any that were
 * initialized using the algorithm-specific template init function.
 */
void
zio_checksum_templates_free(spa_t *spa)
{
	enum zio_checksum checksum;
	for (checksum = 0; checksum < ZIO_CHECKSUM_FUNCTIONS;
	    checksum++) {
		if (spa->spa_cksum_tmpls[checksum] != NULL) {
			zio_checksum_info_t *ci = &zio_checksum_table[checksum];

			VERIFY(ci->ci_tmpl_free != NULL);
			ci->ci_tmpl_free(spa->spa_cksum_tmpls[checksum]);
			spa->spa_cksum_tmpls[checksum] = NULL;
		}
	}
}
