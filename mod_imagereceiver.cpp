#include <opencv2/opencv.hpp>

#include "httpd.h"
#include "http_config.h"
#include "http_protocol.h"
#include "apreq2/apreq_module_apache2.h"
#include "apreq2/apreq_util.h"
#include "apr_strings.h"

#include <string>
#include <vector>

extern "C" module AP_MODULE_DECLARE_DATA imagereceiver_module;

static int imagereceiver_handler(request_rec *r)
{
    if (strcmp(r->handler, "imagereceiver")) {
        return DECLINED;
    }

    apreq_param_t *param = apreq_body_get(apreq_handle_apache2(r), "upfile");
    if (param == NULL) {
        ap_rprintf(r, "NULL\n");
        return OK;
    } else if (param->upload == NULL) {
        ap_rprintf(r, "not upload\n");
        return OK;
    }

    apr_bucket_brigade *bb = apr_brigade_create(r->pool, r->connection->bucket_alloc);
    apreq_brigade_copy(bb, param->upload);
    std::vector<char> vec;
    for (apr_bucket *e = APR_BRIGADE_FIRST(bb); e != APR_BRIGADE_SENTINEL(bb); e = APR_BUCKET_NEXT(e)) {
        char *data;
        apr_size_t dlen;
        if (apr_bucket_read(e, (const char **)&data, &dlen, APR_BLOCK_READ) != APR_SUCCESS) {
            ap_rprintf(r, "bad bucket read\n");
            break;
        }

        const char *data_copied = apr_pstrmemdup(r->pool, data, dlen);
        vec.insert(vec.end(), data_copied, data_copied + dlen);
        apr_bucket_delete(e);
    }

    vec.push_back('\0');
    cv::Mat mat = cv::imdecode(cv::Mat(vec), CV_LOAD_IMAGE_COLOR);
    cv::imwrite("/tmp/a.jpg", mat);

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

