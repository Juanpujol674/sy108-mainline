/* SPDX-License-Identifier: GPL-2.0-only */
#include "halo_rsa_provider.h"
#include <errno.h>
#include <string.h>
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/param_build.h>
#include <openssl/rsa.h>

static int monotonic(void *ctx, uint64_t *out)
{
	struct halo_rsa_provider *p = ctx;
	return p->callbacks.monotonic_ms(p->callbacks.ctx, out);
}
static int clock_word(void *ctx, uint32_t *out)
{
	struct halo_rsa_provider *p = ctx;
	return p->callbacks.clock_word(p->callbacks.ctx, out);
}
static int entropy(void *ctx, uint64_t generation, uint64_t deadline, uint8_t out[16])
{
	struct halo_rsa_provider *p = ctx;
	return p->callbacks.entropy16(p->callbacks.ctx, generation, deadline, out);
}
static void release(void *ctx, uint64_t generation)
{
	struct halo_rsa_provider *p = ctx;
	p->callbacks.release(p->callbacks.ctx, generation);
}
static int private_operation(void *ctx, uint64_t generation, uint64_t deadline,
			     const uint8_t block[128], uint8_t signature[128])
{
	struct halo_rsa_provider *p = ctx;
	EVP_PKEY_CTX *sign = NULL, *verify = NULL;
	uint8_t recovered[128] = {0};
	size_t length = 128, check_length = 128;
	uint64_t before, after;
	int error = -EKEYREJECTED;
	if (!p || !p->key || !generation || !block || !signature)
		return -EINVAL;
	memset(signature, 0, 128);
	if (monotonic(p, &before))
		return -EIO;
	if (before >= deadline)
		return -ETIMEDOUT;
	sign = EVP_PKEY_CTX_new_from_pkey(NULL, p->key, NULL);
	verify = EVP_PKEY_CTX_new_from_pkey(NULL, p->key, NULL);
	if (!sign || !verify) {
		error = -ENOMEM;
		goto done;
	}
	/* halo_material_block already supplies the exact target type-1 encoding.
	 * Applying PKCS1 padding here would encode the block a second time. */
	if (EVP_PKEY_sign_init(sign) <= 0 ||
	    EVP_PKEY_CTX_set_rsa_padding(sign, RSA_NO_PADDING) <= 0 ||
	    EVP_PKEY_sign(sign, signature, &length, block, 128) <= 0 || length != 128)
		goto done;
	/* Check the supplied exponent against n/e for this operation and catch an
	 * incorrect parameter pair without returning a plausible-looking signature. */
	if (EVP_PKEY_verify_recover_init(verify) <= 0 ||
	    EVP_PKEY_CTX_set_rsa_padding(verify, RSA_NO_PADDING) <= 0 ||
	    EVP_PKEY_verify_recover(verify, recovered, &check_length, signature, 128) <= 0 ||
	    check_length != 128 || CRYPTO_memcmp(block, recovered, 128))
		goto done;
	error = 0;
done:
	EVP_PKEY_CTX_free(sign);
	EVP_PKEY_CTX_free(verify);
	OPENSSL_cleanse(recovered, sizeof recovered);
	if (monotonic(p, &after))
		error = -EIO;
	else if (after < before)
		error = -EIO;
	else if (after >= deadline)
		error = -ETIMEDOUT;
	if (error)
		OPENSSL_cleanse(signature, 128);
	return error;
}
void halo_rsa_provider_clear(struct halo_rsa_provider *p)
{
	if (!p)
		return;
	EVP_PKEY_free(p->key);
	OPENSSL_cleanse(p, sizeof(*p));
}
int halo_rsa_provider_init(struct halo_rsa_provider *p,
			   const struct halo_material_provider *callbacks,
			   const uint8_t nbytes[128], const uint8_t dbytes[128],
			   struct halo_credential_provider *out)
{
	BIGNUM *n = NULL, *d = NULL, *e = NULL;
	OSSL_PARAM_BLD *builder = NULL;
	OSSL_PARAM *params = NULL;
	EVP_PKEY_CTX *ctx = NULL;
	int error = -EINVAL;
	if (!out)
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	if (!p || p->key || !callbacks || !nbytes || !dbytes || !callbacks->trusted_modulus ||
	    !callbacks->monotonic_ms || !callbacks->clock_word || !callbacks->entropy16 ||
	    !callbacks->release)
		return -EINVAL;
	/* The captured stock modulus is 1023 bits in the 128-byte ABI. */
	if (!nbytes[0] || !(nbytes[127] & 1))
		return -EINVAL;
	n = BN_bin2bn(nbytes, 128, NULL);
	d = BN_secure_new();
	e = BN_new();
	if (!n || !d || !e) {
		error = -ENOMEM;
		goto done;
	}
	if (!BN_bin2bn(dbytes, 128, d) || !BN_set_word(e, 65537)) {
		error = -ENOMEM;
		goto done;
	}
	if (BN_is_zero(d) || BN_is_one(d) || BN_cmp(d, n) >= 0)
		goto done;
	builder = OSSL_PARAM_BLD_new();
	ctx = EVP_PKEY_CTX_new_from_name(NULL, "RSA", NULL);
	if (!builder || !ctx) {
		error = -ENOMEM;
		goto done;
	}
	if (!OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_RSA_N, n) ||
	    !OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_RSA_E, e) ||
	    !OSSL_PARAM_BLD_push_BN(builder, OSSL_PKEY_PARAM_RSA_D, d))
		goto done;
	params = OSSL_PARAM_BLD_to_param(builder);
	if (!params) {
		error = -ENOMEM;
		goto done;
	}
	if (EVP_PKEY_fromdata_init(ctx) <= 0 ||
	    EVP_PKEY_fromdata(ctx, &p->key, EVP_PKEY_KEYPAIR, params) <= 0)
		goto done;
	p->callbacks = *callbacks;
	memcpy(p->peer_modulus, callbacks->trusted_modulus, 128);
	p->material = (struct halo_material_provider){
	    p, monotonic, entropy, clock_word, private_operation, release, p->peer_modulus};
	error = halo_material_adapter(&p->material, out);
done:
	if (params)
		for (OSSL_PARAM *v = params; v->key; v++)
			if (!strcmp(v->key, OSSL_PKEY_PARAM_RSA_D))
				OPENSSL_cleanse(v->data, v->data_size);
	OSSL_PARAM_free(params);
	OSSL_PARAM_BLD_free(builder);
	EVP_PKEY_CTX_free(ctx);
	BN_free(n);
	BN_clear_free(d);
	BN_free(e);
	if (error)
		halo_rsa_provider_clear(p);
	return error;
}
