#include "httpd.h"
#include "http_config.h"
#include "http_protocol.h"
#include "apreq2/apreq_module_apache2.h"
#include "apreq2/apreq_util.h"
#include "apr_strings.h"

extern "C" module AP_MODULE_DECLARE_DATA imagereceiver_module;

static int imagereceiver_handler(request_rec *r)
{
    if (strcmp(r->handler, "imagereceiver")) {
        return DECLINED;
    }

    apreq_handle_t *req = apreq_handle_apache2(r);    
    apreq_param_t *param = apreq_body_get(req, "upfile");
    if (param == NULL) {
        ap_rprintf(r, "NULL\n");
        return OK;
    }
    else if (param->upload == NULL) {
        ap_rprintf(r, "not upload\n");
        return OK;
    }

    apr_bucket_brigade *bb = apr_brigade_create(r->pool, r->connection->bucket_alloc);
    apr_bucket_brigade *ret = apr_brigade_create(r->pool, r->connection->bucket_alloc);
    apr_bucket *e;
    apreq_brigade_copy(bb, param->upload);
    apr_size_t len = 0;
    for (e = APR_BRIGADE_FIRST(bb); e != APR_BRIGADE_SENTINEL(bb); e = APR_BUCKET_NEXT(e)) {
        char *data;
        apr_size_t dlen;

        if (apr_bucket_read(e, (const char **)&data, &dlen, APR_BLOCK_READ) != APR_SUCCESS) {
            ap_rprintf(r, "bad bucket read\n");
            break;
        }
        else {
            const char *data_copied = apr_pstrmemdup(r->pool, data, dlen);
            len += dlen;
            apr_bucket *bucket_copied = apr_bucket_transient_create(data_copied, dlen, r->connection->bucket_alloc);
            apr_bucket_delete(e);
            APR_BRIGADE_INSERT_TAIL(ret, bucket_copied);
        }
    }

    ap_set_content_type(r, "image/jpg");
    ap_set_content_length(r, len);
    ap_pass_brigade(r->output_filters, ret);
    return OK;
}

static void imagereceiver_register_hooks(apr_pool_t *p)
{
    ap_hook_handler(imagereceiver_handler, NULL, NULL, APR_HOOK_MIDDLE);
}

module AP_MODULE_DECLARE_DATA imagereceiver_module = {
    STANDARD20_MODULE_STUFF, 
    NULL,                  /* create per-dir    config structures */
    NULL,                  /* merge  per-dir    config structures */
    NULL,                  /* create per-server config structures */
    NULL,                  /* merge  per-server config structures */
    NULL,                  /* table of config file commands       */
    imagereceiver_register_hooks  /* register hooks                      */
};

