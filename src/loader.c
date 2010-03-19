/* -*- mode: C; c-basic-offset: 4 -*-
 *
 * Copyright (c) 2007, 2008, 2009, 2010 Tokutek Inc.  All rights reserved." 
 * The technology is licensed by the Massachusetts Institute of Technology, 
 * Rutgers State University of New Jersey, and the Research Foundation of 
 * State University of New York at Stony Brook under United States of America 
 * Serial No. 11/760379 and to the patents and/or patent applications resulting from it."
 */

/*
 *   The loader
 *
 *   (Once we have a functioning loader - currently defaults to reference model)
 *   If LOADER_USE_REFERENCE_MODEL is defined at compile time, a reference model is compiled
 *       $ make LOADER_USE_REFERENCE_MODEL=1 ...
 */

#include <toku_portability.h>
#include <stdio.h>
#include <string.h>
#include "ydb-internal.h"
#include "../newbrt/brtloader.h"
#include "loader.h"
#include "ydb_load.h"
#include "checkpoint.h"
#include "brt-internal.h"

enum {MAX_FILE_SIZE=256};

struct __toku_loader_internal {
    DB_ENV *env;
    DB_TXN *txn;
    BRTLOADER brt_loader;
    int N;
    DB **dbs; /* [N] */
    DB *src_db;
    uint32_t *db_flags;
    uint32_t *dbt_flags;
    uint32_t loader_flags;
    void *extra;
    void (*error_callback)(DB *db, int i, int err, DBT *key, DBT *val, void *extra);
    int  (*poll_func)(void *extra, float progress);
    char *temp_file_template;

    DBT *ekeys;
    DBT *evals;

    DBT err_key;   /* error key */
    DBT err_val;   /* error val */
    int err_i;     /* error i   */
    int err_errno;

    char **inames_in_env; /* [N]  inames of new files to be created */
};

static int verify_empty(DB *db, DB_TXN *txn) 
{
    int r;
    DBC *cursor;
    DBT k, v;
    toku_init_dbt(&k);
    toku_init_dbt(&v);

    r = db->cursor(db, txn, &cursor, 0);             assert(r == 0);
    r = cursor->c_get(cursor, &k, &v, DB_NEXT);
    {
        int r2 = cursor->c_close(cursor);
        assert(r2==0);
    }
    if (r==DB_NOTFOUND) r = 0;
    else if (r==0)      r = DB_KEYEXIST;
    return r;
}

int toku_loader_create_loader(DB_ENV *env, 
                              DB_TXN *txn, 
                              DB_LOADER **blp, 
                              DB *src_db, 
                              int N, 
                              DB *dbs[], 
                              uint32_t db_flags[N], 
                              uint32_t dbt_flags[N], 
                              uint32_t loader_flags,
                              void *extra UU())
{
    DB_LOADER *loader;
    XCALLOC(loader);       // init to all zeroes
    XCALLOC(loader->i);

    loader->i->env                = env;
    loader->i->txn                = txn;
    loader->i->N                  = N;
    loader->i->src_db             = src_db;
    loader->i->dbs                = dbs;
    loader->i->db_flags           = db_flags;
    loader->i->dbt_flags          = dbt_flags;
    loader->i->loader_flags       = loader_flags;
    loader->i->extra              = extra;
    loader->i->temp_file_template = (char *)toku_malloc(MAX_FILE_SIZE);

    int n = snprintf(loader->i->temp_file_template, MAX_FILE_SIZE, "%s/tempXXXXXX", env->i->dir);
    assert(n>0 && n<MAX_FILE_SIZE);

    memset(&loader->i->err_key, 0, sizeof(loader->i->err_key));
    memset(&loader->i->err_val, 0, sizeof(loader->i->err_val));
    loader->i->err_i      = 0;
    loader->i->err_errno  = 0;

    loader->set_poll_function      = toku_loader_set_poll_function;
    loader->set_error_callback     = toku_loader_set_error_callback;
    loader->put                    = toku_loader_put;
    loader->close                  = toku_loader_close;
    loader->abort                  = toku_loader_abort;

    int r;
    // lock tables and check empty
    for(int i=0;i<N;i++) {
        r = toku_db_pre_acquire_table_lock(dbs[i], txn);
        assert(r==0);
        r = verify_empty(dbs[i], txn);
        assert(r==0);
    }

    brt_compare_func compare_functions[N];
    for (int i=0; i<N; i++) {
	compare_functions[i] = dbs[i]->i->key_compare_was_set ? toku_brt_get_bt_compare(dbs[i]->i->brt) : env->i->bt_compare;
    }

    // time to open the big kahuna
#ifdef LOADER_USE_REFERENCE_MODEL
    loader->i->loader_flags |= LOADER_USE_PUTS;
#endif
    if ( loader->i->loader_flags & LOADER_USE_PUTS ) {
        XCALLOC_N(loader->i->N, loader->i->ekeys);
        XCALLOC_N(loader->i->N, loader->i->evals);
        for (int i=0; i<N; i++) {
            loader->i->ekeys[i].flags = DB_DBT_REALLOC;
            loader->i->evals[i].flags = DB_DBT_REALLOC;
        }
        loader->i->brt_loader = NULL;
    }
    else {
        char **XMALLOC_N(N, new_inames_in_env);
	const struct descriptor **XMALLOC_N(N, descriptors);
	for (int i=0; i<N; i++) {
	    descriptors[i] = &dbs[i]->i->brt->h->descriptor;
	}
        loader->i->ekeys = NULL;
        loader->i->evals = NULL;
        r = ydb_load_inames (env, txn, N, dbs, new_inames_in_env);
        assert(r==0);
        toku_brt_loader_open(&loader->i->brt_loader,
                             loader->i->env->i->cachetable,
                             loader->i->env->i->generate_row_for_put,
                             src_db,
                             N,
                             dbs,
			     descriptors,
                             (const char **)new_inames_in_env,
                             compare_functions,
                             loader->i->temp_file_template);
        loader->i->inames_in_env = new_inames_in_env;
	toku_free(descriptors);
    }
    *blp = loader;

    return 0;
}

int toku_loader_set_poll_function(DB_LOADER *loader,
                                  int (*poll_func)(void *extra, float progress)) 
{
    loader->i->poll_func = poll_func;
    return 0;
}

int toku_loader_set_error_callback(DB_LOADER *loader, 
                                   void (*error_cb)(DB *db, int i, int err, DBT *key, DBT *val, void *extra)) 
{
    loader->i->error_callback = error_cb;
    return 0;
}

int toku_loader_put(DB_LOADER *loader, DBT *key, DBT *val) 
{
    int r = 0;
    int i = 0;
    //      err_i is unused now( always 0).  How would we know which dictionary
    //      the error happens in?  (put_multiple and toku_brt_loader_put do NOT report
    //      which dictionary). 

    // skip put if error already found
    if ( loader->i->err_errno != 0 ) {
        return 0;
    }

    if ( loader->i->loader_flags & LOADER_USE_PUTS ) {
        r = loader->i->env->put_multiple(loader->i->env,
                                         loader->i->src_db, // src_db
                                         loader->i->txn,
                                         key, val,
                                         loader->i->N, // num_dbs
                                         loader->i->dbs, // (DB**)db_array
                                         loader->i->ekeys, 
                                         loader->i->evals,
                                         loader->i->db_flags, // flags_array
                                         NULL);
    }
    else {
        r = toku_brt_loader_put(loader->i->brt_loader, key, val);
    }
    if ( r != 0 ) {
        // spec says errors all happen on close
        //   - have to save key, val, errno (r) and i for duplicate callback
        loader->i->err_key.size = key->size;
        loader->i->err_key.data = toku_malloc(key->size);
        memcpy(loader->i->err_key.data, key->data, key->size);

        loader->i->err_val.size = val->size;
        loader->i->err_val.data = toku_malloc(val->size);
        memcpy(loader->i->err_val.data, val->data, val->size);

        loader->i->err_i = i;
        loader->i->err_errno = r;

        return -1;
    }
    return 0;
}

int toku_loader_close(DB_LOADER *loader) 
{
    if ( loader->i->err_errno != 0 ) {
        if ( loader->i->error_callback != NULL ) {
            loader->i->error_callback(loader->i->dbs[loader->i->err_i], loader->i->err_i, loader->i->err_errno, &loader->i->err_key, &loader->i->err_val, NULL);
        }
        toku_free(loader->i->err_key.data);
        toku_free(loader->i->err_val.data);
    }

    if ( !(loader->i->loader_flags & LOADER_USE_PUTS ) ) {
        toku_brt_loader_close(loader->i->brt_loader);

        for (int i=0; i<loader->i->N; i++) {
            toku_ydb_lock(); //Must hold ydb lock for dictionary_redirect.
            int r = toku_dictionary_redirect(loader->i->inames_in_env[i],
                                             loader->i->dbs[i]->i->brt,
                                             db_txn_struct_i(loader->i->txn)->tokutxn);
            assert(r==0);
            toku_ydb_unlock();
            toku_free(loader->i->inames_in_env[i]);
        }
        toku_free(loader->i->inames_in_env);
        toku_free(loader->i->brt_loader);
        // TODO: release table locks
    }
    if (loader->i->loader_flags & LOADER_USE_PUTS) {
        int num_dbts = loader->i->N;
        for (int i=0; i<num_dbts; i++) {
            if (loader->i->ekeys &&
		loader->i->ekeys[i].flags == DB_DBT_REALLOC &&
                loader->i->ekeys[i].data) {
                toku_free(loader->i->ekeys[i].data);
            }
            if (loader->i->evals &&
		loader->i->evals[i].flags == DB_DBT_REALLOC &&
                loader->i->evals[i].data) {
                toku_free(loader->i->evals[i].data);
            }
        }
        if (loader->i->ekeys) toku_free(loader->i->ekeys);
        if (loader->i->evals) toku_free(loader->i->evals);
    }
    toku_free(loader->i->temp_file_template);
    toku_free(loader->i);
    toku_free(loader);
    return 0;
}

int toku_loader_abort(DB_LOADER *loader) 
{
    if ( loader->i->loader_flags & LOADER_USE_PUTS ) {
        return toku_loader_close(loader);
    }
    else {
        loader = loader;
        assert(1); // not implemented
        return 0;
    }
}
