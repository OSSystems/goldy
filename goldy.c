/*
 * goldy - DTLS proxy
 */

#if defined(__linux__)
#define _XOPEN_SOURCE 700
#endif

#include "mbedtls/config.h"
#include "mbedtls/platform.h"

#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/certs.h"
#include "mbedtls/x509.h"
#include "mbedtls/ssl.h"
#include "mbedtls/ssl_cookie.h"
#include "mbedtls/net.h"
#include "mbedtls/error.h"
#include "mbedtls/debug.h"
#include "mbedtls/timing.h"

#if defined(MBEDTLS_SSL_CACHE_C)
#include "mbedtls/ssl_cache.h"
#endif

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <getopt.h>

int connect_to_udp_backend(const char *backend_host, const char* backend_port) {
    struct addrinfo hints;
    struct addrinfo *result;
    struct addrinfo *rp;
    int ret, sfd;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = 0;
    hints.ai_protocol = 0;

    ret = getaddrinfo(backend_host, backend_port, &hints, &result);
    if (ret != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(ret));
        exit(EXIT_FAILURE);
    }

    for (rp = result; rp; rp = rp->ai_next) {
        sfd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sfd == -1) {
            continue;
        }

        if (connect(sfd, rp->ai_addr, rp->ai_addrlen) != -1) {
            break; /* Success */
        }

        close(sfd);
    }

    freeaddrinfo(result);

    if (!rp) {
        printf("failed: could not connect to UDP backend\n");
        return -1;
    }

    return sfd;
}

void print_usage(const char* argv0) {
    printf("Usage: %s -l listen_host:port -b backend_host:port\n", argv0);
    exit(1);
}

int main( int argc, char *argv[] )
{
    int ret, len, opt;
    char* sep;
    int backend_fd;
    mbedtls_net_context listen_fd, client_fd;
    unsigned char buf[10000];
    const char *pers = "goldy";
    char listen_host[100] = "";
    char listen_port[6] = "";
    char backend_host[100] = "";
    char backend_port[6] = "";
    char cert_file[1024] = "";
    char private_key_file[1024] = "";
    unsigned char client_ip[16] = { 0 };
    size_t cliip_len;
    mbedtls_ssl_cookie_ctx cookie_ctx;

    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt srvcert;
    mbedtls_pk_context pkey;
    mbedtls_timing_delay_context timer;
#if defined(MBEDTLS_SSL_CACHE_C)
    mbedtls_ssl_cache_context cache;
#endif

    /* Parse command line */
    while ((opt = getopt(argc, argv, "b:c:l:k:")) != -1) {
        switch (opt) {
        case 'b':
            sep = strchr(optarg, ':');
            if (!sep) {
                print_usage(argv[0]);
            }
            *sep = '\0';
            strncpy(backend_host, optarg, sizeof(backend_host));
            strncpy(backend_port, sep + 1, sizeof(backend_port));
            break;
        case 'c':
            strncpy(cert_file, optarg, sizeof(cert_file));
            break;
        case 'l':
            sep = strchr(optarg, ':');
            if (!sep) {
                print_usage(argv[0]);
            }
            *sep = '\0';
            strncpy(listen_host, optarg, sizeof(listen_host));
            strncpy(listen_port, sep + 1, sizeof(listen_port));
            break;
        case 'k':
            strncpy(private_key_file, optarg, sizeof(private_key_file));
            break;
        default: /* '?' */
            print_usage(argv[0]);
        }
    }

    if (!(listen_host[0] && listen_port[0] &&
          backend_host[0] && backend_port[0] &&
          cert_file[0] && private_key_file[0])) {
        print_usage(argv[0]);
    }

    mbedtls_net_init( &listen_fd );
    mbedtls_net_init( &client_fd );
    mbedtls_ssl_init( &ssl );
    mbedtls_ssl_config_init( &conf );
    mbedtls_ssl_cookie_init( &cookie_ctx );
#if defined(MBEDTLS_SSL_CACHE_C)
    mbedtls_ssl_cache_init( &cache );
#endif
    mbedtls_x509_crt_init( &srvcert );
    mbedtls_pk_init( &pkey );
    mbedtls_entropy_init( &entropy );
    mbedtls_ctr_drbg_init( &ctr_drbg );

    /*
     * 1. Load the certificates and private key
     */
    printf( "\n  . Loading the server cert. and key..." );
    fflush( stdout );

    ret = mbedtls_x509_crt_parse_file(&srvcert, cert_file);
    if( ret != 0 )
    {
        printf( " failed\n  !  mbedtls_x509_crt_parse returned %d\n\n", ret );
        goto exit;
    }

    ret = mbedtls_pk_parse_keyfile(&pkey, private_key_file, NULL);
    if( ret != 0 )
    {
        printf( " failed\n  !  mbedtls_pk_parse_key returned %d\n\n", ret );
        goto exit;
    }

    printf( " ok\n" );

    /*
     * 2. Setup the "listening" UDP socket
     */
    printf( "  . Bind on udp %s:%s ...", listen_host, listen_port );
    fflush( stdout );

    if( ( ret = mbedtls_net_bind( &listen_fd, listen_host, listen_port, MBEDTLS_NET_PROTO_UDP ) ) != 0 )
    {
        printf( " failed\n  ! mbedtls_net_bind returned %d\n\n", ret );
        goto exit;
    }

    printf( " ok\n" );

    /*
     * 3. Seed the RNG
     */
    printf( "  . Seeding the random number generator..." );
    fflush( stdout );

    if( ( ret = mbedtls_ctr_drbg_seed( &ctr_drbg, mbedtls_entropy_func, &entropy,
                               (const unsigned char *) pers,
                               strlen( pers ) ) ) != 0 )
    {
        printf( " failed\n  ! mbedtls_ctr_drbg_seed returned %d\n", ret );
        goto exit;
    }

    printf( " ok\n" );

    /*
     * 4. Setup stuff
     */
    printf( "  . Setting up the DTLS data..." );
    fflush( stdout );

    if( ( ret = mbedtls_ssl_config_defaults( &conf,
                    MBEDTLS_SSL_IS_SERVER,
                    MBEDTLS_SSL_TRANSPORT_DATAGRAM,
                    MBEDTLS_SSL_PRESET_DEFAULT ) ) != 0 )
    {
        mbedtls_printf( " failed\n  ! mbedtls_ssl_config_defaults returned %d\n\n", ret );
        goto exit;
    }

    mbedtls_ssl_conf_rng( &conf, mbedtls_ctr_drbg_random, &ctr_drbg );

#if defined(MBEDTLS_SSL_CACHE_C)
    mbedtls_ssl_conf_session_cache( &conf, &cache,
                                   mbedtls_ssl_cache_get,
                                   mbedtls_ssl_cache_set );
#endif

    mbedtls_ssl_conf_ca_chain( &conf, srvcert.next, NULL );
   if( ( ret = mbedtls_ssl_conf_own_cert( &conf, &srvcert, &pkey ) ) != 0 )
    {
        printf( " failed\n  ! mbedtls_ssl_conf_own_cert returned %d\n\n", ret );
        goto exit;
    }

    if( ( ret = mbedtls_ssl_cookie_setup( &cookie_ctx,
                                  mbedtls_ctr_drbg_random, &ctr_drbg ) ) != 0 )
    {
        printf( " failed\n  ! mbedtls_ssl_cookie_setup returned %d\n\n", ret );
        goto exit;
    }

    mbedtls_ssl_conf_dtls_cookies( &conf, mbedtls_ssl_cookie_write, mbedtls_ssl_cookie_check,
                               &cookie_ctx );

    if( ( ret = mbedtls_ssl_setup( &ssl, &conf ) ) != 0 )
    {
        printf( " failed\n  ! mbedtls_ssl_setup returned %d\n\n", ret );
        goto exit;
    }

    mbedtls_ssl_set_timer_cb( &ssl, &timer, mbedtls_timing_set_delay,
                                            mbedtls_timing_get_delay );

    printf( " ok\n" );

reset:
#ifdef MBEDTLS_ERROR_C
    if( ret != 0 )
    {
        char error_buf[100];
        mbedtls_strerror(ret, error_buf, sizeof(error_buf));
        printf("Last error was: %d - %s\n\n", ret, error_buf);
    }
#endif

    mbedtls_net_free( &client_fd );

    mbedtls_ssl_session_reset( &ssl );

    /*
     * 3. Wait until a client connects
     */
    printf( "  . Waiting for a remote connection ..." );
    fflush( stdout );

    if( ( ret = mbedtls_net_accept( &listen_fd, &client_fd,
                    client_ip, sizeof( client_ip ), &cliip_len ) ) != 0 )
    {
        printf( " failed\n  ! mbedtls_net_accept returned %d\n\n", ret );
        goto exit;
    }

    {
        union sockaddr_u {
            struct sockaddr_storage storage;
            struct sockaddr_in in;
            struct sockaddr_in6 in6;
            struct sockaddr sockaddr;
        } addr;
        socklen_t addrlen = sizeof(addr.storage);
        char client_ip_str[INET6_ADDRSTRLEN];
        int client_port;

        getpeername(client_fd.fd, &addr.sockaddr, &addrlen);

        /* deal with both IPv4 and IPv6: */
        if (addr.storage.ss_family == AF_INET) {
            struct sockaddr_in *s_ip4 = &addr.in;
            client_port = ntohs(s_ip4->sin_port);
            inet_ntop(AF_INET, &s_ip4->sin_addr, client_ip_str, sizeof(client_ip_str));
        } else {
            struct sockaddr_in6 *s_ip6 = &addr.in6;
            client_port = ntohs(s_ip6->sin6_port);
            inet_ntop(AF_INET6, &s_ip6->sin6_addr, client_ip_str, sizeof(client_ip_str));
        }

        printf("\nReceived connection from %s:%d\n", client_ip_str, client_port);
    }

    /* For HelloVerifyRequest cookies */
    if( ( ret = mbedtls_ssl_set_client_transport_id( &ssl,
                    client_ip, cliip_len ) ) != 0 )
    {
        printf( " failed\n  ! "
                "mbedtls_ssl_set_client_transport_id() returned -0x%x\n\n", -ret );
        goto exit;
    }

    mbedtls_ssl_set_bio( &ssl, &client_fd,
                         mbedtls_net_send, mbedtls_net_recv, mbedtls_net_recv_timeout );

    printf( " ok\n" );

    /*
     * 5. Handshake
     */
    printf( "  . Performing the DTLS handshake..." );
    fflush( stdout );

    do ret = mbedtls_ssl_handshake( &ssl );
    while( ret == MBEDTLS_ERR_SSL_WANT_READ ||
           ret == MBEDTLS_ERR_SSL_WANT_WRITE );

    if( ret == MBEDTLS_ERR_SSL_HELLO_VERIFY_REQUIRED )
    {
        printf( " hello verification requested\n" );
        ret = 0;
        goto reset;
    }
    else if( ret != 0 )
    {
        printf( " failed\n  ! mbedtls_ssl_handshake returned -0x%x\n\n", -ret );
        goto reset;
    }

    printf( " ok\n" );

    /*
     * 6. Read the echo Request
     */
    printf( "  < Read from client:" );
    fflush( stdout );

    len = sizeof( buf ) - 1;
    memset( buf, 0, sizeof( buf ) );

    do ret = mbedtls_ssl_read( &ssl, buf, len );
    while( ret == MBEDTLS_ERR_SSL_WANT_READ ||
           ret == MBEDTLS_ERR_SSL_WANT_WRITE );

    if( ret <= 0 )
    {
        switch( ret )
        {
            case MBEDTLS_ERR_SSL_TIMEOUT:
                printf( " timeout\n\n" );
                goto reset;

            case MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY:
                printf( " connection was closed gracefully\n" );
                ret = 0;
                goto close_notify;

            default:
                printf( " mbedtls_ssl_read returned -0x%x\n\n", -ret );
                goto reset;
        }
    }

    len = ret;
    printf( " %d bytes read\n\n%s\n\n", len, buf );

    backend_fd = connect_to_udp_backend(backend_host, backend_port);
    ret = send(backend_fd, buf, len, 0);
    printf("  %d bytes sent to backend server (%s:%s)\n", ret, backend_host, backend_port);

    /* Wait for response */
    len = sizeof(buf) - 1;
    memset(buf, 0, sizeof(buf));
    ret = recv(backend_fd, buf, len, 0);
    printf("  %d bytes received from backend server\n", ret);

    close(backend_fd);

    /*
     * 7. Write the 200 Response
     */
    printf( "  > Write to client:" );
    fflush( stdout );

    len = ret;
    do ret = mbedtls_ssl_write( &ssl, buf, len );
    while( ret == MBEDTLS_ERR_SSL_WANT_READ ||
           ret == MBEDTLS_ERR_SSL_WANT_WRITE );

    if( ret < 0 )
    {
        printf( " failed\n  ! mbedtls_ssl_write returned %d\n\n", ret );
        goto exit;
    }

    len = ret;
    printf( " %d bytes written\n\n%s\n\n", len, buf );

    /*
     * 8. Done, cleanly close the connection
     */
close_notify:
    printf( "  . Closing the connection..." );

    /* No error checking, the connection might be closed already */
    do ret = mbedtls_ssl_close_notify( &ssl );
    while( ret == MBEDTLS_ERR_SSL_WANT_WRITE );
    ret = 0;

    printf( " done\n" );

    goto reset;

    /*
     * Final clean-ups and exit
     */
exit:

#ifdef MBEDTLS_ERROR_C
    if( ret != 0 )
    {
        char error_buf[100];
        mbedtls_strerror( ret, error_buf, 100 );
        printf( "Last error was: %d - %s\n\n", ret, error_buf );
    }
#endif

    mbedtls_net_free( &client_fd );
    mbedtls_net_free( &listen_fd );

    mbedtls_x509_crt_free( &srvcert );
    mbedtls_pk_free( &pkey );
    mbedtls_ssl_free( &ssl );
    mbedtls_ssl_config_free( &conf );
    mbedtls_ssl_cookie_free( &cookie_ctx );
#if defined(MBEDTLS_SSL_CACHE_C)
    mbedtls_ssl_cache_free( &cache );
#endif
    mbedtls_ctr_drbg_free( &ctr_drbg );
    mbedtls_entropy_free( &entropy );

    return ret == 0 ? 0 : 1;
}