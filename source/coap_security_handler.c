/*
 * Copyright (c) 2015-2016 ARM Limited. All Rights Reserved.
 */

#include <string.h>
#include <time.h>
#include <stdlib.h>

#include "mbedtls/sha256.h"
#include "mbedtls/error.h"
#include "mbedtls/ssl_cookie.h"
#include "mbedtls/entropy_poll.h"
#include "mbedtls/ssl.h"
#include "ns_trace.h"
#include "nsdynmemLIB.h"
#include "coap_connection_handler.h"
#include "coap_security_handler.h"
#include "randLIB.h"
#include "mbedtls/ssl_ciphersuites.h"

#if defined(MBEDTLS_KEY_EXCHANGE_ECJPAKE_ENABLED)
const int ECJPAKE_SUITES[] = {
    MBEDTLS_TLS_ECJPAKE_WITH_AES_128_CCM_8,
    0
};
#endif

const static int PSK_SUITES[] = {
    MBEDTLS_TLS_PSK_WITH_AES_128_CBC_SHA256,
    MBEDTLS_TLS_PSK_WITH_AES_256_CCM_8,
    MBEDTLS_TLS_PSK_WITH_AES_128_CCM_8,
    0
};


static void set_timer( void *sec_obj, uint32_t int_ms, uint32_t fin_ms );
static int get_timer( void *sec_obj );
static int coap_security_handler_configure_keys( coap_security_t *sec, coap_security_keys_t keys );

int entropy_poll( void *data, unsigned char *output, size_t len, size_t *olen );
//Point these back to M2MConnectionHandler!!!
int f_send( void *ctx, const unsigned char *buf, size_t len );
int f_recv(void *ctx, unsigned char *buf, size_t len);

static int coap_security_handler_init(coap_security_t *sec){
    const char *pers = "dtls_client";
    mbedtls_ssl_init( &sec->_ssl );
    mbedtls_ssl_config_init( &sec->_conf );
    mbedtls_ctr_drbg_init( &sec->_ctr_drbg );
    mbedtls_entropy_init( &sec->_entropy );

#if defined(MBEDTLS_X509_CRT_PARSE_C)
    mbedtls_x509_crt_init( &sec->_cacert );
    mbedtls_x509_crt_init( &sec->_owncert );
#endif
    mbedtls_pk_init( &sec->_pkey );

    memset(&sec->_cookie, 0, sizeof(simple_cookie_t));
    memset(&sec->_keyblk, 0, sizeof(key_block_t));

    sec->_is_started = false;

    //TODO: Must have at least 1 strong entropy source, otherwise DTLS will fail.
    //This is NOT strong even we say it is!
    if( mbedtls_entropy_add_source( &sec->_entropy, entropy_poll, NULL,
                                128, 1 ) < 0 ){
        return -1;
    }

    if( ( mbedtls_ctr_drbg_seed( &sec->_ctr_drbg, mbedtls_entropy_func, &sec->_entropy,
                               (const unsigned char *) pers,
                               strlen( pers ) ) ) != 0 )
    {
        return -1;
    }
    return 0;
}

static void coap_security_handler_reset(coap_security_t *sec){
#if defined(MBEDTLS_X509_CRT_PARSE_C)
    mbedtls_x509_crt_free(&sec->_cacert);
    mbedtls_x509_crt_free(&sec->_owncert);
#endif

    mbedtls_pk_free(&sec->_pkey);

    mbedtls_entropy_free( &sec->_entropy );
    mbedtls_ctr_drbg_free( &sec->_ctr_drbg );
    mbedtls_ssl_config_free(&sec->_conf);
    mbedtls_ssl_free(&sec->_ssl);
}


coap_security_t *coap_security_create(int8_t socket_id, int8_t timer_id, uint8_t *address_ptr, uint16_t port, SecureConnectionMode mode,
                                          send_cb *send_cb,
                                          receive_cb *receive_cb,
                                          start_timer_cb *start_timer_cb,
                                          timer_status_cb *timer_status_cb)
{
    if( !address_ptr || send_cb == NULL || receive_cb == NULL || start_timer_cb == NULL || timer_status_cb == NULL){
        return NULL;
    }
    coap_security_t *this = ns_dyn_mem_alloc(sizeof(coap_security_t));
    if( !this ){
        return NULL;
    }
    memset(this, 0, sizeof(coap_security_t));
    if( -1 == coap_security_handler_init(this) ){
        ns_dyn_mem_free(this);
        return NULL;
    }
    this->_remote_port = port;
    memcpy(this->_remote_address, address_ptr, 16);
    this->_conn_mode = mode;
    memset(this->_pw, 0, 64);
    this->_pw_len = 0;
    this->_socket_id = socket_id;
    this->_timer_id = timer_id;
    this->_send_cb = send_cb;
    this->_receive_cb = receive_cb;
    this->_start_timer_cb = start_timer_cb;
    this->_timer_status_cb = timer_status_cb;

    return this;
}

void coap_security_destroy(coap_security_t *sec){
    if( sec ){
        coap_security_handler_reset(sec);
        ns_dyn_mem_free(sec);
        sec = NULL;
    }
}

/**** Random number functions ****/

/**
 * Get a random array of bytes.
 * Called back by mbedtls when it wants to fill a buffer with random data
 * Must return 0 on success.
 */
static int get_random(void *ctx, unsigned char *buf, size_t len)
{
    static int initialised = 0;
    uint32_t i;

    (void)ctx; /* No context */

    if (!initialised) {
        randLIB_seed_random();
        initialised = 1;
    }

    for (i = 0; i < len; i++) {
        buf[i] = (uint8_t)randLIB_get_8bit();
    }
    return 0; /* Success */
}

/**** Cookie functions ****/
static int simple_cookie_write(void *ctx,
                               unsigned char **p, unsigned char *end,
                               const unsigned char *info, size_t ilen)
{
    //TODO: As per RFC 6347 cookie must be stateless. This is not the case in here!
    //This should be fixed if we see that dos attack would be an issue.
    //this is proposed solution in RFC: Cookie = HMAC(Secret, Client-IP, Client-Parameters)
    //Secret is generated here and oftenly changed to prevent statistical attack
    simple_cookie_t *p_cookie = (simple_cookie_t *)ctx;

    /* Not using additional info */
    (void)info;
    (void)ilen;

    if ((size_t)(end - *p) < COOKIE_SIMPLE_LEN) {
        return MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL;
    }

    /* Use a simple random number of length COOKIE_SIMPLE_LEN bytes */
    get_random(NULL, p_cookie->value, COOKIE_SIMPLE_LEN);
    memcpy(*p, p_cookie->value, COOKIE_SIMPLE_LEN);
    p_cookie->len = COOKIE_SIMPLE_LEN;
    *p += COOKIE_SIMPLE_LEN;
    return 0;
}

static int simple_cookie_check(void *ctx,
                               const unsigned char *cookie, size_t clen,
                               const unsigned char *info, size_t ilen)
{
    simple_cookie_t *p_cookie = (simple_cookie_t *)ctx;

    /* Not using additional info */
    (void)info;
    (void)ilen;

    if ((p_cookie->len == 0) ||
        (clen != p_cookie->len) ||
        (memcmp(cookie, p_cookie->value, p_cookie->len) != 0)) {
        return -1; /* This is what it is in mbedtls... */
    }
    return 0;
}

/**** Key export function ****/

static int export_key_block(void *ctx,
                            const unsigned char *mk, const unsigned char *kb,
                            size_t maclen, size_t keylen, size_t ivlen)
{
    key_block_t *p_key_block = (key_block_t *)ctx;

    /* Not using master key */
    (void)mk;

    /* Sanity check MAC and key lengths */
    if ((maclen != 0) || (((2 * keylen) + (2 * ivlen)) != KEY_BLOCK_LEN)) {
        return -1; /* Something seriously wrong! */
    }

    /* Copy the key block we are using */
    /* No need to skip over MAC keys, MAC len must be 0 if we are here */
    memcpy(p_key_block->value, kb /* + (2 * maclen)*/, (2 * keylen) + (2 * ivlen));
    return 0;
}

int coap_security_handler_configure_keys( coap_security_t *sec, coap_security_keys_t keys )
{
    int ret = -1;
    switch( sec->_conn_mode ){
        case Certificate:{
#if defined(MBEDTLS_X509_CRT_PARSE_C)
        if(  mbedtls_x509_crt_parse( &sec->_cacert, keys._server_cert,
                                     keys._server_cert_len ) < 0 ){
            break;
        }
        if( mbedtls_x509_crt_parse( &sec->_owncert, keys._pub_cert_or_identifier,
                                    keys._pub_len ) < 0 ){
            break;
        }
        if( mbedtls_pk_parse_key(&sec->_pkey, keys._priv, keys._priv_len, NULL, 0) < 0){
            break;
        }
        //TODO: If needed in server mode, this won't work
        if( 0 != mbedtls_ssl_conf_own_cert(&sec->_conf, &sec->_owncert, &sec->_pkey) ){
            break;
        }
        //TODO: use MBEDTLS_SSL_VERIFY_REQUIRED instead of optional
        mbedtls_ssl_conf_authmode( &sec->_conf, MBEDTLS_SSL_VERIFY_OPTIONAL );
        mbedtls_ssl_conf_ca_chain( &sec->_conf, &sec->_cacert, NULL );
        ret = 0;
#endif
        break;
        }
        case PSK: {
#if defined(MBEDTLS_KEY_EXCHANGE__SOME__PSK_ENABLED)
        if( 0 != mbedtls_ssl_conf_psk(&sec->_conf, keys._priv, keys._priv_len, keys._pub_cert_or_identifier, keys._pub_len) ){
            break;
        }
        mbedtls_ssl_conf_ciphersuites(&sec->_conf, PSK_SUITES);
        ret = 0;
#endif
        break;
        }
        case ECJPAKE: {
#if defined(MBEDTLS_KEY_EXCHANGE_ECJPAKE_ENABLED)
            if( mbedtls_ssl_set_hs_ecjpake_password(&sec->_ssl, keys._priv, keys._priv_len) != 0 ){
                return -1;
            }
            mbedtls_ssl_conf_ciphersuites(&sec->_conf, ECJPAKE_SUITES);

            //NOTE: If thread starts supporting PSK in other modes, then this will be needed!
            mbedtls_ssl_conf_export_keys_cb(&sec->_conf,
                                            export_key_block,
                                            &sec->_keyblk);
        ret = 0;
#endif
        break;
        }

        default:
        break;
    }
    return ret;
}

int coap_security_handler_connect(coap_security_t *sec, bool is_server, SecureSocketMode sock_mode, coap_security_keys_t keys){
    int ret = -1;

    if( !sec ){
        return ret;
    }
    sec->_is_blocking = true;

    int endpoint = MBEDTLS_SSL_IS_CLIENT;
    if( is_server ){
        endpoint = MBEDTLS_SSL_IS_SERVER;
    }

    int mode = MBEDTLS_SSL_TRANSPORT_DATAGRAM;
    if( sock_mode == TLS ){
        mode = MBEDTLS_SSL_TRANSPORT_STREAM;
    }

    if( ( mbedtls_ssl_config_defaults( &sec->_conf,
                       endpoint,
                       mode, 0 ) ) != 0 )
    {
        return -1;
    }

    mbedtls_ssl_set_bio( &sec->_ssl, sec,
                        f_send, f_recv, NULL );

    mbedtls_ssl_set_timer_cb( &sec->_ssl, sec, set_timer,
                                            get_timer );

    if( coap_security_handler_configure_keys( sec, keys ) != 0 ){
        return -1;
    }

    //TODO: Only needed for server type?
    mbedtls_ssl_conf_dtls_cookies(&sec->_conf, simple_cookie_write,
                                  simple_cookie_check,
                                  &sec->_cookie);

    sec->_is_started = true;

    do {
        ret = mbedtls_ssl_handshake_step( &sec->_ssl );
        if( ret == MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED ){ //cookie check failed
            if( is_server ){
                mbedtls_ssl_session_reset(&sec->_ssl);
#if defined(MBEDTLS_KEY_EXCHANGE_ECJPAKE_ENABLED)
                if( mbedtls_ssl_set_hs_ecjpake_password(&sec->_ssl, keys._priv, keys._priv_len) != 0 ){
                    return -1;
                }
#endif
                ret = MBEDTLS_ERR_SSL_WANT_READ; //needed to keep doing
            }else{
                ret = -1;
            }
        }
    }while( ret == MBEDTLS_ERR_SSL_WANT_READ ||
           ret == MBEDTLS_ERR_SSL_WANT_WRITE );

    if( ret != 0){
        ret = -1;
    }else{
        if( mbedtls_ssl_get_verify_result( &sec->_ssl ) != 0 )
            {
                ret = -1;
            }
    }

    return ret;
}

int coap_security_handler_connect_non_blocking(coap_security_t *sec, bool is_server, SecureSocketMode sock_mode, coap_security_keys_t keys, uint32_t timeout_min, uint32_t timeout_max)
{

    if( !sec ){
        return -1;
    }
    sec->_is_blocking = false;

    int endpoint = MBEDTLS_SSL_IS_CLIENT;
    if( is_server ){
        endpoint = MBEDTLS_SSL_IS_SERVER;
    }

    int mode = MBEDTLS_SSL_TRANSPORT_DATAGRAM;
    if( sock_mode == TLS ){
        mode = MBEDTLS_SSL_TRANSPORT_STREAM;
    }

    if( ( mbedtls_ssl_config_defaults( &sec->_conf,
                       endpoint,
                       mode, 0 ) ) != 0 )
    {
        return -1;
    }

    if(!timeout_max && !timeout_min){
        mbedtls_ssl_conf_handshake_timeout( &sec->_conf, DTLS_HANDSHAKE_TIMEOUT_MIN, DTLS_HANDSHAKE_TIMEOUT_MAX );
    }
    else{
        mbedtls_ssl_conf_handshake_timeout( &sec->_conf, timeout_min, timeout_max );
    }

    mbedtls_ssl_conf_rng( &sec->_conf, mbedtls_ctr_drbg_random, &sec->_ctr_drbg );

    if( ( mbedtls_ssl_setup( &sec->_ssl, &sec->_conf ) ) != 0 )
    {
       return -1;
    }

    mbedtls_ssl_set_bio( &sec->_ssl, sec,
                        f_send, f_recv, NULL );

    mbedtls_ssl_set_timer_cb( &sec->_ssl, sec, set_timer,
                                            get_timer );

#if defined(MBEDTLS_KEY_EXCHANGE_ECJPAKE_ENABLED)
    //TODO: Figure out better way!!!
    //Password should never be stored in multiple places!!!
    if( is_server && keys._priv_len > 0){
        memcpy(sec->_pw, keys._priv, keys._priv_len);
        sec->_pw_len = keys._priv_len;
    }
#endif

    if( coap_security_handler_configure_keys( sec, keys ) != 0 ){
        return -1;
    }

    //Only needed for server type?
    mbedtls_ssl_conf_dtls_cookies(&sec->_conf, simple_cookie_write,
                                  simple_cookie_check,
                                  &sec->_cookie);

    mbedtls_ssl_conf_min_version(&sec->_conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MAJOR_VERSION_3);
    mbedtls_ssl_conf_max_version(&sec->_conf, MBEDTLS_SSL_MAJOR_VERSION_3, MBEDTLS_SSL_MAJOR_VERSION_3);
	
    sec->_is_started = true;

    int ret = mbedtls_ssl_handshake_step( &sec->_ssl );
    if( ret == 0 ){
        ret = mbedtls_ssl_handshake_step( &sec->_ssl );
        if( is_server && 0 == ret){
            ret = coap_security_handler_continue_connecting( sec );
        }
    }

    if( ret >= 0){
        ret = 1;
    }else{
        ret = -1;
    }
    return ret;
}

int coap_security_handler_continue_connecting(coap_security_t *sec){
    int ret = -1;

    while( ret != MBEDTLS_ERR_SSL_WANT_READ ){
        ret = mbedtls_ssl_handshake_step( &sec->_ssl );

        if( MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED == ret){
            mbedtls_ssl_session_reset(&sec->_ssl);
#if defined(MBEDTLS_KEY_EXCHANGE_ECJPAKE_ENABLED)
            if( mbedtls_ssl_set_hs_ecjpake_password(&sec->_ssl, sec->_pw, sec->_pw_len) != 0 ){
                return -1;
            }
#endif
            return 1;
        }
        else if(ret && (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)){
            return ret;
        }

        if( sec->_ssl.state == MBEDTLS_SSL_HANDSHAKE_OVER ){
            return 0;
        }
    }

    if(ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE){
        return 1;
    }

    return -1;
}


int coap_security_handler_send_message(coap_security_t *sec, unsigned char *message, size_t len){
    int ret=-1;

    if( sec ){
        do ret = mbedtls_ssl_write( &sec->_ssl, (unsigned char *) message, len );
        while( ret == MBEDTLS_ERR_SSL_WANT_READ ||
               ret == MBEDTLS_ERR_SSL_WANT_WRITE );
    }

    return ret; //bytes written
}

int coap_security_send_close_alert(coap_security_t *sec)
{
    if( !sec ){
        return -1;
    }

    if(!mbedtls_ssl_close_notify(&sec->_ssl)){
        return 0;
    }
    return -1;
}

int coap_security_handler_read(coap_security_t *sec, unsigned char* buffer, size_t len){
    int ret=-1;
    int max_loops = 100;

    if( sec && buffer ){
        memset( buffer, 0, len );
        do {
            ret = mbedtls_ssl_read( &sec->_ssl, buffer, len );
        } while( (ret == MBEDTLS_ERR_SSL_WANT_READ ||
                  ret == MBEDTLS_ERR_SSL_WANT_WRITE)
                && --max_loops);
    }
    return ret; //bytes read
}

/**** Timer functions ****/

/**
 * Set timer function.
 * Called back by mbedtls when it wants to set a timer.
 * Accepts an intermediate and a final delay in milliseconds
 * If the final delay is 0, cancels the running timer.
 * TODO - might be better to use an event timer in conjunction with
 * CoAP tasklet
 */
static void set_timer(void *sec_obj, uint32_t int_ms, uint32_t fin_ms)
{
    coap_security_t *sec = (coap_security_t *)sec_obj;
    if( sec->_start_timer_cb ){
        sec->_start_timer_cb( sec->_timer_id, int_ms, fin_ms);
    }
}

/**
 * Get timer function.
 * Called back by mbedtls when it wants to get a timer state.
 * Returns the state of the current timer
 * TODO - might be better to use an event timer in conjunction with
 * CoAP tasklet
 */
static int get_timer(void *sec_obj)
{
    coap_security_t *sec = (coap_security_t *)sec_obj;
    if( sec->_timer_status_cb ){
        return sec->_timer_status_cb(sec->_timer_id);
    }
    return -1;
}

int f_send( void *ctx, const unsigned char *buf, size_t len){
    coap_security_t *sec = (coap_security_t *)ctx;
    return sec->_send_cb(sec->_socket_id, sec->_remote_address, sec->_remote_port, buf, len);
}

int f_recv(void *ctx, unsigned char *buf, size_t len){
    coap_security_t *sec = (coap_security_t *)ctx;
    return sec->_receive_cb(sec->_socket_id, buf, len);
}

int entropy_poll( void *ctx, unsigned char *output, size_t len,
                           size_t *olen )
{
    //TODO: change to more secure random
    randLIB_seed_random();
    char *c = (char*)ns_dyn_mem_temporary_alloc(len);
    if( !c ){
        return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
    }
    memset(c, 0, len);
    for(uint16_t i=0; i < len; i++){
        *(c + i) = (char)randLIB_get_8bit();
    }
    memmove(output, c, len);
    *olen = len;

    ns_dyn_mem_free(c);
    return( 0 );
}
