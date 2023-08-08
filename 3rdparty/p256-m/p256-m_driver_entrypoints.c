/*
 *  Driver entry points for p256-m
 */
/*
 *  Copyright The Mbed TLS Contributors
 *  SPDX-License-Identifier: Apache-2.0
 *
 *  Licensed under the Apache License, Version 2.0 (the "License"); you may
 *  not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 *  WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include "mbedtls/platform.h"
#include "p256-m_driver_entrypoints.h"
#include "p256-m/p256-m.h"
#include "psa/crypto.h"
#include "psa_crypto_driver_wrappers.h"
#include <stddef.h>
#include <string.h>

#if defined(MBEDTLS_P256M_EXAMPLE_DRIVER_ENABLED)

/* INFORMATION ON PSA KEY EXPORT FORMATS:
 *
 * PSA exports SECP256R1 keys in two formats:
 * 1. Keypair format: 32 byte string which is just the private key (public key
 *                    can be calculated from the private key)
 * 2. Public Key format: A leading byte 0x04 (indicating uncompressed format),
 *                       followed by the 64 byte public key. This results in a
 *                       total of 65 bytes.
 *
 * p256-m's internal format for private keys matches PSA. Its format for public
 * keys is only 64 bytes; the same as PSA but without the leading byte (0x04).
 * Hence, when passing public keys from PSA to p256-m, the leading byte is
 * removed.
 */

/* Convert between p256-m and PSA error codes */
static psa_status_t p256_to_psa_error(int ret)
{
    switch (ret) {
        case P256_SUCCESS:
            return PSA_SUCCESS;
        case P256_INVALID_PUBKEY:
        case P256_INVALID_PRIVKEY:
            return PSA_ERROR_INVALID_ARGUMENT;
        case P256_INVALID_SIGNATURE:
            return PSA_ERROR_INVALID_SIGNATURE;
        case P256_RANDOM_FAILED:
        default:
            return PSA_ERROR_GENERIC_ERROR;
    }
}

psa_status_t p256_transparent_import_key(const psa_key_attributes_t *attributes,
                             const uint8_t *data,
                             size_t data_length,
                             uint8_t *key_buffer,
                             size_t key_buffer_size,
                             size_t *key_buffer_length,
                             size_t *bits)
{
    /* Check the key size */
    if (*bits != 0 && *bits != 256) {
        return PSA_ERROR_NOT_SUPPORTED;
    }

    /* Validate the key (and its type and size) */
    psa_key_type_t type = psa_get_key_type(attributes);
    if (type == PSA_KEY_TYPE_ECC_PUBLIC_KEY(PSA_ECC_FAMILY_SECP_R1)) {
        if (data_length != 65) {
            return *bits == 0 ? PSA_ERROR_NOT_SUPPORTED : PSA_ERROR_INVALID_ARGUMENT;
        }
        if (p256_validate_pubkey(data + 1) != P256_SUCCESS) {
            return PSA_ERROR_INVALID_ARGUMENT;
        }
    } else if (type == PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1)) {
        if (data_length != 32) {
            return *bits == 0 ? PSA_ERROR_NOT_SUPPORTED : PSA_ERROR_INVALID_ARGUMENT;
        }
        if (p256_validate_privkey(data) != P256_SUCCESS) {
            return PSA_ERROR_INVALID_ARGUMENT;
        }
    } else {
        return PSA_ERROR_NOT_SUPPORTED;
    }
    *bits = 256;

    /* We only support the export format for input, so just copy. */
    if (key_buffer_size < data_length) {
        return PSA_ERROR_BUFFER_TOO_SMALL;
    }
    memcpy(key_buffer, data, data_length);
    *key_buffer_length = data_length;

    return PSA_SUCCESS;
}

psa_status_t p256_transparent_export_public_key(const psa_key_attributes_t *attributes,
                                    const uint8_t *key_buffer,
                                    size_t key_buffer_size,
                                    uint8_t *data,
                                    size_t data_size,
                                    size_t *data_length)
{
    /* Is this the right curve? */
    size_t bits = psa_get_key_bits(attributes);
    psa_key_type_t type = psa_get_key_type(attributes);
    if (bits != 256 || type != PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1)) {
        return PSA_ERROR_NOT_SUPPORTED;
    }

    /* Validate sizes, as p256-m expects fixed-size buffers */
    if (key_buffer_size != 32) {
        return PSA_ERROR_CORRUPTION_DETECTED;
    }
    if (data_size < 65) {
        return PSA_ERROR_BUFFER_TOO_SMALL;
    }

    /* Output public key in the PSA export format */
    data[0] = 0x04;
    int ret = p256_public_from_private(data + 1, key_buffer);
    if (ret == P256_SUCCESS) {
        *data_length = 65;
    }

    return p256_to_psa_error(ret);
}

psa_status_t p256_transparent_generate_key(
    const psa_key_attributes_t *attributes,
    uint8_t *key_buffer,
    size_t key_buffer_size,
    size_t *key_buffer_length)
{
    /* We don't use this argument, but the specification mandates the signature
     * of driver entry-points. (void) used to avoid compiler warning. */
    (void) attributes;

    /* Validate sizes, as p256-m expects fixed-size buffers */
    if (key_buffer_size != 32) {
        return PSA_ERROR_BUFFER_TOO_SMALL;
    }

    /*
     *  p256-m's keypair generation function outputs both public and private
     *  keys. Allocate a buffer to which the public key will be written. The
     *  private key will be written to key_buffer, which is passed to this
     *  function as an argument. */
    uint8_t public_key_buffer[64];

    int ret = p256_gen_keypair(key_buffer, public_key_buffer);
    if (ret == P256_SUCCESS) {
        *key_buffer_length = 32;
    }

    return p256_to_psa_error(ret);
}

psa_status_t p256_transparent_key_agreement(
    const psa_key_attributes_t *attributes,
    const uint8_t *key_buffer,
    size_t key_buffer_size,
    psa_algorithm_t alg,
    const uint8_t *peer_key,
    size_t peer_key_length,
    uint8_t *shared_secret,
    size_t shared_secret_size,
    size_t *shared_secret_length)
{
    /* We don't use these arguments, but the specification mandates the
     * sginature of driver entry-points. (void) used to avoid compiler
     * warning. */
    (void) attributes;
    (void) alg;

    /* Validate sizes, as p256-m expects fixed-size buffers */
    if (key_buffer_size != 32 || peer_key_length != 65) {
        return PSA_ERROR_INVALID_ARGUMENT;
    }
    if (shared_secret_size < 32) {
        return PSA_ERROR_BUFFER_TOO_SMALL;
    }

    /* We add 1 to peer_key pointer to omit the leading byte of the public key
     * representation (0x04). See information about PSA key formats at the top
     * of the file. */
    int ret = p256_ecdh_shared_secret(shared_secret, key_buffer, peer_key + 1);
    if (ret == P256_SUCCESS) {
        *shared_secret_length = 32;
    }

    return p256_to_psa_error(ret);
}

psa_status_t p256_transparent_sign_hash(
    const psa_key_attributes_t *attributes,
    const uint8_t *key_buffer,
    size_t key_buffer_size,
    psa_algorithm_t alg,
    const uint8_t *hash,
    size_t hash_length,
    uint8_t *signature,
    size_t signature_size,
    size_t *signature_length)
{
    /* We don't use these arguments, but the specification mandates the
     * sginature of driver entry-points. (void) used to avoid compiler
     * warning. */
    (void) attributes;
    (void) alg;

    /* Validate sizes, as p256-m expects fixed-size buffers */
    if (key_buffer_size != 32) {
        return PSA_ERROR_CORRUPTION_DETECTED;
    }
    if (signature_size < 64) {
        return PSA_ERROR_BUFFER_TOO_SMALL;
    }

    int ret = p256_ecdsa_sign(signature, key_buffer, hash, hash_length);
    if (ret == P256_SUCCESS) {
        *signature_length = 64;
    }

    return p256_to_psa_error(ret);
}

/*  This function expects the key buffer to contain a 65 byte public key,
 *  as exported by psa_export_public_key() */
static psa_status_t p256_verify_hash_with_public_key(
    const uint8_t *key_buffer,
    size_t key_buffer_size,
    const uint8_t *hash,
    size_t hash_length,
    const uint8_t *signature,
    size_t signature_length)
{
    /* Validate sizes, as p256-m expects fixed-size buffers */
    if (key_buffer_size != 65 || *key_buffer != 0x04) {
        return PSA_ERROR_CORRUPTION_DETECTED;
    }
    if (signature_length != 64) {
        return PSA_ERROR_INVALID_SIGNATURE;
    }

    /* We add 1 to public_key_buffer pointer to omit the leading byte of the
     * public key representation (0x04). See information about PSA key formats
     * at the top of the file. */
    const uint8_t *public_key_buffer = key_buffer + 1;
    int ret = p256_ecdsa_verify(signature, public_key_buffer, hash, hash_length);

    return p256_to_psa_error(ret);
}

psa_status_t p256_transparent_verify_hash(
    const psa_key_attributes_t *attributes,
    const uint8_t *key_buffer,
    size_t key_buffer_size,
    psa_algorithm_t alg,
    const uint8_t *hash,
    size_t hash_length,
    const uint8_t *signature,
    size_t signature_length)
{
    /* We don't use this argument, but the specification mandates the signature
     * of driver entry-points. (void) used to avoid compiler warning. */
    (void) alg;

    psa_status_t status;
    uint8_t public_key_buffer[65];
    size_t public_key_buffer_size = 65;

    size_t public_key_length = 65;
    /* As p256-m doesn't require dynamic allocation, we want to avoid it in
     * the entrypoint functions as well. psa_driver_wrapper_export_public_key()
     * requires size_t*, so we use a pointer to a stack variable. */
    size_t *public_key_length_ptr = &public_key_length;

    /* The contents of key_buffer may either be the 32 byte private key
     * (keypair format), or 0x04 followed by the 64 byte public key (public
     * key format). To ensure the key is in the latter format, the public key
     * is exported. */
    status = psa_driver_wrapper_export_public_key(
        attributes,
        key_buffer,
        key_buffer_size,
        public_key_buffer,
        public_key_buffer_size,
        public_key_length_ptr);
    if (status != PSA_SUCCESS) {
        goto exit;
    }

    status = p256_verify_hash_with_public_key(
        public_key_buffer,
        public_key_buffer_size,
        hash,
        hash_length,
        signature,
        signature_length);

exit:
    return status;
}

#endif /* MBEDTLS_P256M_EXAMPLE_DRIVER_ENABLED */
