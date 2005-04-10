/*
   +----------------------------------------------------------------------+
   | PECL :: http                                                         |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.0 of the PHP license, that  |
   | is bundled with this package in the file LICENSE, and is available   |
   | through the world-wide-web at http://www.php.net/license/3_0.txt.    |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
   | Copyright (c) 2004-2005 Michael Wallner <mike@php.net>               |
   +----------------------------------------------------------------------+
*/

/* $Id$ */

#ifdef HAVE_CONFIG_H
#	include "config.h"
#endif

#include <ctype.h>

#include "php.h"
#include "php_output.h"
#include "ext/standard/md5.h"

#include "php_http.h"
#include "php_http_std_defs.h"
#include "php_http_api.h"
#include "php_http_send_api.h"
#include "php_http_cache_api.h"
#include "php_http_headers_api.h"

ZEND_EXTERN_MODULE_GLOBALS(http);

/* char *pretty_key(char *, size_t, zend_bool, zebd_bool) */
char *pretty_key(char *key, size_t key_len, zend_bool uctitle, zend_bool xhyphen)
{
	if (key && key_len) {
		unsigned i, wasalpha;
		if (wasalpha = isalpha(key[0])) {
			key[0] = uctitle ? toupper(key[0]) : tolower(key[0]);
		}
		for (i = 1; i < key_len; i++) {
			if (isalpha(key[i])) {
				key[i] = ((!wasalpha) && uctitle) ? toupper(key[i]) : tolower(key[i]);
				wasalpha = 1;
			} else {
				if (xhyphen && (key[i] == '_')) {
					key[i] = '-';
				}
				wasalpha = 0;
			}
		}
	}
	return key;
}
/* }}} */

/* {{{ static STATUS http_ob_stack_get(php_ob_buffer *, php_ob_buffer **) */
static STATUS http_ob_stack_get(php_ob_buffer *o, php_ob_buffer **s)
{
	static int i = 0;
	php_ob_buffer *b = emalloc(sizeof(php_ob_buffer));
	b->handler_name = estrdup(o->handler_name);
	b->buffer = estrndup(o->buffer, o->text_length);
	b->text_length = o->text_length;
	b->chunk_size = o->chunk_size;
	b->erase = o->erase;
	s[i++] = b;
	return SUCCESS;
}
/* }}} */

/* {{{ zval *http_get_server_var_ex(char *, size_t) */
PHP_HTTP_API zval *_http_get_server_var_ex(const char *key, size_t key_size, zend_bool check TSRMLS_DC)
{
	zval **var;
	if (SUCCESS == zend_hash_find(HTTP_SERVER_VARS,	(char *) key, key_size, (void **) &var)) {
		if (check) {
			return Z_STRVAL_PP(var) && Z_STRLEN_PP(var) ? *var : NULL;
		} else {
			return *var;
		}
	}
	return NULL;
}
/* }}} */

/* {{{ void http_ob_etaghandler(char *, uint, char **, uint *, int) */
PHP_HTTP_API void _http_ob_etaghandler(char *output, uint output_len,
	char **handled_output, uint *handled_output_len, int mode TSRMLS_DC)
{
	char etag[33] = { 0 };
	unsigned char digest[16];

	if (mode & PHP_OUTPUT_HANDLER_START) {
		PHP_MD5Init(&HTTP_G(etag_md5));
	}

	PHP_MD5Update(&HTTP_G(etag_md5), output, output_len);

	if (mode & PHP_OUTPUT_HANDLER_END) {
		PHP_MD5Final(digest, &HTTP_G(etag_md5));

		/* just do that if desired */
		if (HTTP_G(etag_started)) {
			make_digest(etag, digest);

			if (http_etag_match("HTTP_IF_NONE_MATCH", etag)) {
				http_send_status(304);
				zend_bailout();
			} else {
				http_send_etag(etag, 32);
			}
		}
	}

	*handled_output_len = output_len;
	*handled_output = estrndup(output, output_len);
}
/* }}} */

/* {{{ STATUS http_start_ob_handler(php_output_handler_func_t, char *, uint, zend_bool) */
PHP_HTTP_API STATUS _http_start_ob_handler(php_output_handler_func_t handler_func,
	char *handler_name, uint chunk_size, zend_bool erase TSRMLS_DC)
{
	php_ob_buffer **stack;
	int count, i;

	if (count = OG(ob_nesting_level)) {
		stack = ecalloc(count, sizeof(php_ob_buffer *));

		if (count > 1) {
			zend_stack_apply_with_argument(&OG(ob_buffers), ZEND_STACK_APPLY_BOTTOMUP,
				(int (*)(void *elem, void *)) http_ob_stack_get, stack);
		}

		if (count > 0) {
			http_ob_stack_get(&OG(active_ob_buffer), stack);
		}

		while (OG(ob_nesting_level)) {
			php_end_ob_buffer(0, 0 TSRMLS_CC);
		}
	}

	php_ob_set_internal_handler(handler_func, chunk_size, handler_name, erase TSRMLS_CC);

	for (i = 0; i < count; i++) {
		php_ob_buffer *s = stack[i];
		if (strcmp(s->handler_name, "default output handler")) {
			php_start_ob_buffer_named(s->handler_name, s->chunk_size, s->erase TSRMLS_CC);
		}
		php_body_write(s->buffer, s->text_length TSRMLS_CC);
		efree(s->handler_name);
		efree(s->buffer);
		efree(s);
	}
	if (count) {
		efree(stack);
	}

	return SUCCESS;
}
/* }}} */

/* {{{ STATUS http_chunked_decode(char *, size_t, char **, size_t *) */
PHP_HTTP_API STATUS _http_chunked_decode(const char *encoded, size_t encoded_len,
	char **decoded, size_t *decoded_len TSRMLS_DC)
{
	const char *e_ptr;
	char *d_ptr;

	*decoded_len = 0;
	*decoded = ecalloc(1, encoded_len);
	d_ptr = *decoded;
	e_ptr = encoded;

	while (((e_ptr - encoded) - encoded_len) > 0) {
		char hex_len[9] = {0};
		size_t chunk_len = 0;
		int i = 0;

		/* read in chunk size */
		while (isxdigit(*e_ptr)) {
			if (i == 9) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING,
					"Chunk size is too long: 0x%s...", hex_len);
				efree(*decoded);
				return FAILURE;
			}
			hex_len[i++] = *e_ptr++;
		}

		/* reached the end */
		if (!strcmp(hex_len, "0")) {
			break;
		}

		/* new line */
		if (strncmp(e_ptr, HTTP_CRLF, 2)) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING,
				"Invalid character (expected 0x0D 0x0A; got: %x %x)",
				*e_ptr, *(e_ptr + 1));
			efree(*decoded);
			return FAILURE;
		}

		/* hex to long */
		{
			char *error = NULL;
			chunk_len = strtol(hex_len, &error, 16);
			if (error == hex_len) {
				php_error_docref(NULL TSRMLS_CC, E_WARNING,
					"Invalid chunk size string: '%s'", hex_len);
				efree(*decoded);
				return FAILURE;
			}
		}

		memcpy(d_ptr, e_ptr += 2, chunk_len);
		d_ptr += chunk_len;
		e_ptr += chunk_len + 2;
		*decoded_len += chunk_len;
	}

	return SUCCESS;
}
/* }}} */

/* {{{ STATUS http_split_response(zval *, zval *, zval *) */
PHP_HTTP_API STATUS _http_split_response(zval *response, zval *headers, zval *body TSRMLS_DC)
{
	char *b = NULL;
	size_t l = 0;
	STATUS status = http_split_response_ex(Z_STRVAL_P(response), Z_STRLEN_P(response), Z_ARRVAL_P(headers), &b, &l);
	ZVAL_STRINGL(body, b, l, 0);
	return status;
}
/* }}} */

/* {{{ STATUS http_split_response(char *, size_t, HashTable *, char **, size_t *) */
PHP_HTTP_API STATUS _http_split_response_ex(char *response, size_t response_len,
	HashTable *headers, char **body, size_t *body_len TSRMLS_DC)
{
	char *header = response, *real_body = NULL;

	while (0 < (response_len - (response - header + 4))) {
		if (	(*response++ == '\r') &&
				(*response++ == '\n') &&
				(*response++ == '\r') &&
				(*response++ == '\n')) {
			real_body = response;
			break;
		}
	}

	if (real_body && (*body_len = (response_len - (real_body - header)))) {
		*body = ecalloc(1, *body_len + 1);
		memcpy(*body, real_body, *body_len);
	}

	return http_parse_headers_ex(header, real_body ? response_len - *body_len : response_len, headers, 1);
}
/* }}} */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */

