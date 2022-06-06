#include <ngx_lua_resty_lmdb_module.h>


#define ENC_KEY_LEN         32

#ifndef EVP_DIGEST_CONSTANT
#define EVP_DIGEST_CONSTANT "konglmdb"
#endif


static void *ngx_lua_resty_lmdb_create_conf(ngx_cycle_t *cycle);
static char *ngx_lua_resty_lmdb_init_conf(ngx_cycle_t *cycle, void *conf);
static ngx_int_t ngx_lua_resty_lmdb_init(ngx_cycle_t *cycle);
static ngx_int_t ngx_lua_resty_lmdb_init_worker(ngx_cycle_t *cycle);
static void ngx_lua_resty_lmdb_exit_worker(ngx_cycle_t *cycle);


static int get_digest_key(ngx_str_t *passwd, MDB_val *key);
static int lmdb_encrypt_func(const MDB_val *src, MDB_val *dst,
                        const MDB_val *key, int encdec);


static const EVP_CIPHER *cipher;


static ngx_command_t  ngx_lua_resty_lmdb_commands[] = {

    { ngx_string("lmdb_environment_path"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_path_slot,
      0,
      offsetof(ngx_lua_resty_lmdb_conf_t, env_path),
      NULL },

    { ngx_string("lmdb_max_databases"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      0,
      offsetof(ngx_lua_resty_lmdb_conf_t, max_databases),
      NULL },

    { ngx_string("lmdb_map_size"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_size_slot,
      0,
      offsetof(ngx_lua_resty_lmdb_conf_t, map_size),
      NULL },

    { ngx_string("lmdb_encryption_key_data"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      0,
      offsetof(ngx_lua_resty_lmdb_conf_t, key_data),
      NULL },

    { ngx_string("lmdb_encryption_type"),
      NGX_MAIN_CONF|NGX_DIRECT_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_str_slot,
      0,
      offsetof(ngx_lua_resty_lmdb_conf_t, encryption_type),
      NULL },

      ngx_null_command
};


static ngx_core_module_t  ngx_lua_resty_lmdb_module_ctx = {
   ngx_string("lua_resty_lmdb"),
   ngx_lua_resty_lmdb_create_conf,
   ngx_lua_resty_lmdb_init_conf
};


ngx_module_t  ngx_lua_resty_lmdb_module = {
    NGX_MODULE_V1,
    &ngx_lua_resty_lmdb_module_ctx,        /* module context */
    ngx_lua_resty_lmdb_commands,           /* module directives */
    NGX_CORE_MODULE,                       /* module type */
    NULL,                                  /* init master */
    ngx_lua_resty_lmdb_init,               /* init module */
    ngx_lua_resty_lmdb_init_worker,        /* init process */
    NULL,                                  /* init thread */
    NULL,                                  /* exit thread */
    ngx_lua_resty_lmdb_exit_worker,        /* exit process */
    NULL,                                  /* exit master */
    NGX_MODULE_V1_PADDING
};


static void *
ngx_lua_resty_lmdb_create_conf(ngx_cycle_t *cycle)
{
    ngx_lua_resty_lmdb_conf_t  *lcf;

    lcf = ngx_pcalloc(cycle->pool, sizeof(ngx_lua_resty_lmdb_conf_t));
    if (lcf == NULL) {
        return NULL;
    }

    /*
     * set by ngx_pcalloc():
     *
     *     conf->env_path = NULL;
     *     conf->env = NULL;
     */

    lcf->max_databases = NGX_CONF_UNSET_SIZE;
    lcf->map_size = NGX_CONF_UNSET_SIZE;

    return lcf;
}


static char *
ngx_lua_resty_lmdb_init_conf(ngx_cycle_t *cycle, void *conf)
{
    ngx_lua_resty_lmdb_conf_t *lcf = conf;

    ngx_conf_init_size_value(lcf->max_databases, 1);

    /* same as mdb.c DEFAULT_MAPSIZE */
    ngx_conf_init_size_value(lcf->map_size, 1048576);

    /* The default encryption mode is aes-256-gcm */
    if (lcf->encryption_type.data == NULL) {
        ngx_str_set(&lcf->encryption_type, "aes-256-gcm");
    }

    if (ngx_strcasecmp(
            lcf->encryption_type.data, (u_char*)"aes-256-gcm") != 0 &&
        ngx_strcasecmp(
            lcf->encryption_type.data, (u_char*)"chacha20-poly1305") != 0 ) {

        ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                "invalid \"lmdb_encryption_type\": \"%V\"",
                &lcf->encryption_type);

        return NGX_CONF_ERROR;
    }

    if (lcf->key_data.data != NULL) {
        cipher = EVP_get_cipherbyname((char *)lcf->encryption_type.data);

        if (cipher == NULL ) {
            ngx_log_error(NGX_LOG_EMERG, cycle->log, 0,
                "init \"lmdb_encryption\": \"%V\" failed",
                &lcf->encryption_type);

            return NGX_CONF_ERROR;
        }
    }

    return NGX_CONF_OK;
}


static ngx_int_t ngx_lua_resty_lmdb_init(ngx_cycle_t *cycle) {
    /* ngx_lua_resty_lmdb_conf_t *lcf;

    lcf = (ngx_lua_resty_lmdb_conf_t *) ngx_get_conf(cycle->conf_ctx,
                                                     ngx_lua_resty_lmdb_module); */

    return NGX_OK;
}


static ngx_int_t ngx_lua_resty_lmdb_init_worker(ngx_cycle_t *cycle)
{
    ngx_lua_resty_lmdb_conf_t *lcf;
    int                        rc;
    int                        dead;
    MDB_val                    enckey;
    int                        block_size = 0;
    u_char                     keybuf[2048];

    lcf = (ngx_lua_resty_lmdb_conf_t *) ngx_get_conf(cycle->conf_ctx,
                                                     ngx_lua_resty_lmdb_module);

    if (lcf == NULL || lcf->env_path == NULL) {
        return NGX_OK;
    }

    rc = mdb_env_create(&lcf->env);
    if (rc != 0) {
        ngx_log_error(NGX_LOG_CRIT, cycle->log, 0,
                      "unable to create LMDB environment");
        return NGX_ERROR;
    }

    rc = mdb_env_set_mapsize(lcf->env, lcf->map_size);
    if (rc != 0) {
        ngx_log_error(NGX_LOG_CRIT, cycle->log, 0,
                      "unable to set map size for LMDB");
        return NGX_ERROR;
    }

    rc = mdb_env_set_maxdbs(lcf->env, lcf->max_databases);
    if (rc != 0) {
        ngx_log_error(NGX_LOG_CRIT, cycle->log, 0,
                      "unable to set maximum DB count for LMDB");
        return NGX_ERROR;
    }

    if (cipher != NULL) {
        enckey.mv_data = keybuf;
        enckey.mv_size = ENC_KEY_LEN;

        rc = get_digest_key(&lcf->key_data, &enckey);
        if (rc != 0) {
            ngx_log_error(NGX_LOG_CRIT, cycle->log, 0,
                "unable to set LMDB encryption key, string to key");
            return NGX_ERROR;
        }

        block_size = EVP_CIPHER_block_size(cipher);
        if (block_size == 0) {
            ngx_log_error(NGX_LOG_CRIT, cycle->log, 0,
                "unable to set LMDB encryption key, block size");
            return NGX_ERROR;
        }

        rc = mdb_env_set_encrypt(lcf->env, lmdb_encrypt_func, &enckey, block_size);
        if (rc != 0) {
            ngx_log_error(NGX_LOG_CRIT, cycle->log, 0,
                "unable to set LMDB encryption key: %s", mdb_strerror(rc));
            return NGX_ERROR;
        }

        /* destroy data*/
        ngx_memzero(lcf->key_data.data, lcf->key_data.len);
        ngx_memzero(lcf->encryption_type.data, lcf->encryption_type.len);

        ngx_str_null(&lcf->key_data);
        ngx_str_null(&lcf->encryption_type);
    }

    rc = mdb_env_open(lcf->env, (const char *) lcf->env_path->name.data, 0, 0600);
    if (rc != 0) {
        ngx_log_error(NGX_LOG_CRIT, cycle->log, 0,
                      "unable to open LMDB environment: %s", mdb_strerror(rc));
        return NGX_ERROR;
    }

    rc = mdb_reader_check(lcf->env, &dead);
    if (rc != 0) {
        ngx_log_error(NGX_LOG_CRIT, cycle->log, 0,
                      "unable to check LMDB reader slots: %s", mdb_strerror(rc));
        /* this is not a fatal error */

    } else if (dead > 0) {
        ngx_log_error(NGX_LOG_WARN, cycle->log, 0,
                      "found and cleared %d stale readers from LMDB", dead);
    }

    rc = mdb_txn_begin(lcf->env, NULL, MDB_RDONLY, &lcf->ro_txn);
    if (rc != 0) {
        ngx_log_error(NGX_LOG_CRIT, cycle->log, 0,
                      "unable to open LMDB read only transaction: %s", mdb_strerror(rc));
        return NGX_ERROR;
    }

    mdb_txn_reset(lcf->ro_txn);

    return NGX_OK;
}


static void ngx_lua_resty_lmdb_exit_worker(ngx_cycle_t *cycle)
{
    ngx_lua_resty_lmdb_conf_t *lcf;

    lcf = (ngx_lua_resty_lmdb_conf_t *) ngx_get_conf(cycle->conf_ctx,
                                                     ngx_lua_resty_lmdb_module);

    if (lcf == NULL || lcf->env_path == NULL) {
        return;
    }

    if (lcf->env != NULL) {
        mdb_txn_abort(lcf->ro_txn);
        mdb_env_close(lcf->env);
    }
}


static int get_digest_key(ngx_str_t *passwd, MDB_val *key)
{
    unsigned int    size;
    int             rc;
    EVP_MD_CTX     *mdctx = EVP_MD_CTX_new();

    rc = EVP_DigestInit_ex(mdctx, EVP_sha256(), NULL);
    if (rc) {
        rc = EVP_DigestUpdate(mdctx,
                EVP_DIGEST_CONSTANT, sizeof(EVP_DIGEST_CONSTANT));
    }

    if (rc) {
        rc = EVP_DigestUpdate(mdctx, passwd->data, passwd->len);
    }

    if (rc) {
        rc = EVP_DigestFinal_ex(mdctx, key->mv_data, &size);
    }

    EVP_MD_CTX_free(mdctx);
    return rc == 0;
}


static int
lmdb_encrypt_func(const MDB_val *src, MDB_val *dst, const MDB_val *key, int encdec)
{
    u_char                      iv[12];
    int                         ivl, outl, rc;
    mdb_size_t                 *ptr;
    EVP_CIPHER_CTX             *ctx = EVP_CIPHER_CTX_new();

    ptr = key[1].mv_data;
    ivl = ptr[0] & 0xffffffff;
    ngx_memcpy(iv, &ivl, sizeof(int));
    ngx_memcpy(iv + sizeof(int), ptr + 1, sizeof(mdb_size_t));

    rc = EVP_CipherInit_ex(ctx, cipher, NULL, key[0].mv_data, iv, encdec);
    if (rc) {
        EVP_CIPHER_CTX_set_padding(ctx, 0);
    }

    if (rc && !encdec) {
        rc = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_SET_TAG,
                                 key[2].mv_size, key[2].mv_data);
    }

    if (rc) {
        rc = EVP_CipherUpdate(ctx, dst->mv_data, &outl,
                              src->mv_data, src->mv_size);
    }

    if (rc) {
        rc = EVP_CipherFinal_ex(ctx, key[2].mv_data, &outl);
    }

    if (rc && encdec) {
        rc = EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_AEAD_GET_TAG,
                                 key[2].mv_size, key[2].mv_data);
    }

    return rc == 0;
}


