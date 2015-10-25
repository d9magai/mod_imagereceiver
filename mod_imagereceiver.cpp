#include "imagematcher/src/matSerialization.h"
#include "imagematcher/src/commons.h"
#include "imagematcher/src/s3Utils.h"

#include <opencv2/opencv.hpp>
#include <opencv2/features2d.hpp>

#include <httpd.h>
#include <http_config.h>
#include <http_protocol.h>
#include <http_log.h>
#include <apreq2/apreq_module_apache2.h>
#include <apreq2/apreq_util.h>
#include <apr_strings.h>
#include <json-c/json.h>

#include <sstream>
#include <string>
#include <vector>
#include <cstdlib>
#include <pqxx/pqxx>

extern "C" module AP_MODULE_DECLARE_DATA imagereceiver_module;

APLOG_USE_MODULE (imagereceiver);

cv::Mat convert_to_Mat(request_rec *r, apreq_param_t *param) {

    apr_bucket_brigade *bb = apr_brigade_create(r->pool, r->connection->bucket_alloc);
    apreq_brigade_copy(bb, param->upload);
    std::vector<char> vec;
    for (apr_bucket *e = APR_BRIGADE_FIRST(bb); e != APR_BRIGADE_SENTINEL(bb); e = APR_BUCKET_NEXT(e)) {
        const char *data;
        apr_size_t len;
        if (apr_bucket_read(e, &data, &len, APR_BLOCK_READ) != APR_SUCCESS) {
            throw "failed to read bucket";
        }

        const char *dup_data = apr_pstrmemdup(r->pool, data, len);
        vec.insert(vec.end(), dup_data, dup_data + len);
        apr_bucket_delete(e);
    }
    vec.push_back('\0');
    return cv::imdecode(cv::Mat(vec), CV_LOAD_IMAGE_COLOR);
}

static std::vector<cv::Mat> descriptor;
static std::vector<cv::Mat> getDescriptor(Aws::S3::S3Client s3client) {

    if (descriptor.empty()) {
        descriptor = d9magai::s3utils::getDescriptor(s3client, "mtg.d9magai.jp", "LEA/descriptor");
    }
    return descriptor;
}

static cv::Ptr<cv::FlannBasedMatcher> matcher;
static cv::Ptr<cv::FlannBasedMatcher> getMatcher(Aws::S3::S3Client s3client) {

    if (matcher.empty()) {
        cv::Ptr < cv::flann::IndexParams > indexParams = new cv::flann::LshIndexParams(12, 20, 2);
        matcher = cv::Ptr < cv::FlannBasedMatcher > (new cv::FlannBasedMatcher(indexParams));
        std::vector < cv::Mat > desc = getDescriptor(s3client);
        matcher->add(d9magai::s3utils::getDescriptor(s3client, "mtg.d9magai.jp", "LEA/descriptor"));
        matcher->train();
    }
    return matcher;
}

static int imagereceiver_handler(request_rec *r) {
    if (strcmp(r->handler, "imagereceiver")) {
        return DECLINED;
    }

    apreq_param_t *param = apreq_body_get(apreq_handle_apache2(r), "upfile");
    if (param == NULL) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, APLOG_MODULE_INDEX, r, "bad request");
        return HTTP_BAD_REQUEST;
    } else if (param->upload == NULL) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, APLOG_MODULE_INDEX, r, "not uploaded");
        return HTTP_BAD_REQUEST;
    }

    cv::Mat image;
    std::stringstream ss;
    std::stringstream s;
    try {
        auto start = std::chrono::system_clock::now();

        std::string aws_access_key_id = apr_table_get(r->subprocess_env, "AWS_ACCESS_KEY_ID");
        std::string aws_secret_access_key =apr_table_get(r->subprocess_env, "AWS_SECRET_ACCESS_KEY");
        Aws::StringStream ass;
        ass << aws_access_key_id;
        Aws::String accesskey = ass.str();
        ass.str("");
        ass << aws_secret_access_key;
        Aws::String secretkey = ass.str();
        Aws::S3::S3Client s3client = d9magai::s3utils::getS3client(accesskey, secretkey);
        image = convert_to_Mat(r, param);
        cv::Ptr < cv::FeatureDetector > d = cv::ORB::create();
        std::vector < cv::KeyPoint > kp;
        d->detect(image, kp);
        cv::Mat queryDesc;
        d->compute(image, kp, queryDesc);
        auto end = std::chrono::system_clock::now();       // 計測終了時刻を保存
        auto dur = end - start;        // 要した時間を計算
        auto msec = std::chrono::duration_cast < std::chrono::milliseconds > (dur).count();
        ss << "get image, and compute: " << msec << " milli sec \n";
        ap_log_rerror(APLOG_MARK, APLOG_ERR, APLOG_MODULE_INDEX, r, ss.str().c_str());
        ss.str("");
        ss.clear(std::stringstream::goodbit);

        start = std::chrono::system_clock::now();
        cv::Ptr < cv::FlannBasedMatcher > m = getMatcher(s3client);

        end = std::chrono::system_clock::now();       // 計測終了時刻を保存
        dur = end - start;        // 要した時間を計算
        msec = std::chrono::duration_cast < std::chrono::milliseconds > (dur).count();
        ss << "download descriptor: " << msec << " milli sec \n";
        ap_log_rerror(APLOG_MARK, APLOG_ERR, APLOG_MODULE_INDEX, r, ss.str().c_str());
        ss.str("");
        ss.clear(std::stringstream::goodbit);

        start = std::chrono::system_clock::now();

        std::vector < cv::DMatch > matches;
        m->match(queryDesc, matches);

        int votes[295] = { }; // 学習画像の投票箱
        // 投票数の多い画像のIDと特徴点の数を調査
        int maxImageId = -1;
        int maxVotes = 0;
        for (const cv::DMatch& match : matches) {
            if (match.distance < 45.0) {
                votes[match.imgIdx]++;
                if (votes[match.imgIdx] > maxVotes) {
                    maxImageId = match.imgIdx;        //マッチした特徴点を一番多く持つ学習画像のIDを記憶
                    maxVotes = votes[match.imgIdx];        //マッチした特徴点の数
                }
            }
        }

        if (static_cast<double>(maxVotes) / m->getTrainDescriptors()[maxImageId].rows < 0.05) {
            maxImageId = -1; // マッチした特徴点の数が全体の5%より少なければ、未検出とする
        }
        end = std::chrono::system_clock::now();       // 計測終了時刻を保存
        dur = end - start;        // 要した時間を計算
        msec = std::chrono::duration_cast < std::chrono::milliseconds > (dur).count();
        ss << "matching: " << msec << " milli sec \n";
        ap_log_rerror(APLOG_MARK, APLOG_ERR, APLOG_MODULE_INDEX, r, ss.str().c_str());
        ss.str("");
        ss.clear(std::stringstream::goodbit);

        start = std::chrono::system_clock::now();
        pqxx::connection conn("dbname=mtg user=d9magai host=127.0.0.1");
        pqxx::work T(conn);
        std::string sql = "SELECT name FROM cards  WHERE id = " + std::to_string(maxImageId + 1);
        pqxx::result R(T.exec(sql));

        for (pqxx::result::const_iterator c = R.begin(); c != R.end(); ++c) {
            s << c[0].as(std::string());
        }
        T.commit();
        end = std::chrono::system_clock::now();       // 計測終了時刻を保存
        dur = end - start;        // 要した時間を計算
        msec = std::chrono::duration_cast < std::chrono::milliseconds > (dur).count();
        ss << "get from db: " << msec << " milli sec \n";
        ap_log_rerror(APLOG_MARK, APLOG_ERR, APLOG_MODULE_INDEX, r, ss.str().c_str());
        ss.str("");
        ss.clear(std::stringstream::goodbit);

    } catch (char const *message) {
        ap_log_rerror(APLOG_MARK, APLOG_ERR, APLOG_MODULE_INDEX, r, message);
        return HTTP_INTERNAL_SERVER_ERROR;
    }
    json_object *jobj = json_object_new_object();
    json_object_object_add(jobj, "name", json_object_new_string(s.str().c_str()));
    ap_set_content_type(r, "application/json");
    ap_rprintf(r, json_object_to_json_string(jobj));

    return OK;
}

static void imagereceiver_register_hooks(apr_pool_t *p) {
    ap_hook_handler(imagereceiver_handler, NULL, NULL, APR_HOOK_MIDDLE);
}

module AP_MODULE_DECLARE_DATA imagereceiver_module = { STANDARD20_MODULE_STUFF, NULL, /* create per-dir    config structures */
NULL, /* merge  per-dir    config structures */
NULL, /* create per-server config structures */
NULL, /* merge  per-server config structures */
NULL, /* table of config file commands       */
imagereceiver_register_hooks /* register hooks                      */
};

