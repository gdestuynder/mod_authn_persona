/* Copyright 1999-2004 The Apache Software Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Based in part, on mod_auth_memcookie, made by Mathieu CARBONNEAUX.
 *
 * See http://authmemcookie.sourceforge.net/ for details;
 * licensed under Apache License, Version 2.0.
 *
 * SHA-1 implementation by Steve Reid, steve@edmweb.com, in
 * public domain.
 */

#include <stdio.h>
#include <string.h>
#define APR_WANT_STRFUNC

#include <apr_want.h>
#include <apr_strings.h>
#include <apr_base64.h>

#include <httpd.h>
#include <http_config.h>
#include <http_core.h>
#include <http_log.h>

#include "cookie.h"
#include "defines.h"

/** Generates a HMAC with the given inputs, returning a Base64-encoded
 * signature value. */
static char *generateHMAC(request_rec *r, const buffer_t *secret,
                          const char *userAddress, const char *issuer,
                          const char *expires)
{
  char *data;
  unsigned char digest[HMAC_DIGESTSIZE];
  char *digest64;

  data = apr_pstrcat(r->pool, userAddress, issuer, expires, NULL);
  hmac(secret->data, secret->len, data, strlen(data), &digest);
  digest64 = apr_palloc(r->pool, apr_base64_encode_len(HMAC_DIGESTSIZE));
  apr_base64_encode(digest64, (char *) digest, HMAC_DIGESTSIZE);

  return digest64;
}

/* Look through the 'Cookie' headers for the indicated cookie; extract it
 * and URL-unescape it. Return the cookie on success, NULL on failure. */
char *extractCookie(request_rec *r, const buffer_t *secret,
                    const char *szCookie_name)
{
  char *szRaw_cookie_start = NULL, *szRaw_cookie_end;
  char *szCookie;
  /* get cookie string */
  char *szRaw_cookie = (char *) apr_table_get(r->headers_in, "Cookie");
  if (!szRaw_cookie)
    return 0;

  /* loop to search cookie name in cookie header */
  do {
    /* search cookie name in cookie string */
    if (!(szRaw_cookie = strstr(szRaw_cookie, szCookie_name)))
      return 0;
    szRaw_cookie_start = szRaw_cookie;
    /* search '=' */
    if (!(szRaw_cookie = strchr(szRaw_cookie, '=')))
      return 0;
  } while (strncmp
           (szCookie_name, szRaw_cookie_start,
            szRaw_cookie - szRaw_cookie_start) != 0);

  /* skip '=' */
  szRaw_cookie++;

  /* search end of cookie name value: ';' or end of cookie strings */
  if (!((szRaw_cookie_end = strchr(szRaw_cookie, ';'))
        || (szRaw_cookie_end = strchr(szRaw_cookie, '\0'))))
    return 0;

  /* dup the value string found in apache pool and set the result pool ptr to szCookie ptr */
  if (!
      (szCookie =
       apr_pstrndup(r->pool, szRaw_cookie, szRaw_cookie_end - szRaw_cookie)))
         return 0;
  /* unescape the value string */
  if (ap_unescape_url(szCookie) != 0)
    return 0;

  return szCookie;
}

/* Check the cookie and make sure it is valid */
Cookie validateCookie(request_rec *r, const buffer_t *secret,
                      const char *szCookieValue)
{

  /* split at | */
  char *iss = NULL;
  char *exp = NULL;
  char *sig = NULL;
  char *addr = apr_strtok((char *) szCookieValue, "|", &iss);
  if (!addr) {
    ap_log_rerror(APLOG_MARK, APLOG_ERR | APLOG_NOERRNO, 0, r,
                  ERRTAG "malformed Persona cookie, can't extract email");
    return NULL;
  }

  iss = apr_strtok((char *) iss, "|", &sig);
  if (!iss) {
    ap_log_rerror(APLOG_MARK, APLOG_ERR | APLOG_NOERRNO, 0, r,
                  ERRTAG "malformed Persona cookie, can't extract issuer");
    return NULL;
  }

  exp = apr_strtok((char *) exp, "|", &sig);
  if (!exp) {
    ap_log_rerror(APLOG_MARK, APLOG_ERR | APLOG_NOERRNO, 0, r,
                  ERRTAG "malformed Persona cookie, can't extract expiry");
    return NULL;
  }

  char *digest64 = generateHMAC(r, secret, addr, iss, exp);

  /* paranoia indicates that we should use a time-invariant compare here */
  if (strcmp(digest64, sig)) {
    ap_log_rerror(APLOG_MARK, APLOG_ERR | APLOG_NOERRNO, 0, r,
                  ERRTAG "invalid Persona cookie, HMAC mismatch (tamper?)");
    return NULL;
  }

  // Verify the cookie is still valid
  apr_time_t expiry;
  apr_time_ansi_put(&expiry, atol(exp));
  if (expiry > 0) {
    apr_time_t now = apr_time_now();
    if (now >= expiry) {
      char when[APR_RFC822_DATE_LEN];
      apr_rfc822_date(when, expiry);
      ap_log_rerror(APLOG_MARK, APLOG_DEBUG | APLOG_NOERRNO, 0, r,
                    ERRTAG "Persona cookie expired on %s", when);
      return NULL;
    }
  }

  Cookie c = apr_pcalloc(r->pool, sizeof(struct _Cookie));
  c->verifiedEmail = addr;
  c->identityIssuer = iss;
  return c;
}

void clearCookie(request_rec *r, const buffer_t *secret,
                 const char *cookie_name, const Cookie cookie)
{
  char *cookie_buf;
  char *domain = "";

  if (cookie->domain) {
    domain = apr_pstrcat(r->pool, "Domain=", cookie->domain, ";", NULL);
  }

  cookie_buf = apr_psprintf(r->pool,
                            "%s=; Path=%s; %sMax-Age=0",
                            cookie_name, cookie->path, domain);
  apr_table_set(r->err_headers_out, "Set-Cookie", cookie_buf);
  apr_table_set(r->headers_out, "Set-Cookie", cookie_buf);

  ap_log_rerror(APLOG_MARK, APLOG_DEBUG | APLOG_NOERRNO, 0, r,
                ERRTAG "Sending cookie payload: %s", cookie_buf);

  return;
}

/** Create a session cookie with a given identity */
void sendSignedCookie(request_rec *r, const buffer_t *secret,
                      const char *cookie_name, const Cookie cookie)
{
  apr_time_t duration;
  char *path = "/";
  char *max_age = "";
  char *domain = "";
  char *expiry = "0";
  char *secure = "";

  if (cookie->path) {
    path = apr_pstrcat(r->pool, "Path=", cookie->path, ";", NULL);
  }

  if (cookie->expires > 0) {
    apr_time_ansi_put(&duration, cookie->expires);
    duration += apr_time_now();

    max_age =
      apr_pstrcat(r->pool, "Max-Age=", apr_itoa(r->pool, cookie->expires),
                  ";", NULL);
    expiry =
      apr_psprintf(r->pool, "%" APR_TIME_T_FMT, apr_time_sec(duration));
  }

  if (cookie->domain) {
    domain = apr_pstrcat(r->pool, "Domain=", cookie->domain, ";", NULL);
  }

  if (cookie->secure) {
    secure = "Secure;";
  }

  char *digest64 =
    generateHMAC(r, secret, cookie->verifiedEmail, cookie->identityIssuer,
                 expiry);

  char *cookie_buf = apr_psprintf(r->pool, "%s=%s|%s|%s|%s",
                                  cookie_name, cookie->verifiedEmail,
                                  cookie->identityIssuer, expiry, digest64);
  char *cookie_flags = apr_psprintf(r->pool, ";HttpOnly;Version=1;%s%s%s%s",
                                    path, domain, max_age, secure);

  char *cookie_payload =
    apr_pstrcat(r->pool, cookie_buf, " ", cookie_flags, NULL);

  ap_log_rerror(APLOG_MARK, APLOG_DEBUG | APLOG_NOERRNO, 0, r,
                ERRTAG "Sending cookie payload: %s", cookie_payload);

  /* syntax of cookie is identity|signature */
  apr_table_set(r->err_headers_out, "Set-Cookie", cookie_payload);
}
