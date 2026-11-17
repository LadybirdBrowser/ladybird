/*
 * Copyright (c) 2025, Altomani Gianluca <altomanigianluca@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

extern "C" {
typedef struct engine_st ENGINE;
typedef struct evp_md_st EVP_MD;
typedef struct evp_md_ctx_st EVP_MD_CTX;
typedef struct evp_pkey_st EVP_PKEY;
typedef struct evp_pkey_ctx_st EVP_PKEY_CTX;
typedef struct x509_st X509;

void ERR_print_errors_cb(int (*cb)(char const* str, size_t len, void* u), void* u);

void X509_free(X509*);

EVP_MD_CTX* EVP_MD_CTX_new();
void EVP_MD_CTX_free(EVP_MD_CTX*);

void EVP_PKEY_CTX_free(EVP_PKEY_CTX*);
void EVP_PKEY_free(EVP_PKEY*);
}
