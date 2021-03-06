/*
 +----------------------------------------------------------------------+
 | Swoole                                                               |
 +----------------------------------------------------------------------+
 | This source file is subject to version 2.0 of the Apache license,    |
 | that is bundled with this package in the file LICENSE, and is        |
 | available through the world-wide-web at the following url:           |
 | http://www.apache.org/licenses/LICENSE-2.0.html                      |
 | If you did not receive a copy of the Apache2.0 license and are unable|
 | to obtain it through the world-wide-web, please send a note to       |
 | license@swoole.com so we can mail you a copy immediately.            |
 +----------------------------------------------------------------------+
 | Author: Fang  <coooold@live.com>                                     |
 | Author: Tianfeng Han  <mikan.tenny@gmail.com>                        |
 | Author: Yuanyi   Zhi  <syyuanyizhi@163.com>                          |
 +----------------------------------------------------------------------+
 */

#include "php_swoole.h"

#ifdef SW_COROUTINE
#include "swoole_http_client.h"
#include "swoole_coroutine.h"
#include <setjmp.h>

static swString *http_client_buffer;

static void http_client_coro_onReceive(swClient *cli, char *data, uint32_t length);
static void http_client_coro_onConnect(swClient *cli);
static void http_client_coro_onClose(swClient *cli);
static void http_client_coro_onError(swClient *cli);

static int http_client_coro_send_http_request(zval *zobject TSRMLS_DC);
static int http_client_coro_execute(zval *zobject, char *uri, zend_size_t uri_len TSRMLS_DC);

static void http_client_coro_onTimeout(php_context *cxt);

static sw_inline void client_free_php_context(zval *object)
{
    //free memory
    php_context *context = swoole_get_property(object, 1);
    if (!context)
    {
        return;
    }

    if (likely(context->state == SW_CORO_CONTEXT_RUNNING))
    {
        efree(context);
    }
    else
    {
        context->state = SW_CORO_CONTEXT_TERM;
    }
    swoole_set_property(object, 1, NULL);
}

static const php_http_parser_settings http_parser_settings =
{
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    http_client_parser_on_header_field,
    http_client_parser_on_header_value,
    http_client_parser_on_headers_complete,
    http_client_parser_on_body,
    http_client_parser_on_message_complete
};

zend_class_entry swoole_http_client_coro_ce;
zend_class_entry *swoole_http_client_coro_class_entry_ptr;

static PHP_METHOD(swoole_http_client_coro, __construct);
static PHP_METHOD(swoole_http_client_coro, __destruct);
static PHP_METHOD(swoole_http_client_coro, set);
static PHP_METHOD(swoole_http_client_coro, setMethod);
static PHP_METHOD(swoole_http_client_coro, setHeaders);
static PHP_METHOD(swoole_http_client_coro, setCookies);
static PHP_METHOD(swoole_http_client_coro, setData);
static PHP_METHOD(swoole_http_client_coro, addFile);
static PHP_METHOD(swoole_http_client_coro, execute);
static PHP_METHOD(swoole_http_client_coro, isConnected);
static PHP_METHOD(swoole_http_client_coro, close);
static PHP_METHOD(swoole_http_client_coro, get);
static PHP_METHOD(swoole_http_client_coro, post);
static PHP_METHOD(swoole_http_client_coro, setDefer);
static PHP_METHOD(swoole_http_client_coro, getDefer);
static PHP_METHOD(swoole_http_client_coro, recv);

static const zend_function_entry swoole_http_client_coro_methods[] =
{
    PHP_ME(swoole_http_client_coro, __construct, NULL, ZEND_ACC_PUBLIC | ZEND_ACC_CTOR)
    PHP_ME(swoole_http_client_coro, __destruct, NULL, ZEND_ACC_PUBLIC | ZEND_ACC_DTOR)
    PHP_ME(swoole_http_client_coro, set, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_client_coro, setMethod, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_client_coro, setHeaders, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_client_coro, setCookies, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_client_coro, setData, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_client_coro, execute, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_client_coro, get, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_client_coro, post, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_client_coro, addFile, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_client_coro, isConnected, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_client_coro, close, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_client_coro, setDefer, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_client_coro, getDefer, NULL, ZEND_ACC_PUBLIC)
    PHP_ME(swoole_http_client_coro, recv, NULL, ZEND_ACC_PUBLIC)

    PHP_FE_END
};

static int http_client_coro_execute(zval *zobject, char *uri, zend_size_t uri_len TSRMLS_DC)
{
    if (uri_len <= 0)
    {
        swoole_php_fatal_error(E_WARNING, "path is empty.");
        return SW_ERR;
    }

    http_client *http = swoole_get_object(zobject);

    //http is not null when keeping alive
    if (http)
    {
        //http not ready
        if (http->state != HTTP_CLIENT_STATE_READY)
        {
            //swWarn("fd=%d, state=%d, active=%d, keep_alive=%d", http->cli->socket->fd, http->state, http->cli->socket->active, http->keep_alive);
            swoole_php_fatal_error(E_WARNING, "Operation now in progress phase %d.", http->state);
            return SW_ERR;
        }
        else if (!http->cli->socket->active)
        {
            swoole_php_fatal_error(E_WARNING, "connection#%d is closed.", http->cli->socket->fd);
            return SW_ERR;
        }
    }
    else
    {
        php_swoole_check_reactor();
        http = http_client_create(zobject TSRMLS_CC);
    }

    if (http == NULL)
    {
        return SW_ERR;
    }

    if (http->body == NULL)
    {
        http->body = swString_new(SW_HTTP_RESPONSE_INIT_SIZE);
        if (http->body == NULL)
        {
            swoole_php_fatal_error(E_ERROR, "[1] swString_new(%d) failed.", SW_HTTP_RESPONSE_INIT_SIZE);
            return SW_ERR;
        }
        http->header_field_buffer = swString_new(SW_HTTP_HEADER_BUFFER_SIZE);
        if (http->header_field_buffer == NULL)
        {
            swoole_php_fatal_error(E_ERROR, "[2] swString_new(%d) failed.", SW_HTTP_HEADER_BUFFER_SIZE);
            return SW_ERR;
        }
        http->header_value_buffer = swString_new(SW_HTTP_HEADER_BUFFER_SIZE);
        if (http->header_value_buffer == NULL)
        {
            swoole_php_fatal_error(E_ERROR, "[3] swString_new(%d) failed.", SW_HTTP_HEADER_BUFFER_SIZE);
            return SW_ERR;
        }
    }
    else
    {
        swString_clear(http->body);
    }

    if (http->uri)
    {
        efree(http->uri);
    }

    http->uri = estrdup(uri);
    http->uri_len = uri_len;
    //if connection exists
    if (http->cli)
    {
        http_client_coro_send_http_request(zobject TSRMLS_CC);

        return SW_OK;
    }


    swClient *cli = php_swoole_client_new(zobject, http->host, http->host_len, http->port);
    if (cli == NULL)
    {
        return SW_ERR;
    }
    http->cli = cli;



    zval *ztmp;
    HashTable *vht;
    zval *zset = sw_zend_read_property(swoole_http_client_coro_class_entry_ptr, zobject, ZEND_STRL("setting"), 1 TSRMLS_CC);
    if (zset && !ZVAL_IS_NULL(zset))
    {
        vht = Z_ARRVAL_P(zset);
        /**
         * timeout
         */
        if (php_swoole_array_get_value(vht, "timeout", ztmp))
        {
            convert_to_double(ztmp);
            http->timeout = (double) Z_DVAL_P(ztmp);
        }
        /**
         * keep_alive
         */
        if (php_swoole_array_get_value(vht, "keep_alive", ztmp))
        {
            convert_to_boolean(ztmp);
            http->keep_alive = (int) Z_LVAL_P(ztmp);
        }
        //client settings
        php_swoole_client_check_setting(http->cli, zset TSRMLS_CC);
    }



    if (cli->socket->active == 1)
    {
        swoole_php_fatal_error(E_WARNING, "swoole_http_client is already connected.");
        return SW_ERR;
    }

#if PHP_MAJOR_VERSION < 7
    sw_zval_add_ref(&zobject);
#endif
    cli->object = zobject;
	
#if PHP_MAJOR_VERSION >= 7
    http_client_property *hcc = swoole_get_property(zobject, 0);
    sw_copy_to_stack(cli->object, hcc->_object);
#endif
    cli->open_eof_check = 0;
    cli->open_length_check = 0;
    cli->reactor_fdtype = PHP_SWOOLE_FD_STREAM_CLIENT;
    cli->onReceive = http_client_coro_onReceive;
    cli->onConnect = http_client_coro_onConnect;
    cli->onClose = http_client_coro_onClose;
    cli->onError = http_client_coro_onError;

    return cli->connect(cli, http->host, http->port, http->timeout, 0);

}



static void http_client_coro_onTimeout(php_context *ctx)
{

#if PHP_MAJOR_VERSION < 7
    TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);
#endif
    zval *zdata;
    zval *retval = NULL;

    SW_MAKE_STD_ZVAL(zdata);
    ZVAL_BOOL(zdata, 0); //return false
#if PHP_MAJOR_VERSION < 7
    zval *zobject = (zval *)ctx->coro_params;
#else
    zval _zobject = ctx->coro_params;
    zval *zobject = & _zobject;
#endif
    //define time out RETURN ERROR  110
    zend_update_property_long(swoole_http_client_coro_class_entry_ptr, zobject, ZEND_STRL("errCode"), 110 TSRMLS_CC);

    http_client *http = swoole_get_object(zobject);
    http->cli->released = 1;
    http_client_free(zobject TSRMLS_CC);
    swoole_set_object(zobject, NULL);

    http_client_property *hcc = swoole_get_property(zobject, 0);
    if(hcc->defer && hcc->defer_status != HTTP_CLIENT_STATE_DEFER_WAIT){
        hcc->defer_status = HTTP_CLIENT_STATE_DEFER_DONE;
        hcc->defer_result = 0;
        goto free_zdata;
    }

    hcc->defer_status = HTTP_CLIENT_STATE_DEFER_INIT;
    int ret = coro_resume(ctx, zdata, &retval);
    if (ret > 0)
    {
        goto free_zdata;
    }
    if (retval != NULL)
    {
        sw_zval_ptr_dtor(&retval);
    }
free_zdata:
    sw_zval_ptr_dtor(&zdata);
}



void swoole_http_client_coro_init(int module_number TSRMLS_DC)
{
    SWOOLE_INIT_CLASS_ENTRY(swoole_http_client_coro_ce, "swoole_http_client_coro", "Swoole\\Coroutine\\Http\\Client", swoole_http_client_coro_methods);
    swoole_http_client_coro_class_entry_ptr = zend_register_internal_class(&swoole_http_client_coro_ce TSRMLS_CC);
    //todo search
    SWOOLE_CLASS_ALIAS(swoole_http_client_coro, "Swoole\\Coroutine\\Http\\Client");

    zend_declare_property_long(swoole_http_client_coro_class_entry_ptr, SW_STRL("errCode")-1, 0, ZEND_ACC_PUBLIC TSRMLS_CC);
    zend_declare_property_long(swoole_http_client_coro_class_entry_ptr, SW_STRL("sock")-1, 0, ZEND_ACC_PUBLIC TSRMLS_CC);

    http_client_buffer = swString_new(SW_HTTP_RESPONSE_INIT_SIZE);
    if (!http_client_buffer)
    {
        swoole_php_fatal_error(E_ERROR, "[1] swString_new(%d) failed.", SW_HTTP_RESPONSE_INIT_SIZE);
    }

#ifdef SW_HAVE_ZLIB
    swoole_zlib_buffer = swString_new(2048);
    if (!swoole_zlib_buffer)
    {
        swoole_php_fatal_error(E_ERROR, "[2] swString_new(%d) failed.", SW_HTTP_RESPONSE_INIT_SIZE);
    }
#endif
}

/**
 * @zobject: swoole_http_client_coro object
 */
static void http_client_coro_onClose(swClient *cli)
{
#if PHP_MAJOR_VERSION < 7
    TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);
#endif
    zval *zobject = cli->object;
    if (!cli->released)
    {
        http_client_free(zobject TSRMLS_CC);
    }
#if PHP_MAJOR_VERSION < 7
    sw_zval_ptr_dtor(&zobject);
#endif
    return;
}

/**
 * @zobject: swoole_http_client object
 */
static void http_client_coro_onError(swClient *cli)
{
#if PHP_MAJOR_VERSION < 7
    TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);
#endif
    zval *zdata;
    zval *retval = NULL;

    SW_MAKE_STD_ZVAL(zdata);
    //return false
    ZVAL_BOOL(zdata, 0);

    zval *zobject = cli->object;
    php_context *sw_current_context = swoole_get_property(zobject, 1);
    zend_update_property_long(swoole_http_client_coro_class_entry_ptr, zobject, ZEND_STRL("errCode"), SwooleG.error TSRMLS_CC);
    if (cli->timeout_id > 0)
    {
        php_swoole_clear_timer_coro(cli->timeout_id TSRMLS_CC);
        cli->timeout_id=0;
    }

    if (!cli->released)
    {
        http_client_free(zobject TSRMLS_CC);
    }
    swoole_set_object(zobject, NULL);

    http_client_property *hcc = swoole_get_property(zobject, 0);
    if(hcc->defer && hcc->defer_status != HTTP_CLIENT_STATE_DEFER_WAIT){
        hcc->defer_status = HTTP_CLIENT_STATE_DEFER_DONE;
        hcc->defer_result = 0;
        goto free_zdata;
    }

    hcc->defer_status = HTTP_CLIENT_STATE_DEFER_INIT;
    int ret = coro_resume(sw_current_context, zdata, &retval);
    if (ret > 0)
    {
        goto free_zdata;
    }
    if (retval != NULL)
    {
        sw_zval_ptr_dtor(&retval);
    }
free_zdata:
    sw_zval_ptr_dtor(&zdata);
}

static void http_client_coro_onReceive(swClient *cli, char *data, uint32_t length)
{
#if PHP_MAJOR_VERSION < 7
    TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);
#endif

    zval *zobject = cli->object;
    zval *retval = NULL;
    http_client *http = swoole_get_object(zobject);
    if (!http->cli)
    {
        swoole_php_fatal_error(E_WARNING, "object is not instanceof swoole_http_client_coro.");
        return;
    }
    //timeout
    if (cli->timeout_id > 0)
    {
        php_swoole_clear_timer_coro(cli->timeout_id TSRMLS_CC);
        cli->timeout_id=0;
    }

    long parsed_n = php_http_parser_execute(&http->parser, &http_parser_settings, data, length);

    http_client_property *hcc = swoole_get_property(zobject, 0);
    zval *zdata;
    SW_MAKE_STD_ZVAL(zdata);

    if (parsed_n < 0)
    {
        //错误情况 标志位 done defer 保存
        sw_zend_call_method_with_0_params(&zobject, swoole_http_client_coro_class_entry_ptr, NULL, "close", &retval);
        if (retval)
        {
            sw_zval_ptr_dtor(&retval);
        }
        ZVAL_BOOL(zdata, 0); //return false
        if (hcc->defer && hcc->defer_status != HTTP_CLIENT_STATE_DEFER_WAIT)
        {
            //not recv yet  sava data
            hcc->defer_status = HTTP_CLIENT_STATE_DEFER_DONE;
            hcc->defer_result = 0;
            goto free_zdata;
            //wait for recv
        }
        goto begin_resume;
    }

    //not complete
    if (!http->completed)
    {
        return;
    }


//    if(!hcc->defer_chunk_status){ //not recv all wait for next
//        return;
//    }

    ZVAL_BOOL(zdata, 1); //return false
    if (hcc->defer && hcc->defer_status != HTTP_CLIENT_STATE_DEFER_WAIT)
    {
        //not recv yet  sava data
        hcc->defer_status = HTTP_CLIENT_STATE_DEFER_DONE;
        hcc->defer_result = 1;
        goto free_zdata;
    }

    begin_resume:
    {
        //if should resume
        /*if next cr*/
        php_context *sw_current_context = swoole_get_property(zobject, 1);
        hcc->defer_status = HTTP_CLIENT_STATE_DEFER_INIT;
    //    hcc->defer_chunk_status = 0;
        http->completed = 0;

        int ret = coro_resume(sw_current_context, zdata, &retval);
        if (ret > 0)
        {
            goto free_zdata;
        }
        if (retval != NULL)
        {
            sw_zval_ptr_dtor(&retval);
        }
    }

    free_zdata:
    sw_zval_ptr_dtor(&zdata);
}


static void http_client_coro_onConnect(swClient *cli)
{
#if PHP_MAJOR_VERSION < 7
    TSRMLS_FETCH_FROM_CTX(sw_thread_ctx ? sw_thread_ctx : NULL);
#endif

    zval *zobject = cli->object;
    http_client *http = swoole_get_object(zobject);
    if (!http->cli)
    {
        swoole_php_fatal_error(E_WARNING, "object is not instanceof swoole_http_client_coro.");
        return;
    }
    http_client_coro_send_http_request(zobject TSRMLS_CC);
}


static int http_client_coro_send_http_request(zval *zobject TSRMLS_DC)
{

    int ret;
    http_client *http = swoole_get_object(zobject);
    if (!http->cli || !http->cli->socket )
    {
        swoole_php_fatal_error(E_WARNING, "object is not instanceof swoole_http_client.");
        return SW_ERR;
    }

    if (http->cli->socket->active == 0)
    {
        swoole_php_error(E_WARNING, "server is not connected.");
        return SW_ERR;
    }

    if (http->state != HTTP_CLIENT_STATE_READY)
    {
        swoole_php_error(E_WARNING, "http client is not ready.");
        return SW_ERR;
    }

    http->state = HTTP_CLIENT_STATE_BUSY;
    //clear errno
    SwooleG.error = 0;

    http_client_property *hcc = swoole_get_property(zobject, 0);

    zval *post_data = hcc->request_body;
    zval *send_header = hcc->request_header;

    //POST
    if (post_data)
    {
        if (hcc->request_method == NULL)
        {
            hcc->request_method = "POST";
        }
    }
    //GET
    else
    {
        if (hcc->request_method == NULL)
        {
            hcc->request_method = "GET";
        }
    }

    swString_clear(http_client_buffer);
    swString_append_ptr(http_client_buffer, hcc->request_method, strlen(hcc->request_method));
    hcc->request_method = NULL;
    swString_append_ptr(http_client_buffer, ZEND_STRL(" "));
    swString_append_ptr(http_client_buffer, http->uri, http->uri_len);
    swString_append_ptr(http_client_buffer, ZEND_STRL(" HTTP/1.1\r\n"));

    char *key;
    uint32_t keylen;
    int keytype;
    zval *value;

    if (send_header && Z_TYPE_P(send_header) == IS_ARRAY)
    {
        if (sw_zend_hash_find(Z_ARRVAL_P(send_header), ZEND_STRS("Connection"), (void **) &value) == FAILURE)
        {
            if (http->keep_alive)
            {
                http_client_swString_append_headers(http_client_buffer, ZEND_STRL("Connection"), ZEND_STRL("keep-alive"));
            }
            else
            {
                http_client_swString_append_headers(http_client_buffer, ZEND_STRL("Connection"), ZEND_STRL("closed"));
            }
        }

        if (sw_zend_hash_find(Z_ARRVAL_P(send_header), ZEND_STRS("Host"), (void **) &value) == FAILURE)
        {
            http_client_swString_append_headers(http_client_buffer, ZEND_STRL("Host"), http->host, http->host_len);
        }

#ifdef SW_HAVE_ZLIB
        if (sw_zend_hash_find(Z_ARRVAL_P(send_header), ZEND_STRS("Accept-Encoding"), (void **) &value) == FAILURE)
        {
            http_client_swString_append_headers(http_client_buffer, ZEND_STRL("Accept-Encoding"), ZEND_STRL("gzip"));
        }
#endif

        SW_HASHTABLE_FOREACH_START2(Z_ARRVAL_P(send_header), key, keylen, keytype, value)
        if (HASH_KEY_IS_STRING != keytype)
        {
            continue;
        }
        convert_to_string(value);
        http_client_swString_append_headers(http_client_buffer, key, keylen, Z_STRVAL_P(value), Z_STRLEN_P(value));
        SW_HASHTABLE_FOREACH_END();
    }
    else
    {
        http_client_swString_append_headers(http_client_buffer, ZEND_STRL("Connection"), ZEND_STRL("keep-alive"));
        http->keep_alive = 1;
        http_client_swString_append_headers(http_client_buffer, ZEND_STRL("Host"), http->host, http->host_len);
#ifdef SW_HAVE_ZLIB
        http_client_swString_append_headers(http_client_buffer, ZEND_STRL("Accept-Encoding"), ZEND_STRL("gzip"));
#endif
    }

    if (hcc->cookies && Z_TYPE_P(hcc->cookies) == IS_ARRAY)
    {
        swString_append_ptr(http_client_buffer, ZEND_STRL("Cookie: "));
        int n_cookie = Z_ARRVAL_P(hcc->cookies)->nNumOfElements;
        int i = 0;
        char *encoded_value;

        SW_HASHTABLE_FOREACH_START2(Z_ARRVAL_P(hcc->cookies), key, keylen, keytype, value)
        i ++;
        if (HASH_KEY_IS_STRING != keytype)
        {
            continue;
        }
        convert_to_string(value);
        swString_append_ptr(http_client_buffer, key, keylen);
        swString_append_ptr(http_client_buffer, "=", 1);

        int encoded_value_len;
        encoded_value = sw_php_url_encode( Z_STRVAL_P(value), Z_STRLEN_P(value), &encoded_value_len);
        if (encoded_value)
        {
            swString_append_ptr(http_client_buffer, encoded_value, encoded_value_len);
            efree(encoded_value);
        }
        if (i < n_cookie)
        {
            swString_append_ptr(http_client_buffer, "; ", 2);
        }
        SW_HASHTABLE_FOREACH_END();
        swString_append_ptr(http_client_buffer, ZEND_STRL("\r\n"));
    }

    //form-data
        if (hcc->request_upload_files)
        {
            char header_buf[2048];
            char boundary_str[39];
            int n;

            memcpy(boundary_str, SW_HTTP_CLIENT_BOUNDARY_PREKEY, sizeof(SW_HTTP_CLIENT_BOUNDARY_PREKEY) - 1);
            swoole_random_string(boundary_str + sizeof(SW_HTTP_CLIENT_BOUNDARY_PREKEY) - 1,
                    sizeof(boundary_str) - sizeof(SW_HTTP_CLIENT_BOUNDARY_PREKEY));

            n = snprintf(header_buf, sizeof(header_buf), "Content-Type: multipart/form-data; boundary=%*s\r\n",
                    sizeof(boundary_str) - 1, boundary_str);

            swString_append_ptr(http_client_buffer, header_buf, n);

            int content_length = 0;

            //post data
            if (Z_TYPE_P(post_data) == IS_ARRAY)
            {
                SW_HASHTABLE_FOREACH_START2(Z_ARRVAL_P(post_data), key, keylen, keytype, value)
                    if (HASH_KEY_IS_STRING != keytype)
                    {
                        continue;
                    }
                    convert_to_string(value);
                    //strlen("%.*")*2 = 6
                    //header + body + CRLF
                    content_length += (sizeof(SW_HTTP_FORM_DATA_FORMAT_STRING) - 7) + (sizeof(boundary_str) - 1) + keylen
                            + Z_STRLEN_P(value) + 2;
                SW_HASHTABLE_FOREACH_END();
            }

            zval *zname;
            zval *ztype;
            zval *zsize;
            zval *zpath;
            zval *zfilename;
            zval *zoffset;

            if (hcc->request_upload_files)
            {
                //upload files
                SW_HASHTABLE_FOREACH_START2(Z_ARRVAL_P(hcc->request_upload_files), key, keylen, keytype, value)
                    if (sw_zend_hash_find(Z_ARRVAL_P(value), ZEND_STRS("name"), (void **) &zname) == FAILURE)
                    {
                        continue;
                    }
                    if (sw_zend_hash_find(Z_ARRVAL_P(value), ZEND_STRS("filename"), (void **) &zfilename) == FAILURE)
                    {
                        continue;
                    }
                    if (sw_zend_hash_find(Z_ARRVAL_P(value), ZEND_STRS("size"), (void **) &zsize) == FAILURE)
                    {
                        continue;
                    }
                    if (sw_zend_hash_find(Z_ARRVAL_P(value), ZEND_STRS("type"), (void **) &ztype) == FAILURE)
                    {
                        continue;
                    }
                    //strlen("%.*")*4 = 12
                    //header + body + CRLF
                    content_length += (sizeof(SW_HTTP_FORM_DATA_FORMAT_FILE) - 13) + (sizeof(boundary_str) - 1)
                            + Z_STRLEN_P(zname) + Z_STRLEN_P(zfilename) + Z_STRLEN_P(ztype) + Z_LVAL_P(zsize) + 2;
                SW_HASHTABLE_FOREACH_END();
            }

            http_client_append_content_length(http_client_buffer, content_length + sizeof(boundary_str) - 1 + 6);

            //post data
            if (Z_TYPE_P(post_data) == IS_ARRAY)
            {
                SW_HASHTABLE_FOREACH_START2(Z_ARRVAL_P(post_data), key, keylen, keytype, value)
                    if (HASH_KEY_IS_STRING != keytype)
                    {
                        continue;
                    }
                    convert_to_string(value);
                    n = snprintf(header_buf, sizeof(header_buf), SW_HTTP_FORM_DATA_FORMAT_STRING, sizeof(boundary_str) - 1,
                            boundary_str, keylen, key);
                    swString_append_ptr(http_client_buffer, header_buf, n);
                    swString_append_ptr(http_client_buffer, Z_STRVAL_P(value), Z_STRLEN_P(value));
                    swString_append_ptr(http_client_buffer, ZEND_STRL("\r\n"));
                SW_HASHTABLE_FOREACH_END();

                zend_update_property_null(swoole_http_client_coro_class_entry_ptr, zobject, ZEND_STRL("requestBody") TSRMLS_CC);
                hcc->request_body = NULL;
            }

            if ((ret = http->cli->send(http->cli, http_client_buffer->str, http_client_buffer->length, 0)) < 0)
            {
                goto send_fail;
            }

            if (hcc->request_upload_files)
            {
                //upload files
                SW_HASHTABLE_FOREACH_START2(Z_ARRVAL_P(hcc->request_upload_files), key, keylen, keytype, value)
                    if (sw_zend_hash_find(Z_ARRVAL_P(value), ZEND_STRS("name"), (void **) &zname) == FAILURE)
                    {
                        continue;
                    }
                    if (sw_zend_hash_find(Z_ARRVAL_P(value), ZEND_STRS("filename"), (void **) &zfilename) == FAILURE)
                    {
                        continue;
                    }
                    if (sw_zend_hash_find(Z_ARRVAL_P(value), ZEND_STRS("path"), (void **) &zpath) == FAILURE)
                    {
                        continue;
                    }
                    if (sw_zend_hash_find(Z_ARRVAL_P(value), ZEND_STRS("type"), (void **) &ztype) == FAILURE)
                    {
                        continue;
                    }
                    if (sw_zend_hash_find(Z_ARRVAL_P(value), ZEND_STRS("offset"), (void **) &zoffset) == FAILURE)
                    {
                        continue;
                    }
                    n = snprintf(header_buf, sizeof(header_buf), SW_HTTP_FORM_DATA_FORMAT_FILE, sizeof(boundary_str) - 1,
                            boundary_str, Z_STRLEN_P(zname), Z_STRVAL_P(zname), Z_STRLEN_P(zfilename),
                            Z_STRVAL_P(zfilename), Z_STRLEN_P(ztype), Z_STRVAL_P(ztype));

                    if ((ret = http->cli->send(http->cli, header_buf, n, 0)) < 0)
                    {
                        goto send_fail;
                    }
                    if ((ret = http->cli->sendfile(http->cli, Z_STRVAL_P(zpath), Z_LVAL_P(zoffset), Z_LVAL_P(zsize))) < 0)
                    {
                        goto send_fail;
                    }
                    if ((ret = http->cli->send(http->cli, "\r\n", 2, 0)) < 0)
                    {
                        goto send_fail;
                    }
                SW_HASHTABLE_FOREACH_END();

                zend_update_property_null(swoole_http_client_coro_class_entry_ptr, zobject, ZEND_STRL("uploadFiles") TSRMLS_CC);
                hcc->request_upload_files = NULL;
            }

            n = snprintf(header_buf, sizeof(header_buf), "--%*s--\r\n", sizeof(boundary_str) - 1, boundary_str);
            if ((ret = http->cli->send(http->cli, header_buf, n, 0)) < 0)
            {
                goto send_fail;
            }
            else
            {
                return SW_OK;
            }
        }

    //x-www-form-urlencoded or raw

    else if (post_data)
    {

         if (Z_TYPE_P(post_data) == IS_ARRAY)
        {
            zend_size_t len;
            http_client_swString_append_headers(http_client_buffer, ZEND_STRL("Content-Type"), ZEND_STRL("application/x-www-form-urlencoded"));
            smart_str formstr_s = { 0 };
            char *formstr = sw_http_build_query(post_data, &len, &formstr_s TSRMLS_CC);
            if (formstr == NULL)
            {
                swoole_php_error(E_WARNING, "http_build_query failed.");
                return SW_ERR;
            }
            http_client_append_content_length(http_client_buffer, len);
            swString_append_ptr(http_client_buffer, formstr, len);
            smart_str_free(&formstr_s);
        }
        else
        {
            http_client_append_content_length(http_client_buffer, Z_STRLEN_P(post_data));
            swString_append_ptr(http_client_buffer, Z_STRVAL_P(post_data), Z_STRLEN_P(post_data));
        }
        zend_update_property_null(swoole_http_client_coro_class_entry_ptr, zobject, ZEND_STRL("requestBody") TSRMLS_CC);
        hcc->request_body = NULL;
    }
    else
    {
        swString_append_ptr(http_client_buffer, ZEND_STRL("\r\n"));
    }

    swTrace("[%d]: %s\n", (int)http_client_buffer->length, http_client_buffer->str);

   if ((ret = http->cli->send(http->cli, http_client_buffer->str, http_client_buffer->length, 0)) < 0)
       {
           send_fail:
           SwooleG.error = errno;
           swoole_php_sys_error(E_WARNING, "send(%d) %d bytes failed.", http->cli->socket->fd, (int )http_client_buffer->length);
           zend_update_property_long(swoole_http_client_coro_class_entry_ptr, zobject, SW_STRL("errCode")-1, SwooleG.error TSRMLS_CC);
       }
       return ret;
}

static PHP_METHOD(swoole_http_client_coro, __construct)
{
    coro_check(TSRMLS_C);

    char *host;
    zend_size_t host_len;
    long port = 80;
    zend_bool ssl = SW_FALSE;

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|lb", &host, &host_len, &port, &ssl) == FAILURE)
    {
        return;
    }

    if (host_len <= 0)
    {
        swoole_php_fatal_error(E_ERROR, "host is empty.");
        RETURN_FALSE;
    }

    zend_update_property_stringl(swoole_http_client_coro_class_entry_ptr, getThis(), ZEND_STRL("host"), host, host_len TSRMLS_CC);

    zend_update_property_long(swoole_http_client_coro_class_entry_ptr,getThis(), ZEND_STRL("port"), port TSRMLS_CC);

    //init
    swoole_set_object(getThis(), NULL);

    zval *headers;
    SW_MAKE_STD_ZVAL(headers);
    array_init(headers);
    zend_update_property(swoole_http_client_coro_class_entry_ptr, getThis(), ZEND_STRL("headers"), headers TSRMLS_CC);
    sw_zval_ptr_dtor(&headers);

    http_client_property *hcc;
    hcc = (http_client_property*) emalloc(sizeof(http_client_property));
    bzero(hcc, sizeof(http_client_property));
    hcc->defer_status = HTTP_CLIENT_STATE_DEFER_INIT;
   // hcc->defer_chunk_status = 0;
    swoole_set_property(getThis(), 0, hcc);

    int flags = SW_SOCK_TCP | SW_FLAG_ASYNC;
    if (ssl)
    {
#ifdef SW_USE_OPENSSL
        flags |= SW_SOCK_SSL;
#else
        swoole_php_fatal_error(E_ERROR, "require openssl library.");
#endif
    }

    zend_update_property_long(swoole_http_client_coro_class_entry_ptr, getThis(), ZEND_STRL("type"), flags TSRMLS_CC);

    php_context *context = swoole_get_property(getThis(), 1);
    if (!context)
    {
        context = emalloc(sizeof(php_context));
        swoole_set_property(getThis(), 1, context);
    }
    context->onTimeout = http_client_coro_onTimeout;
#if PHP_MAJOR_VERSION < 7
	context->coro_params = getThis();
#else
	context->coro_params = *getThis();
#endif
	context->state = SW_CORO_CONTEXT_RUNNING;

    RETURN_TRUE;
}



static PHP_METHOD(swoole_http_client_coro, __destruct)
{
    //free context
    client_free_php_context(getThis());
    http_client *http = swoole_get_object(getThis());
    if (http)
    {
        zval *zobject = getThis();
        zval *retval = NULL;
        sw_zend_call_method_with_0_params(&zobject, swoole_http_client_coro_class_entry_ptr, NULL, "close", &retval);
        if (retval)
        {
            sw_zval_ptr_dtor(&retval);
        }
    }
    http_client_property *hcc = swoole_get_property(getThis(), 0);
    efree(hcc);
    swoole_set_property(getThis(), 0, NULL);
}

static PHP_METHOD(swoole_http_client_coro, set)
{
    zval *zset;
    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &zset) == FAILURE)
    {
        return;
    }
    php_swoole_array_separate(zset);
    zend_update_property(swoole_http_client_coro_class_entry_ptr, getThis(), ZEND_STRL("setting"), zset TSRMLS_CC);
    RETURN_TRUE;
}

static PHP_METHOD(swoole_http_client_coro, setHeaders)
{
    zval *headers;
    if (zend_parse_parameters(ZEND_NUM_ARGS()TSRMLS_CC, "z", &headers) == FAILURE)
    {
        return;
    }
    zend_update_property(swoole_http_client_coro_class_entry_ptr, getThis(), ZEND_STRL("requestHeaders"), headers TSRMLS_CC);
    http_client_property *hcc = swoole_get_property(getThis(), 0);
    hcc->request_header = sw_zend_read_property(swoole_http_client_coro_class_entry_ptr, getThis(), ZEND_STRL("requestHeaders"), 1 TSRMLS_CC);
    sw_copy_to_stack(hcc->request_header, hcc->_request_header);
    RETURN_TRUE;
}

static PHP_METHOD(swoole_http_client_coro, setCookies)
{
    zval *cookies;
    if (zend_parse_parameters(ZEND_NUM_ARGS()TSRMLS_CC, "z", &cookies) == FAILURE)
    {
        return;
    }
    zend_update_property(swoole_http_client_coro_class_entry_ptr, getThis(), ZEND_STRL("cookies"), cookies TSRMLS_CC);
    http_client_property *hcc = swoole_get_property(getThis(), 0);
    hcc->cookies = sw_zend_read_property(swoole_http_client_coro_class_entry_ptr, getThis(), ZEND_STRL("cookies"), 1 TSRMLS_CC);
    sw_copy_to_stack(hcc->cookies, hcc->_cookies);

    RETURN_TRUE;
}

static PHP_METHOD(swoole_http_client_coro, getDefer)
{
    http_client_property *hcc = swoole_get_property(getThis(), 0);

    RETURN_BOOL(hcc->defer);
}

static PHP_METHOD(swoole_http_client_coro, setDefer)
{
    zend_bool defer = 1;
    http_client_property *hcc = swoole_get_property(getThis(), 0);

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|b", &defer) == FAILURE)
    {
        return;
    }

    if (hcc->defer_status != HTTP_CLIENT_STATE_DEFER_INIT)
    {
        RETURN_BOOL(defer);
    }

    hcc->defer = defer;

    RETURN_TRUE;
}

static PHP_METHOD(swoole_http_client_coro, recv)
{

    //todo
    http_client_property *hcc = swoole_get_property(getThis(), 0);

    if (!hcc->defer)
    {	//no defer
        swoole_php_fatal_error(E_WARNING, "you should not use recv without defer ");
        RETURN_FALSE;
    }


    switch (hcc->defer_status)
    {
        case HTTP_CLIENT_STATE_DEFER_DONE:
            hcc->defer_status = HTTP_CLIENT_STATE_DEFER_INIT;
            RETURN_BOOL(hcc->defer_result);
            break;
        case HTTP_CLIENT_STATE_DEFER_SEND:
            hcc->defer_status = HTTP_CLIENT_STATE_DEFER_WAIT;
            //not ready
            php_context *context = swoole_get_property(getThis(), 1);
            coro_save(context);
            coro_yield();
            break;
        case HTTP_CLIENT_STATE_DEFER_INIT:
            //not ready
            swoole_php_fatal_error(E_WARNING, "you should post or get or execute before recv  ");
            RETURN_FALSE;
            break;
        default:
            break;
    }
}

static PHP_METHOD(swoole_http_client_coro, setData)
{
    zval *data;
    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &data) == FAILURE)
    {
        return;
    }
    zend_update_property(swoole_http_client_coro_class_entry_ptr, getThis(), ZEND_STRL("requestBody"), data TSRMLS_CC);
    http_client_property *hcc = swoole_get_property(getThis(), 0);
    hcc->request_body = sw_zend_read_property(swoole_http_client_coro_class_entry_ptr, getThis(), ZEND_STRL("requestBody"), 1 TSRMLS_CC);
    RETURN_TRUE;
}

static PHP_METHOD(swoole_http_client_coro, addFile)
{
    char *path;
    zend_size_t l_path;
    char *name;
    zend_size_t l_name;
    char *type = NULL;
    zend_size_t l_type;
    char *filename = NULL;
    zend_size_t l_filename;
    long offset = 0;
    long length = 0;

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "ss|ssll", &path, &l_path, &name, &l_name, &type, &l_type,
            &filename, &l_filename, &offset, &length) == FAILURE)
    {
        RETURN_FALSE;
    }
    if (offset < 0)
    {
        offset = 0;
    }
    if (length < 0)
    {
        length = 0;
    }
    struct stat file_stat;
    if (stat(path, &file_stat) < 0)
    {
        swoole_php_sys_error(E_WARNING, "stat(%s) failed.", path);
        RETURN_FALSE;
    }
    if (file_stat.st_size == 0)
    {
        swoole_php_sys_error(E_WARNING, "cannot send empty file[%s].", filename);
        RETURN_FALSE;
    }
    if (file_stat.st_size <= offset)
    {
        swoole_php_error(E_WARNING, "parameter $offset[%ld] exceeds the file size.", offset);
        RETURN_FALSE;
    }
    if (length > file_stat.st_size - offset)
    {
        swoole_php_sys_error(E_WARNING, "parameter $length[%ld] exceeds the file size.", length);
        RETURN_FALSE;
    }
    if (length == 0)
    {
        length = file_stat.st_size - offset;
    }
    if (type == NULL)
    {
        type = swoole_get_mimetype(path);
        l_type = strlen(type);
    }
    if (filename == NULL)
    {
        char *dot = strrchr(path, '/');
        if (dot == NULL)
        {
            filename = path;
            l_filename = l_path;
        }
        else
        {
            filename = dot + 1;
            l_filename = strlen(filename);
        }
    }

    http_client_property *hcc = swoole_get_property(getThis(), 0);
    zval *files;
    if (!hcc->request_upload_files)
    {
        SW_MAKE_STD_ZVAL(files);
        array_init(files);
        zend_update_property(swoole_http_client_coro_class_entry_ptr, getThis(), ZEND_STRL("uploadFiles"), files TSRMLS_CC);
        sw_zval_ptr_dtor(&files);

        hcc->request_upload_files = sw_zend_read_property(swoole_http_client_coro_class_entry_ptr, getThis(), ZEND_STRL("uploadFiles"), 0 TSRMLS_CC);
        sw_copy_to_stack(hcc->request_upload_files, hcc->_request_upload_files);
    }

    zval *upload_file;
    SW_MAKE_STD_ZVAL(upload_file);
    array_init(upload_file);

    sw_add_assoc_stringl_ex(upload_file, ZEND_STRS("path"), path, l_path, 1);
    sw_add_assoc_stringl_ex(upload_file, ZEND_STRS("name"), name, l_name, 1);
    sw_add_assoc_stringl_ex(upload_file, ZEND_STRS("filename"), filename, l_filename, 1);
    sw_add_assoc_stringl_ex(upload_file, ZEND_STRS("type"), type, l_type, 1);
    add_assoc_long(upload_file, "size", length);
    add_assoc_long(upload_file, "offset", offset);

    add_next_index_zval(hcc->request_upload_files, upload_file);
    RETURN_TRUE;
}



static PHP_METHOD(swoole_http_client_coro, setMethod)
{
    zval *method;
    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "z", &method) == FAILURE)
    {
        return;
    }
    convert_to_string(method);
    zend_update_property(swoole_http_client_coro_class_entry_ptr, getThis(), ZEND_STRL("requestMethod"), method TSRMLS_CC);
    http_client_property *hcc = swoole_get_property(getThis(), 0);
    hcc->request_method = Z_STRVAL_P(method);
    RETURN_TRUE;
}

static PHP_METHOD(swoole_http_client_coro, isConnected)
{
    http_client *http = swoole_get_object(getThis());
    if (!http || !http->cli)
    {
        RETURN_FALSE;
    }
    if (!http->cli->socket)
    {
        RETURN_FALSE;
    }
    RETURN_BOOL(http->cli->socket->active);
}

static PHP_METHOD(swoole_http_client_coro, close)
{
    http_client *http = swoole_get_object(getThis());
    if(!http){

        RETURN_FALSE;
    }

    swClient *cli = http->cli;
    if (!cli)
    {
        swoole_php_fatal_error(E_WARNING, "object is not instanceof swoole_http_client.");
        RETURN_FALSE;
    }
    if (cli->timeout_id > 0)
    {
        php_swoole_clear_timer_coro(cli->timeout_id TSRMLS_CC);
        cli->timeout_id=0;
    }
    if (!cli->socket)
    {
        swoole_php_error(E_WARNING, "not connected to the server");
        RETURN_FALSE;
    }
    if (cli->socket->closed)
    {
        http_client_free(getThis() TSRMLS_CC);
        RETURN_FALSE;
    }
    int ret = SW_OK;
    if (!cli->keep || swConnection_error(SwooleG.error) == SW_CLOSE)
    {
        cli->released = 1;
        ret = cli->close(cli);
        http_client_free(getThis() TSRMLS_CC);
    }
    else
    {
        //unset object
        swoole_set_object(getThis(), NULL);
    }
    SW_CHECK_RETURN(ret);
}



static PHP_METHOD(swoole_http_client_coro, execute)
{
    int ret;
    char *uri = NULL;
    zend_size_t uri_len = 0;
    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &uri, &uri_len) == FAILURE)
    {
        return;
    }
    http_client_property *hcc = swoole_get_property(getThis(), 0);
    if (hcc->defer)
    {
        if (hcc->defer_status != HTTP_CLIENT_STATE_DEFER_INIT)
        {
            RETURN_FALSE;
        }
        hcc->defer_status = HTTP_CLIENT_STATE_DEFER_SEND;
    }
    ret = http_client_coro_execute(getThis(), uri, uri_len TSRMLS_CC);
    if(ret==SW_ERR){
        SW_CHECK_RETURN(ret);
    }


    php_context *context = swoole_get_property(getThis(), 1);
    http_client *http = swoole_get_object(getThis());
	if (http->timeout > 0)
	{
		if (php_swoole_add_timer_coro((int)(http->timeout*1000), http->cli->socket->fd, &http->cli->timeout_id, (void *)context, NULL TSRMLS_CC) == SW_OK
				&& hcc->defer)
		{
			context->state = SW_CORO_CONTEXT_IN_DELAYED_TIMEOUT_LIST;
		}
	}
    if (hcc->defer)
    {
        RETURN_TRUE;
    }
    coro_save(context);
    coro_yield();
}

static PHP_METHOD(swoole_http_client_coro, get)
{
    int ret;
    char *uri = NULL;
    zend_size_t uri_len = 0;
    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s", &uri, &uri_len) == FAILURE)
    {
        return;
    }

    http_client_property *hcc = swoole_get_property(getThis(), 0);
    hcc->request_method = "GET";
    if (hcc->defer)
    {
        if (hcc->defer_status != HTTP_CLIENT_STATE_DEFER_INIT)
        {
            RETURN_FALSE;
        }
        hcc->defer_status = HTTP_CLIENT_STATE_DEFER_SEND;
    }
    ret = http_client_coro_execute(getThis(), uri, uri_len TSRMLS_CC);
    if (ret==SW_ERR)
    {
        SW_CHECK_RETURN(ret);
    }


    http_client *http = swoole_get_object(getThis());
    php_context *context = swoole_get_property(getThis(), 1);
	if (http->timeout > 0)
	{
		if (php_swoole_add_timer_coro((int)(http->timeout*1000), http->cli->socket->fd, &http->cli->timeout_id, (void *)context, NULL TSRMLS_CC) == SW_OK
				&& hcc->defer)
		{
			context->state = SW_CORO_CONTEXT_IN_DELAYED_TIMEOUT_LIST;
		}
	}
    if (hcc->defer)
    {
        RETURN_TRUE;
    }

    coro_save(context);
    coro_yield();
}


static PHP_METHOD(swoole_http_client_coro, post)
{
    int ret;
    char *uri = NULL;
    zend_size_t uri_len = 0;
    zval *post_data;
    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sz", &uri, &uri_len, &post_data) == FAILURE)
    {
        return;
    }

    if (Z_TYPE_P(post_data) != IS_ARRAY && Z_TYPE_P(post_data) != IS_STRING)
    {
        swoole_php_fatal_error(E_WARNING, "post data must be string or array.");
        RETURN_FALSE;
    }

    http_client_property *hcc = swoole_get_property(getThis(), 0);
    zend_update_property(swoole_http_client_coro_class_entry_ptr, getThis(), ZEND_STRL("requestBody"), post_data TSRMLS_CC);
    hcc->request_body = sw_zend_read_property(swoole_http_client_coro_class_entry_ptr, getThis(), ZEND_STRL("requestBody"), 1 TSRMLS_CC);
    sw_copy_to_stack(hcc->request_body, hcc->_request_body);
    hcc->request_method = "POST";
    if (hcc->defer)
    {
        if (hcc->defer_status != HTTP_CLIENT_STATE_DEFER_INIT)
        {
            RETURN_FALSE;
        }
        hcc->defer_status = HTTP_CLIENT_STATE_DEFER_SEND;
    }
    ret = http_client_coro_execute(getThis(), uri, uri_len TSRMLS_CC);
    if (ret==SW_ERR)
    {
        SW_CHECK_RETURN(ret);
    }

    http_client *http = swoole_get_object(getThis());
    php_context *context = swoole_get_property(getThis(), 1);
	if (http->timeout > 0)
	{
		if (php_swoole_add_timer_coro((int)(http->timeout*1000), http->cli->socket->fd, &http->cli->timeout_id, (void *)context, NULL TSRMLS_CC) == SW_OK
				&& hcc->defer)
		{
			context->state = SW_CORO_CONTEXT_IN_DELAYED_TIMEOUT_LIST;
		}
	}
    if (hcc->defer)
    {
        RETURN_TRUE;
    }
    coro_save(context);
    coro_yield();
}
#endif
