// Copyright 2019 The Beam Team
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.#include "misc.h"

#include "nonce_generator.h"

const uint8_t scalar_order[] =
    {  // fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141
        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0xFF, 0xFF, 0xFE, 0xBA, 0xAE, 0xDC, 0xE6, 0xAF, 0x48,
        0xA0, 0x3B, 0xBF, 0xD2, 0x5E, 0x8C, 0xD0, 0x36, 0x41, 0x41};

inline int is_scalar_valid(uint8_t *scalar_bytes) {
  return os_memcmp(scalar_bytes, scalar_order, 32) < 0;
}

inline void nonce_generator_init(nonce_generator_t *nonce, const uint8_t *salt,
                                 uint8_t salt_size) {
  nonce->number = 1;
  os_memset(nonce->okm, 0, sizeof(nonce->okm));
  os_memset(nonce->prk, 0, sizeof(nonce->prk));
  beam_hash_hmac_sha256_init(&nonce->hash, (uint8_t *)salt, salt_size);
}

inline void nonce_generator_write(nonce_generator_t *nonce, const uint8_t *seed,
                                  uint8_t seed_size) {
  beam_hash_hmac_sha256_update(&nonce->hash, seed, seed_size);
}

inline void nonce_generator_get_first_output_key_material(
    nonce_generator_t *nonce, const uint8_t *context, size_t context_size) {
  beam_hash_hmac_sha256_final(&nonce->hash, nonce->prk);
  beam_hash_hmac_sha256_init(&nonce->hash, nonce->prk, 32);

  beam_hash_hmac_sha256_update(&nonce->hash, context, context_size);
  beam_hash_hmac_sha256_update(&nonce->hash, &nonce->number, 1);
  beam_hash_hmac_sha256_final(&nonce->hash, nonce->okm);
}

inline void nonce_generator_get_rest_output_key_material(
    nonce_generator_t *nonce, const uint8_t *context, size_t context_size) {
  beam_hash_hmac_sha256_init(&nonce->hash, nonce->prk, 32);

  beam_hash_hmac_sha256_update(&nonce->hash, nonce->okm, 32);
  beam_hash_hmac_sha256_update(&nonce->hash, context, context_size);
  beam_hash_hmac_sha256_update(&nonce->hash, &nonce->number, 1);
  beam_hash_hmac_sha256_final(&nonce->hash, nonce->okm);
}

inline uint8_t nonce_generator_export_output_key(nonce_generator_t *nonce,
                                                 const uint8_t *context,
                                                 uint8_t context_size,
                                                 uint8_t *okm32) {
  if (1 == nonce->number) {
    nonce_generator_get_first_output_key_material(nonce, context, context_size);
  } else {
    nonce_generator_get_rest_output_key_material(nonce, context, context_size);
  }

  if (NULL != okm32) os_memcpy(okm32, nonce->okm, 32);
  return ++nonce->number;
}

inline uint8_t nonce_generator_export_scalar(nonce_generator_t *nonce,
                                             const uint8_t *context,
                                             uint8_t context_size,
                                             secp256k1_scalar *out_scalar) {
  secp256k1_scalar_clear(out_scalar);
  do {
    nonce_generator_export_output_key(nonce, context, context_size, NULL);
  } while (!scalar_import_nnz(out_scalar, nonce->okm));

  return nonce->number;
}