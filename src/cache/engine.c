/*
 * Cache engine functions.
 *
 * Copyright (C) [Jiang Wenyuan](https://github.com/jiangwenyuan), < koubunen AT gmail DOT com >
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include <types/applet.h>
#include <types/cli.h>
#include <types/global.h>
#include <types/cache.h>

#include <proto/filters.h>
#include <proto/log.h>
#include <proto/proto_http.h>
#include <proto/sample.h>
#include <proto/raw_sock.h>
#include <proto/stream_interface.h>
#include <proto/acl.h>
#include <proto/proxy.h>
#include <proto/cache.h>

#include <import/xxhash.h>

#ifdef USE_OPENSSL
#include <proto/ssl_sock.h>
#include <types/ssl_sock.h>
#endif

static const char *cache_msgs[NUSTER_CACHE_MSG_SIZE] = {
    [NUSTER_CACHE_200] =
        "HTTP/1.0 200 OK\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "OK\n",

    [NUSTER_CACHE_400] =
        "HTTP/1.0 400 Bad request\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "Bad request\n",

    [NUSTER_CACHE_404] =
        "HTTP/1.0 404 Not Found\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "Not Found\n",

    [NUSTER_CACHE_500] =
        "HTTP/1.0 500 Server Error\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: close\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "Server Error\n",
};

struct chunk cache_msg_chunks[NUSTER_CACHE_MSG_SIZE];

/*
 * Cache the keys which calculated in request for response use
 */
struct cache_rule_stash *cache_stash_rule(struct cache_ctx *ctx,
        struct cache_rule *rule, char *key, uint64_t hash) {

    struct cache_rule_stash *stash = pool_alloc2(global.cache.pool.stash);

    if(stash) {
        stash->rule = rule;
        stash->key  = key;
        stash->hash = hash;
        if(ctx->stash) {
            stash->next = ctx->stash;
        } else {
            stash->next = NULL;
        }
        ctx->stash = stash;
    }
    return stash;
}

int cache_test_rule(struct cache_rule *rule, struct stream *s, int res) {
    int ret;

    /* no acl defined */
    if(!rule->cond) {
        return 1;
    }

    if(res) {
        ret = acl_exec_cond(rule->cond, s->be, s->sess, s, SMP_OPT_DIR_RES|SMP_OPT_FINAL);
    } else {
        ret = acl_exec_cond(rule->cond, s->be, s->sess, s, SMP_OPT_DIR_REQ|SMP_OPT_FINAL);
    }
    ret = acl_pass(ret);
    if(rule->cond->pol == ACL_COND_UNLESS) {
        ret = !ret;
    }

    if(ret) {
        return 1;
    }
    return 0;
}

static char *_cache_key_append(char *dst, int *dst_len, int *dst_size,
        char *src, int src_len) {

    int left     = *dst_size - *dst_len;
    int need     = src_len + 1;
    int old_size = *dst_size;

    if(left < need) {
        *dst_size += ((need - left) / CACHE_DEFAULT_KEY_SIZE + 1)  * CACHE_DEFAULT_KEY_SIZE;
    }

    if(old_size != *dst_size) {
        char *new_dst = realloc(dst, *dst_size);
        if(!new_dst) {
            free(dst);
            return NULL;
        }
        dst = new_dst;
    }

    memcpy(dst + *dst_len, src, src_len);
    *dst_len += src_len;
    dst[*dst_len] = '\0';
    return dst;
}

static int _cache_find_param_value_by_name(char *query_beg, char *query_end,
        char *name, char **value, int *value_len) {

    char equal   = '=';
    char and     = '&';
    char *ptr    = query_beg;
    int name_len = strlen(name);

    while(ptr + name_len + 1 < query_end) {
        if(!memcmp(ptr, name, name_len) && *(ptr + name_len) == equal) {
            if(ptr == query_beg || *(ptr - 1) == and) {
                ptr    = ptr + name_len + 1;
                *value = ptr;
                while(ptr < query_end && *ptr != and) {
                    (*value_len)++;
                    ptr++;
                }
                return 1;
            }
        }
        ptr++;
    }
    return 0;
}

/*
 * create a new cache_data and insert it to cache->data list
 */
struct cache_data *cache_data_new() {

    struct cache_data *data = cache_memory_alloc(global.cache.pool.data, sizeof(*data));

    nuster_shctx_lock(cache);
    if(data) {
        data->clients  = 0;
        data->invalid  = 0;
        data->element  = NULL;

        if(cache->data_head == NULL) {
            cache->data_head = data;
            cache->data_tail = data;
            data->next       = data;
        } else {
            if(cache->data_head == cache->data_tail) {
                cache->data_head->next = data;
                data->next             = cache->data_head;
                cache->data_tail       = data;
            } else {
                data->next             = cache->data_head;
                cache->data_tail->next = data;
                cache->data_tail       = data;
            }
        }
    }
    nuster_shctx_unlock(cache);
    return data;
}

/*
 * Append partial http response data
 */
static struct cache_element *cache_data_append(struct cache_element *tail,
        struct http_msg *msg, long msg_len) {

    struct cache_element *element = cache_memory_alloc(global.cache.pool.element, sizeof(*element));

    if(element) {
        char *data = msg->chn->buf->data;
        char *p    = msg->chn->buf->p;
        int size   = msg->chn->buf->size;

        element->msg = cache_memory_alloc(global.cache.pool.chunk, msg_len);
        if(!element->msg) return NULL;

        if(p - data + msg_len > size) {
            int right = data + size - p;
            int left  = msg_len - right;
            memcpy(element->msg, p, right);
            memcpy(element->msg + right, data, left);
        } else {
            memcpy(element->msg, p, msg_len);
        }
        element->msg_len = msg_len;
        element->next    = NULL;
        if(tail == NULL) {
            tail = element;
        } else {
            tail->next = element;
        }
        cache_stats_update_used_mem(msg_len);
    }
    return element;
}


static int _cache_data_invalid(struct cache_data *data) {
    if(data->invalid) {
        if(!data->clients) {
            return 1;
        }
    }
    return 0;
}

/*
 * free invalid cache_data
 */
static void _cache_data_cleanup() {
    struct cache_data *data = NULL;

    if(cache->data_head) {
        if(cache->data_head == cache->data_tail) {
            if(_cache_data_invalid(cache->data_head)) {
                data             = cache->data_head;
                cache->data_head = NULL;
                cache->data_tail = NULL;
            }
        } else {
            if(_cache_data_invalid(cache->data_head)) {
                data                   = cache->data_head;
                cache->data_tail->next = cache->data_head->next;
                cache->data_head       = cache->data_head->next;
            } else {
                cache->data_tail       = cache->data_head;
                cache->data_head       = cache->data_head->next;
            }
        }
    }

    if(data) {
        struct cache_element *element = data->element;
        while(element) {
            struct cache_element *tmp = element;
            element                   = element->next;

            cache_stats_update_used_mem(-tmp->msg_len);
            cache_memory_free(global.cache.pool.chunk, tmp->msg);
            cache_memory_free(global.cache.pool.element, tmp);
        }
        cache_memory_free(global.cache.pool.data, data);
    }
}

void cache_housekeeping() {
    if(global.cache.status == CACHE_STATUS_ON) {
        cache_dict_rehash();
        nuster_shctx_lock(&cache->dict[0]);
        cache_dict_cleanup();
        nuster_shctx_unlock(&cache->dict[0]);
        nuster_shctx_lock(cache);
        _cache_data_cleanup();
        nuster_shctx_unlock(cache);
    }
}

void cache_init() {
    int i;
    struct proxy *p;

    if(global.cache.status == CACHE_STATUS_ON) {
        if(global.cache.share == CACHE_STATUS_UNDEFINED) {
            if(global.nbproc == 1) {
                global.cache.share = CACHE_SHARE_OFF;
            } else {
                global.cache.share = CACHE_SHARE_ON;
            }
        }

        global.cache.pool.stash   = create_pool("cp.stash", sizeof(struct cache_rule_stash), MEM_F_SHARED);
        global.cache.pool.ctx     = create_pool("cp.ctx", sizeof(struct cache_ctx), MEM_F_SHARED);

        if(global.cache.share) {
            global.cache.memory = nuster_memory_create("cache.shm", global.cache.dict_size + global.cache.data_size, global.tune.bufsize, CACHE_DEFAULT_CHUNK_SIZE);
            if(!global.cache.memory) {
                goto shm_err;
            }
            if(!nuster_shctx_init(global.cache.memory)) {
                goto shm_err;
            }
            cache = nuster_memory_alloc(global.cache.memory, sizeof(struct cache));
        } else {
            global.cache.memory = nuster_memory_create("cache.shm", CACHE_DEFAULT_SIZE, 0, 0);
            if(!global.cache.memory) {
                goto shm_err;
            }
            if(!nuster_shctx_init(global.cache.memory)) {
                goto shm_err;
            }
            global.cache.pool.data    = create_pool("cp.data", sizeof(struct cache_data), MEM_F_SHARED);
            global.cache.pool.element = create_pool("cp.element", sizeof(struct cache_element), MEM_F_SHARED);
            global.cache.pool.chunk   = create_pool("cp.chunk", global.tune.bufsize, MEM_F_SHARED);
            global.cache.pool.entry   = create_pool("cp.entry", sizeof(struct cache_entry), MEM_F_SHARED);

            cache = malloc(sizeof(struct cache));
        }
        if(!cache) {
            goto err;
        }
        cache->dict[0].entry = NULL;
        cache->dict[0].used  = 0;
        cache->dict[1].entry = NULL;
        cache->dict[1].used  = 0;
        cache->data_head     = NULL;
        cache->data_tail     = NULL;
        cache->rehash_idx    = -1;
        cache->cleanup_idx   = 0;

        if(!nuster_shctx_init(cache)) {
            goto shm_err;
        }

        if(!cache_dict_init()) {
            goto err;
        }

        if(!cache_stats_init()) {
            goto err;
        }

        for (i = 0; i < NUSTER_CACHE_MSG_SIZE; i++) {
            cache_msg_chunks[i].str = (char *)cache_msgs[i];
            cache_msg_chunks[i].len = strlen(cache_msgs[i]);
        }

        /* init cache rule */
        i = 0;
        p = proxy;
        while(p) {
            struct cache_rule *rule = NULL;
            uint32_t ttl;

            list_for_each_entry(rule, &p->cache_rules, list) {
                struct proxy *pt = proxy;

                rule->state  = nuster_memory_alloc(global.cache.memory, sizeof(*rule->state));
                if(!rule->state) {
                    goto err;
                }
                *rule->state = CACHE_RULE_ENABLED;
                ttl          = *rule->ttl;
                free(rule->ttl);
                rule->ttl    = nuster_memory_alloc(global.cache.memory, sizeof(*rule->ttl));
                if(!rule->ttl) {
                    goto err;
                }
                *rule->ttl   = ttl;

                while(pt) {
                    struct cache_rule *rt = NULL;
                    list_for_each_entry(rt, &pt->cache_rules, list) {
                        if(rt == rule) goto out;
                        if(!strcmp(rt->name, rule->name)) {
                            Alert("cache-rule with same name=[%s] found.\n", rule->name);
                            rule->id = rt->id;
                            goto out;
                        }
                    }
                    pt = pt->next;
                }

out:
                if(rule->id == -1) {
                    rule->id = i++;
                }
            }
            p = p->next;
        }

        cache_debug("[CACHE] on, data_size=%llu\n", global.cache.data_size);
    }
    return;
err:
    Alert("Out of memory when initializing cache.\n");
    exit(1);
shm_err:
    Alert("Error when initializing cache.\n");
    exit(1);
}

void cache_prebuild_key(struct cache_ctx *ctx, struct stream *s, struct http_msg *msg) {

    struct http_txn *txn = s->txn;

    char *url_end;
    struct hdr_ctx hdr;

    ctx->req.scheme = SCH_HTTP;
#ifdef USE_OPENSSL
    if(s->sess->listener->xprt == &ssl_sock) {
        ctx->req.scheme = SCH_HTTPS;
    }
#endif

    ctx->req.host.data = NULL;
    ctx->req.host.len  = 0;
    hdr.idx  = 0;
    if(http_find_header2("Host", 4, msg->chn->buf->p, &txn->hdr_idx, &hdr)) {
        ctx->req.host.data = hdr.line + hdr.val;
        ctx->req.host.len  = hdr.vlen;
    }

    ctx->req.path.data = http_get_path(txn);
    ctx->req.path.len  = 0;
    ctx->req.uri.data  = ctx->req.path.data;
    ctx->req.uri.len   = 0;
    url_end  = NULL;
    if(ctx->req.path.data) {
        char *ptr = ctx->req.path.data;
        url_end   = msg->chn->buf->p + msg->sl.rq.u + msg->sl.rq.u_l;
        while(ptr < url_end && *ptr != '?') {
            ptr++;
        }
        ctx->req.path.len = ptr - ctx->req.path.data;
        ctx->req.uri.len  = url_end - ctx->req.uri.data;
    }

    ctx->req.query.data = NULL;
    ctx->req.query.len  = 0;
    ctx->req.delimiter  = 0;
    if(ctx->req.path.data) {
        ctx->req.query.data = memchr(ctx->req.path.data, '?', url_end - ctx->req.path.data);
        if(ctx->req.query.data) {
            ctx->req.query.data++;
            ctx->req.query.len = url_end - ctx->req.query.data;
            if(ctx->req.query.len) {
                ctx->req.delimiter = 1;
            }
        }
    }

    hdr.idx    = 0;
    ctx->req.cookie.data = NULL;
    ctx->req.cookie.len  = 0;
    if(http_find_header2("Cookie", 6, msg->chn->buf->p, &txn->hdr_idx, &hdr)) {
        ctx->req.cookie.data = hdr.line + hdr.val;
        ctx->req.cookie.len  = hdr.vlen;
    }

}

char *cache_build_key(struct cache_ctx *ctx, struct cache_key **pck, struct stream *s,
        struct http_msg *msg) {

    struct http_txn *txn = s->txn;

    struct hdr_ctx hdr;

    struct cache_key *ck = NULL;
    int key_len          = 0;
    int key_size         = CACHE_DEFAULT_KEY_SIZE;
    char *key            = malloc(key_size);
    if(!key) {
        return NULL;
    }

    cache_debug("[CACHE] Calculate key: ");
    while((ck = *pck++)) {
        switch(ck->type) {
            case CK_METHOD:
                cache_debug("method.");
                key = _cache_key_append(key, &key_len, &key_size, http_known_methods[txn->meth].name, strlen(http_known_methods[txn->meth].name));
                break;
            case CK_SCHEME:
                cache_debug("scheme.");
                key = _cache_key_append(key, &key_len, &key_size, ctx->req.scheme == SCH_HTTPS ? "HTTPS" : "HTTP", ctx->req.scheme == SCH_HTTPS ? 5 : 4);
                break;
            case CK_HOST:
                cache_debug("host.");
                if(ctx->req.host.data) {
                    key = _cache_key_append(key, &key_len, &key_size, ctx->req.host.data, ctx->req.host.len);
                }
                break;
            case CK_URI:
                cache_debug("uri.");
                if(ctx->req.uri.data) {
                    key = _cache_key_append(key, &key_len, &key_size, ctx->req.uri.data, ctx->req.uri.len);
                }
                break;
            case CK_PATH:
                cache_debug("path.");
                if(ctx->req.path.data) {
                    key = _cache_key_append(key, &key_len, &key_size, ctx->req.path.data, ctx->req.path.len);
                }
                break;
            case CK_DELIMITER:
                cache_debug("delimiter.");
                key = _cache_key_append(key, &key_len, &key_size, ctx->req.delimiter ? "?": "", ctx->req.delimiter);
                break;
            case CK_QUERY:
                cache_debug("query.");
                if(ctx->req.query.data && ctx->req.query.len) {
                    key = _cache_key_append(key, &key_len, &key_size, ctx->req.query.data, ctx->req.query.len);
                }
                break;
            case CK_PARAM:
                cache_debug("param_%s.", ck->data);
                if(ctx->req.query.data && ctx->req.query.len) {
                    char *v = NULL;
                    int v_l = 0;
                    if(_cache_find_param_value_by_name(ctx->req.query.data, ctx->req.query.data + ctx->req.query.len, ck->data, &v, &v_l)) {
                        key = _cache_key_append(key, &key_len, &key_size, v, v_l);
                    }

                }
                break;
            case CK_HEADER:
                hdr.idx = 0;
                cache_debug("header_%s.", ck->data);
                if(http_find_header2(ck->data, strlen(ck->data), msg->chn->buf->p, &txn->hdr_idx, &hdr)) {
                    key = _cache_key_append(key, &key_len, &key_size, hdr.line + hdr.val, hdr.vlen);
                }
                break;
            case CK_COOKIE:
                cache_debug("header_%s.", ck->data);
                if(ctx->req.cookie.data) {
                    char *v = NULL;
                    int v_l = 0;
                    if(extract_cookie_value(ctx->req.cookie.data, ctx->req.cookie.data + ctx->req.cookie.len, ck->data, strlen(ck->data), 1, &v, &v_l)) {
                        key = _cache_key_append(key, &key_len, &key_size, v, v_l);
                    }
                }
                break;
            case CK_BODY:
                cache_debug("body.");
                if(txn->meth == HTTP_METH_POST || txn->meth == HTTP_METH_PUT) {
                    if((s->be->options & PR_O_WREQ_BODY) && msg->body_len > 0 ) {
                        key = _cache_key_append(key, &key_len, &key_size, msg->chn->buf->p + msg->sov, msg->body_len);
                    }
                }
                break;
            default:
                break;
        }
        if(!key) return NULL;
    }
    cache_debug("\n");
    return key;
}

uint64_t cache_hash_key(const char *key) {
    return XXH64(key, strlen(key), 0);
}

/*
 * Check if valid cache exists
 */
struct cache_data *cache_exists(const char *key, uint64_t hash) {
    struct cache_entry *entry = NULL;
    struct cache_data  *data  = NULL;

    if(!key) return NULL;

    nuster_shctx_lock(&cache->dict[0]);
    entry = cache_dict_get(key, hash);
    if(entry && entry->state == CACHE_ENTRY_STATE_VALID) {
        data = entry->data;
        data->clients++;
    }
    nuster_shctx_unlock(&cache->dict[0]);

    return data;
}

/*
 * Start to create cache,
 * if cache does not exist, add a new cache_entry
 * if cache exists but expired, add a new cache_data to the entry
 * otherwise, set the corresponding state: bypass, wait
 */
void cache_create(struct cache_ctx *ctx, char *key, uint64_t hash) {
    struct cache_entry *entry = NULL;

    /* Check if cache is full */
    if(cache_stats_full()) {
        ctx->state = CACHE_CTX_STATE_FULL;
        return;
    }

    nuster_shctx_lock(&cache->dict[0]);
    entry = cache_dict_get(key, hash);
    if(entry) {
        if(entry->state == CACHE_ENTRY_STATE_CREATING) {
            ctx->state = CACHE_CTX_STATE_WAIT;
        } else if(entry->state == CACHE_ENTRY_STATE_VALID) {
            ctx->state = CACHE_CTX_STATE_HIT;
        } else if(entry->state == CACHE_ENTRY_STATE_EXPIRED || entry->state == CACHE_ENTRY_STATE_INVALID) {
            entry->state = CACHE_ENTRY_STATE_CREATING;
            entry->data = cache_data_new();
            if(!entry->data) {
                ctx->state = CACHE_CTX_STATE_BYPASS;
                return;
            }
            ctx->state = CACHE_CTX_STATE_CREATE;
        } else {
            ctx->state = CACHE_CTX_STATE_BYPASS;
        }
    } else {
        entry = cache_dict_set(key, hash, ctx);
        if(entry) {
            ctx->state = CACHE_CTX_STATE_CREATE;
        } else {
            ctx->state = CACHE_CTX_STATE_BYPASS;
            return;
        }
    }
    nuster_shctx_unlock(&cache->dict[0]);
    ctx->entry   = entry;
    ctx->data    = entry->data;
    ctx->element = entry->data->element;
}

/*
 * Add partial http data to cache_data
 */
int cache_update(struct cache_ctx *ctx, struct http_msg *msg, long msg_len) {
    struct cache_element *element = cache_data_append(ctx->element, msg, msg_len);

    if(element) {
        if(!ctx->element) {
            ctx->data->element = element;
        }
        ctx->element = element;
        return 1;
    } else {
        return 0;
    }
}

/*
 * cache done
 */
void cache_finish(struct cache_ctx *ctx) {
    ctx->state = CACHE_CTX_STATE_DONE;
    ctx->entry->state = CACHE_ENTRY_STATE_VALID;
    if(*ctx->rule->ttl == 0) {
        ctx->entry->expire = 0;
    } else {
        ctx->entry->expire = get_current_timestamp_s() + *ctx->rule->ttl;
    }
}

void cache_abort(struct cache_ctx *ctx) {
    ctx->entry->state = CACHE_ENTRY_STATE_INVALID;
}

/*
 * Create cache applet to handle the request
 */
void cache_hit(struct stream *s, struct stream_interface *si, struct channel *req,
        struct channel *res, struct cache_data *data) {

    struct appctx *appctx = NULL;

    /*
     * set backend to cache_io_applet
     */
    s->target = &cache_io_applet.obj_type;
    if(unlikely(!stream_int_register_handler(si, objt_applet(s->target)))) {
        /* return to regular process on error */
        data->clients--;
        s->target = NULL;
    } else {
        appctx = si_appctx(si);
        memset(&appctx->ctx.cache, 0, sizeof(appctx->ctx.cache));
        appctx->ctx.cache.data    = data;
        appctx->ctx.cache.element = data->element;

        req->analysers &= ~AN_REQ_FLT_HTTP_HDRS;
        req->analysers &= ~AN_REQ_FLT_XFER_DATA;

        req->analysers |= AN_REQ_FLT_END;
        req->analyse_exp = TICK_ETERNITY;

        res->flags |= CF_NEVER_WAIT;
    }
}

/*
 * The cache applet acts like the backend to send cached http data
 */
static void cache_io_handler(struct appctx *appctx) {
    struct stream_interface *si   = appctx->owner;
    struct channel *res           = si_ic(si);
    struct stream *s              = si_strm(si);
    struct cache_element *element = NULL;
    int ret;

    if(appctx->ctx.cache.element) {
        if(appctx->ctx.cache.element == appctx->ctx.cache.data->element) {
            s->res.analysers = 0;
            s->res.analysers |= (AN_RES_WAIT_HTTP | AN_RES_HTTP_PROCESS_BE | AN_RES_HTTP_XFER_BODY);
        }
        element = appctx->ctx.cache.element;

        ret = bi_putblk(res, element->msg, element->msg_len);
        if(ret >= 0) {
            appctx->ctx.cache.element = element->next;
        } else if(ret == -2) {
            appctx->ctx.cache.data->clients--;
            si_shutr(si);
            res->flags |= CF_READ_NULL;
        }
    } else {
        bo_skip(si_oc(si), si_ob(si)->o);
        si_shutr(si);
        res->flags |= CF_READ_NULL;
        appctx->ctx.cache.data->clients--;
    }
}

void cache_debug(const char *fmt, ...) {
    if((global.mode & MODE_DEBUG)) {
        va_list args;
        va_start(args, fmt);
        vfprintf(stderr, fmt, args);
        va_end(args);
    }
}

char *cache_purge_build_key(struct stream *s, struct http_msg *msg) {

    struct http_txn *txn = s->txn;

    int https;
    char *path_beg, *url_end;
    struct hdr_ctx ctx;

    int key_len  = 0;

    /* "method.scheme.host.path.query.body" */
    int key_size = CACHE_DEFAULT_KEY_SIZE;
    char *key    = malloc(key_size);
    if(!key) {
        return NULL;
    }

    key = _cache_key_append(key, &key_len, &key_size, "GET", 3);

    https = 0;
#ifdef USE_OPENSSL
    if(s->sess->listener->xprt == &ssl_sock) {
        https = 1;
    }
#endif

    key = _cache_key_append(key, &key_len, &key_size, https ? "HTTPS": "HTTP", strlen(https ? "HTTPS": "HTTP"));
    if(!key) return NULL;

    ctx.idx  = 0;
    if(http_find_header2("Host", 4, msg->chn->buf->p, &txn->hdr_idx, &ctx)) {
        key = _cache_key_append(key, &key_len, &key_size, ctx.line + ctx.val, ctx.vlen);
        if(!key) return NULL;
    }

    path_beg = http_get_path(txn);
    url_end  = NULL;
    if(path_beg) {
        url_end = msg->chn->buf->p + msg->sl.rq.u + msg->sl.rq.u_l;
        key     = _cache_key_append(key, &key_len, &key_size, path_beg, url_end - path_beg);
        if(!key) return NULL;
    }
    return key;
}

/*
 * purge cache by key
 */
int cache_purge_by_key(const char *key, uint64_t hash) {
    struct cache_entry *entry = NULL;
    int ret;

    nuster_shctx_lock(&cache->dict[0]);
    entry = cache_dict_get(key, hash);
    if(entry && entry->state == CACHE_ENTRY_STATE_VALID) {
        entry->state         = CACHE_ENTRY_STATE_EXPIRED;
        entry->data->invalid = 1;
        entry->data          = NULL;
        entry->expire        = 0;
        ret                  = 200;
    } else {
        ret = 404;
    }
    nuster_shctx_unlock(&cache->dict[0]);

    return ret;
}

void cache_response(struct stream *s, struct chunk *msg) {
    s->txn->flags &= ~TX_WAIT_NEXT_RQ;
    stream_int_retnclose(&s->si[0], msg);
    if(!(s->flags & SF_ERR_MASK)) {
        s->flags |= SF_ERR_LOCAL;
    }
}

int cache_purge(struct stream *s, struct channel *req, struct proxy *px) {
    struct http_txn *txn = s->txn;
    struct http_msg *msg = &txn->req;


    if(txn->meth == HTTP_METH_OTHER &&
            memcmp(msg->chn->buf->p, global.cache.purge_method, strlen(global.cache.purge_method)) == 0) {

        char *key = cache_purge_build_key(s, msg);
        if(!key) {
            txn->status = 500;
            cache_response(s, &cache_msg_chunks[NUSTER_CACHE_500]);
        } else {
            uint64_t hash = cache_hash_key(key);
            txn->status = cache_purge_by_key(key, hash);
            if(txn->status == 200) {
                cache_response(s, &cache_msg_chunks[NUSTER_CACHE_200]);
            } else {
                cache_response(s, &cache_msg_chunks[NUSTER_CACHE_404]);
            }
        }
        return 1;
    }
    return 0;
}

struct applet cache_io_applet = {
    .obj_type = OBJ_TYPE_APPLET,
    .name = "<CACHE>",
    .fct = cache_io_handler,
    .release = NULL,
};

int cache_manager_state_ttl(struct stream *s, struct channel *req, struct proxy *px, int state, int ttl) {
    struct http_txn *txn = s->txn;
    struct http_msg *msg = &txn->req;
    int found, mode      = NUSTER_CACHE_PURGE_MODE_NAME_RULE;
    struct hdr_ctx ctx;
    struct proxy *p;

    if(state == -1 && ttl == -1) {
        return 400;
    }

    ctx.idx = 0;
    if(http_find_header2("name", 4, msg->chn->buf->p, &txn->hdr_idx, &ctx)) {
        if(ctx.vlen == 1 && !memcmp(ctx.line + ctx.val, "*", 1)) {
            found = 1;
            mode  = NUSTER_CACHE_PURGE_MODE_NAME_ALL;
        }
        p = proxy;
        while(p) {
            struct cache_rule *rule = NULL;

            if(mode != NUSTER_CACHE_PURGE_MODE_NAME_ALL && strlen(p->id) == ctx.vlen && !memcmp(ctx.line + ctx.val, p->id, ctx.vlen)) {
                found = 1;
                mode  = NUSTER_CACHE_PURGE_MODE_NAME_PROXY;
            }

            list_for_each_entry(rule, &p->cache_rules, list) {
                if(mode != NUSTER_CACHE_PURGE_MODE_NAME_RULE) {
                    *rule->state = state == -1 ? *rule->state : state;
                    *rule->ttl   = ttl   == -1 ? *rule->ttl   : ttl;
                } else if(strlen(rule->name) == ctx.vlen && !memcmp(ctx.line + ctx.val, rule->name, ctx.vlen)) {
                    *rule->state = state == -1 ? *rule->state : state;
                    *rule->ttl   = ttl   == -1 ? *rule->ttl   : ttl;
                    found        = 1;
                }
            }
            if(mode == NUSTER_CACHE_PURGE_MODE_NAME_PROXY) {
                break;
            }
            p = p->next;
        }
        if(found) {
            return 200;
        } else {
            return 404;
        }
    }

    return 400;
}

static inline int cache_manager_purge_method(struct http_txn *txn, struct http_msg *msg) {
    return txn->meth == HTTP_METH_OTHER &&
            memcmp(msg->chn->buf->p, global.cache.purge_method, strlen(global.cache.purge_method)) == 0;
}

static inline int cache_manager_uri(struct http_msg *msg) {
    const char *uri      = msg->chn->buf->p + msg->sl.rq.u;

    if(!global.cache.manager_uri) {
        return 0;
    }

    if(strlen(global.cache.manager_uri) != msg->sl.rq.u_l) {
        return 0;
    }

    if(memcmp(uri, global.cache.manager_uri, msg->sl.rq.u_l) != 0) {
        return 0;
    }

    return 1;
}

int cache_manager_purge(struct stream *s, struct channel *req, struct proxy *px) {
    struct stream_interface *si = &s->si[1];
    struct http_txn *txn        = s->txn;
    struct http_msg *msg        = &txn->req;
    struct appctx *appctx       = NULL;
    int mode                    = NUSTER_CACHE_PURGE_MODE_NAME_RULE;
    int st1                     = 0;
    struct hdr_ctx ctx;
    struct proxy *p;

    ctx.idx = 0;
    if(http_find_header2("name", 4, msg->chn->buf->p, &txn->hdr_idx, &ctx)) {
        if(ctx.vlen == 1 && !memcmp(ctx.line + ctx.val, "*", 1)) {
            mode = NUSTER_CACHE_PURGE_MODE_NAME_ALL;
            goto purge;
        }
        p = proxy;
        while(p) {
            struct cache_rule *rule = NULL;

            if(mode != NUSTER_CACHE_PURGE_MODE_NAME_ALL && strlen(p->id) == ctx.vlen && !memcmp(ctx.line + ctx.val, p->id, ctx.vlen)) {
                mode = NUSTER_CACHE_PURGE_MODE_NAME_PROXY;
                st1  = p->uuid;
                goto purge;
            }

            list_for_each_entry(rule, &p->cache_rules, list) {
                if(strlen(rule->name) == ctx.vlen && !memcmp(ctx.line + ctx.val, rule->name, ctx.vlen)) {
                    mode = NUSTER_CACHE_PURGE_MODE_NAME_RULE;
                    st1  = rule->id;
                    goto purge;
                }
            }
            p = p->next;
        }
        return 404;
    }
    return 400;

purge:
    s->target = &cache_manager_applet.obj_type;
    if(unlikely(!stream_int_register_handler(si, objt_applet(s->target)))) {
        return 500;
    } else {
        appctx      = si_appctx(si);
        appctx->st0 = mode;
        appctx->st1 = st1;
        appctx->st2 = 0;

        req->analysers &= (AN_REQ_HTTP_BODY | AN_REQ_FLT_HTTP_HDRS | AN_REQ_FLT_END);
        req->analysers &= ~AN_REQ_FLT_XFER_DATA;
        req->analysers |= AN_REQ_HTTP_XFER_BODY;
    }
    return 0;
}

/*
 * return 1 if the request is done, otherwise 0
 */
int cache_manager(struct stream *s, struct channel *req, struct proxy *px) {
    struct http_txn *txn = s->txn;
    struct http_msg *msg = &txn->req;
    int state            = -1;
    int ttl              = -1;
    struct hdr_ctx ctx;

    if(global.cache.status != CACHE_STATUS_ON) {
        return 0;
    }

    if(txn->meth == HTTP_METH_POST) {
        /* POST */
        if(cache_manager_uri(msg)) {
            /* manager_uri */
            ctx.idx = 0;
            if(http_find_header2("state", 5, msg->chn->buf->p, &txn->hdr_idx, &ctx)) {
                if(ctx.vlen == 6 && !memcmp(ctx.line + ctx.val, "enable", 6)) {
                    state = CACHE_RULE_ENABLED;
                } else if(ctx.vlen == 7 && !memcmp(ctx.line + ctx.val, "disable", 7)) {
                    state = CACHE_RULE_DISABLED;
                }
            }
            ctx.idx = 0;
            if(http_find_header2("ttl", 3, msg->chn->buf->p, &txn->hdr_idx, &ctx)) {
                cache_parse_time(ctx.line + ctx.val, ctx.vlen, (unsigned *)&ttl);
            }

            txn->status = cache_manager_state_ttl(s, req, px, state, ttl);
        } else {
            return 0;
        }
    } else if(cache_manager_purge_method(txn, msg)) {
        /* purge */
        if(cache_manager_uri(msg)) {
            /* manager_uri */
            txn->status = cache_manager_purge(s, req, px);
            if(txn->status == 0) {
                return 0;
            }
        } else {
            /* single uri */
            return cache_purge(s, req, px);
        }
    } else {
        return 0;
    }

    switch(txn->status) {
        case 200:
            cache_response(s, &cache_msg_chunks[NUSTER_CACHE_200]);
            break;
        case 400:
            cache_response(s, &cache_msg_chunks[NUSTER_CACHE_400]);
            break;
        case 404:
            cache_response(s, &cache_msg_chunks[NUSTER_CACHE_404]);
            break;
        case 500:
            cache_response(s, &cache_msg_chunks[NUSTER_CACHE_500]);
            break;
        default:
            cache_response(s, &cache_msg_chunks[NUSTER_CACHE_400]);
    }
    return 1;
}


static void cache_manager_handler(struct appctx *appctx) {
    struct stream_interface *si   = appctx->owner;
    struct channel *res           = si_ic(si);
    struct stream *s              = si_strm(si);
    struct cache_entry *entry     = NULL;
    int max                       = 1000;
    uint64_t start                = get_current_timestamp();

    while(1) {
        nuster_shctx_lock(&cache->dict[0]);
        while(appctx->st2 < cache->dict[0].size && max--) {
            entry = cache->dict[0].entry[appctx->st2];
            while(entry) {
                if(entry->state == CACHE_ENTRY_STATE_VALID &&
                        (appctx->st0 == NUSTER_CACHE_PURGE_MODE_NAME_ALL ||
                        (appctx->st0 == NUSTER_CACHE_PURGE_MODE_NAME_PROXY && entry->pid == appctx->st1) ||
                        entry->rule->id == appctx->st1)) {
                    entry->state         = CACHE_ENTRY_STATE_INVALID;
                    entry->data->invalid = 1;
                    entry->data          = NULL;
                    entry->expire        = 0;
                }
                entry = entry->next;
            }
            appctx->st2++;
        }
        nuster_shctx_unlock(&cache->dict[0]);
        if(get_current_timestamp() - start > 1) break;
        max = 1000;
    }
    task_wakeup(s->task, TASK_WOKEN_OTHER);

    if(appctx->st2 == cache->dict[0].size) {
        bi_putblk(res, cache_msgs[NUSTER_CACHE_200], strlen(cache_msgs[NUSTER_CACHE_200]));
        bo_skip(si_oc(si), si_ob(si)->o);
        si_shutr(si);
        res->flags |= CF_READ_NULL;
    }
}

struct applet cache_manager_applet = {
    .obj_type = OBJ_TYPE_APPLET,
    .name = "<CACHE-MANAGER>",
    .fct = cache_manager_handler,
    .release = NULL,
};

__attribute__((constructor)) static void __cache_init(void) { }

