/* Configuracao extra do mbedTLS.
 *
 * O mbedTLS vem com DTLS-SRTP desligado por padrao, e sem isso o
 * libdatachannel nao compila: e exatamente a extensao que negocia as chaves
 * do SRTP durante o handshake DTLS, o coracao de como o WebRTC cifra a midia.
 *
 * Este arquivo entra por MBEDTLS_USER_CONFIG_FILE, que o mbedTLS inclui depois
 * da configuracao padrao - assim da para ligar o que falta sem editar o codigo
 * da dependencia. */

#ifndef MBEDTLS_SSL_DTLS_SRTP
#define MBEDTLS_SSL_DTLS_SRTP
#endif
