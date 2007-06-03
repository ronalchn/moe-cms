/*
 *  The Submit Daemon
 *
 *  (c) 2007 Martin Mares <mj@ucw.cz>
 */

#define LOCAL_DEBUG

#include "lib/lib.h"

#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <gnutls/gnutls.h>
#include <gnutls/x509.h>

static int port = 8888;

static gnutls_certificate_credentials_t cert_cred;
static gnutls_dh_params_t dh_params;

#define DH_BITS 1024
#define TLS_CHECK(name) if (err < 0) die(#name " failed: %s", gnutls_strerror(err))

static void
tls_init(void)
{
  int err;

  log(L_INFO, "Initializing TLS");
  gnutls_global_init();
  err = gnutls_certificate_allocate_credentials(&cert_cred);
  TLS_CHECK(gnutls_certificate_allocate_credentials);
  err = gnutls_certificate_set_x509_trust_file(cert_cred, "ca-cert.pem", GNUTLS_X509_FMT_PEM);
  if (!err)
    die("No CA certificate found");
  if (err < 0)
    die("Unable to load X509 trust file: %s", gnutls_strerror(err));
  err = gnutls_certificate_set_x509_key_file(cert_cred, "server-cert.pem", "server-key.pem", GNUTLS_X509_FMT_PEM);
  if (err < 0)
    die("Unable to load X509 key file: %s", gnutls_strerror(err));

  log(L_INFO, "Setting up DH parameters");
  err = gnutls_dh_params_init(&dh_params); TLS_CHECK(gnutls_dh_params_init);
  err = gnutls_dh_params_generate2(dh_params, DH_BITS); TLS_CHECK(gnutls_dh_params_generate2);
  gnutls_certificate_set_dh_params(cert_cred, dh_params);
}

static gnutls_session_t
tls_new_session(int sk)
{
  gnutls_session_t s;
  int err;

  err = gnutls_init(&s, GNUTLS_SERVER); TLS_CHECK(gnutls_init);
  err = gnutls_set_default_priority(s); TLS_CHECK(gnutls_set_default_priority);			// FIXME
  gnutls_credentials_set(s, GNUTLS_CRD_CERTIFICATE, cert_cred);
  gnutls_certificate_server_set_request(s, GNUTLS_CERT_REQUEST);
  gnutls_dh_set_prime_bits(s, DH_BITS);
  gnutls_transport_set_ptr(s, (gnutls_transport_ptr_t) sk);
  return s;
}

static const char *
tls_verify_cert(gnutls_session_t s)
{
  uns status, num_certs;
  int err;
  gnutls_x509_crt_t cert;
  const gnutls_datum_t *certs;

  DBG("Verifying peer certificates");
  err = gnutls_certificate_verify_peers2(s, &status);
  if (err < 0)
    return gnutls_strerror(err);
  DBG("Verify status: %04x", status);
  if (status & GNUTLS_CERT_INVALID)
    return "Certificate is invalid";
  /* XXX: We do not handle revokation. */
  if (gnutls_certificate_type_get(s) != GNUTLS_CRT_X509)
    return "Certificate is not X509";

  err = gnutls_x509_crt_init(&cert);
  if (err < 0)
    return "gnutls_x509_crt_init() failed";
  certs = gnutls_certificate_get_peers(s, &num_certs);
  if (!certs)
    return "No peer certificate found";
  DBG("Got certificate list with %d peers", num_certs);

  err = gnutls_x509_crt_import(cert, &certs[0], GNUTLS_X509_FMT_DER);
  if (err < 0)
    return "Cannot import certificate";
  /* XXX: We do not check expiration and activation since the keys are generated for a single contest only anyway. */

  byte dn[256];
  size_t dn_len = sizeof(dn);
  err = gnutls_x509_crt_get_dn_by_oid(cert, GNUTLS_OID_X520_COMMON_NAME, 0, 0, dn, &dn_len);
  if (err < 0)
    return "Cannot retrieve common name";
  log(L_INFO, "Cert CN: %s", dn);

  /* Check certificate purpose */
  byte purp[256];
  int purpi = 0;
  do
    {
      size_t purp_len = sizeof(purp);
      uns crit;
      err = gnutls_x509_crt_get_key_purpose_oid(cert, purpi++, purp, &purp_len, &crit);
      if (err == GNUTLS_E_REQUESTED_DATA_NOT_AVAILABLE)
	return "Not a client certificate";
      TLS_CHECK(gnutls_x509_crt_get_key_purpose_oid);
    }
  while (strcmp(purp, GNUTLS_KP_TLS_WWW_CLIENT));

  DBG("Verified OK");
  return NULL;
}

static void
tls_log_params(gnutls_session_t s)
{
  const char *proto = gnutls_protocol_get_name(gnutls_protocol_get_version(s));
  const char *kx = gnutls_kx_get_name(gnutls_kx_get(s));
  const char *cert = gnutls_certificate_type_get_name(gnutls_certificate_type_get(s));
  const char *comp = gnutls_compression_get_name(gnutls_compression_get(s));
  const char *cipher = gnutls_cipher_get_name(gnutls_cipher_get(s));
  const char *mac = gnutls_mac_get_name(gnutls_mac_get(s));
  log(L_DEBUG, "TLS params: proto=%s kx=%s cert=%s comp=%s cipher=%s mac=%s",
    proto, kx, cert, comp, cipher, mac);
}

int main(int argc UNUSED, char **argv UNUSED)
{
  tls_init();

  int sk = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (sk < 0)
    die("socket: %m");
  int one = 1;
  if (setsockopt(sk, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0)
    die("setsockopt(SO_REUSEADDR): %m");

  struct sockaddr_in sa;
  bzero(&sa, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_addr.s_addr = INADDR_ANY;
  sa.sin_port = htons(port);
  if (bind(sk, (struct sockaddr *) &sa, sizeof(sa)) < 0)
    die("Cannot bind to port %d: %m", port);
  if (listen(sk, 1024) < 0)
    die("Cannot listen on port %d: %m", port);
  log(L_INFO, "Listening on port %d", port);

  for (;;)
    {
      struct sockaddr_in sa2;
      int sa2len = sizeof(sa2);
      int sk2 = accept(sk, (struct sockaddr *) &sa2, &sa2len);
      if (sk2 < 0)
	die("accept: %m");

      byte ipbuf[INET_ADDRSTRLEN];
      inet_ntop(AF_INET, &sa2.sin_addr, ipbuf, sizeof(ipbuf));
      log(L_INFO, "Connection from %s port %d", ipbuf, ntohs(sa2.sin_port));

      gnutls_session_t sess = tls_new_session(sk2);
      int err = gnutls_handshake(sess);
      if (err < 0)
	{
	  log(L_ERROR_R, "Handshake failed: %s", gnutls_strerror(err));
	  goto shut;
	}
      tls_log_params(sess);

      const char *cert_err = tls_verify_cert(sess);
      if (cert_err)
	{
	  log(L_ERROR_R, "Certificate verification failed: %s", cert_err);
	  goto shut;
	}

      for (;;)
	{
	  byte buf[1024];
	  int ret = gnutls_record_recv(sess, buf, sizeof(buf));
	  if (ret < 0)
	    {
	      log(L_ERROR_R, "Connection broken: %s", gnutls_strerror(ret));
	      break;
	    }
	  if (!ret)
	    {
	      log(L_INFO, "Client closed connection");
	      break;
	    }
	  log(L_DEBUG, "Received %d bytes", ret);
	  gnutls_record_send(sess, buf, ret);
	}

      gnutls_bye(sess, GNUTLS_SHUT_WR);
shut:
      close(sk2);
      gnutls_deinit(sess);
    }

  return 0;
}