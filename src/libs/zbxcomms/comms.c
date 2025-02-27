/*
** Zabbix
** Copyright (C) 2001-2023 Zabbix SIA
**
** This program is free software; you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation; either version 2 of the License, or
** (at your option) any later version.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
** GNU General Public License for more details.
**
** You should have received a copy of the GNU General Public License
** along with this program; if not, write to the Free Software
** Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
**/

#include "zbxcomms.h"
#include "comms.h"

#if defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
#include "tls.h"
#endif
#include "zbxlog.h"
#include "zbxcompress.h"
#include "zbxstr.h"
#include "zbxnum.h"
#include "zbxip.h"
#include "zbxtime.h"
#include "zbxcrypto.h"

#ifdef _WINDOWS
#	ifndef _WIN32_WINNT_WIN7
#		define _WIN32_WINNT_WIN7		0x0601	/* allow compilation on older Windows systems */
#	endif
#	ifndef WSA_FLAG_NO_HANDLE_INHERIT
#		define WSA_FLAG_NO_HANDLE_INHERIT	0x80	/* allow compilation on older Windows systems */
#	endif
#endif

#ifndef ZBX_SOCKLEN_T
#	define ZBX_SOCKLEN_T socklen_t
#endif

#ifndef SOCK_CLOEXEC
#	define SOCK_CLOEXEC 0	/* SOCK_CLOEXEC is Linux-specific, available since 2.6.23 */
#endif

#ifdef HAVE_OPENSSL
extern ZBX_THREAD_LOCAL char	info_buf[256];
#endif

extern int	CONFIG_TCP_MAX_BACKLOG_SIZE;

static void	tcp_init_hints(struct addrinfo *hints, int socktype, int flags);
static int	socket_set_nonblocking(ZBX_SOCKET s);
static void	tcp_set_socket_strerror_from_getaddrinfo(const char *ip);
static ssize_t	tcp_read(zbx_socket_t *s, char *buffer, size_t size);

zbx_config_tls_t	*zbx_config_tls_new(void)
{
	zbx_config_tls_t	*config_tls;

	config_tls = (zbx_config_tls_t *)zbx_malloc(NULL, sizeof(zbx_config_tls_t));

	config_tls->connect_mode	= ZBX_TCP_SEC_UNENCRYPTED;
	config_tls->accept_modes	= ZBX_TCP_SEC_UNENCRYPTED;

	config_tls->connect		= NULL;
	config_tls->accept		= NULL;
	config_tls->ca_file		= NULL;
	config_tls->crl_file		= NULL;
	config_tls->server_cert_issuer	= NULL;
	config_tls->server_cert_subject	= NULL;
	config_tls->cert_file		= NULL;
	config_tls->key_file		= NULL;
	config_tls->psk_identity	= NULL;
	config_tls->psk_file		= NULL;
	config_tls->cipher_cert13	= NULL;
	config_tls->cipher_cert		= NULL;
	config_tls->cipher_psk13	= NULL;
	config_tls->cipher_psk		= NULL;
	config_tls->cipher_all13	= NULL;
	config_tls->cipher_all		= NULL;
	config_tls->cipher_cmd13	= NULL;
	config_tls->cipher_cmd		= NULL;

	return config_tls;
}

void	zbx_config_tls_free(zbx_config_tls_t *config_tls)
{
	zbx_free(config_tls->connect);
	zbx_free(config_tls->accept);
	zbx_free(config_tls->ca_file);
	zbx_free(config_tls->crl_file);
	zbx_free(config_tls->server_cert_issuer);
	zbx_free(config_tls->server_cert_subject);
	zbx_free(config_tls->cert_file);
	zbx_free(config_tls->key_file);
	zbx_free(config_tls->psk_identity);
	zbx_free(config_tls->psk_file);
	zbx_free(config_tls->cipher_cert13);
	zbx_free(config_tls->cipher_cert);
	zbx_free(config_tls->cipher_psk13);
	zbx_free(config_tls->cipher_psk);
	zbx_free(config_tls->cipher_all13);
	zbx_free(config_tls->cipher_all);
	zbx_free(config_tls->cipher_cmd13);
	zbx_free(config_tls->cipher_cmd);

	zbx_free(config_tls);
}

/******************************************************************************
 *                                                                            *
 * Purpose: return string describing tcp error                                *
 *                                                                            *
 * Return value: pointer to the null terminated string                        *
 *                                                                            *
 ******************************************************************************/

#define ZBX_SOCKET_STRERROR_LEN	512

static ZBX_THREAD_LOCAL char	zbx_socket_strerror_message[ZBX_SOCKET_STRERROR_LEN];

const char	*zbx_socket_strerror(void)
{
	zbx_socket_strerror_message[ZBX_SOCKET_STRERROR_LEN - 1] = '\0';	/* force null termination */
	return zbx_socket_strerror_message;
}

__zbx_attr_format_printf(1, 2)
static void	zbx_set_socket_strerror(const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);

	zbx_vsnprintf(zbx_socket_strerror_message, sizeof(zbx_socket_strerror_message), fmt, args);

	va_end(args);
}

/******************************************************************************
 *                                                                            *
 * Purpose: get peer IP address info from a socket early while it is          *
 *          connected. Connection can be terminated due to various errors at  *
 *          any time and peer IP address will not be available anymore.       *
 *                                                                            *
 * Return value: SUCCEED or FAIL                                              *
 *                                                                            *
 ******************************************************************************/
static int	zbx_socket_peer_ip_save(zbx_socket_t *s)
{
	ZBX_SOCKADDR	sa;
	ZBX_SOCKLEN_T	sz = sizeof(sa);
	char		*error_message = NULL;

	if (ZBX_PROTO_ERROR == getpeername(s->socket, (struct sockaddr *)&sa, &sz))
	{
		error_message = strerror_from_system(zbx_socket_last_error());
		zbx_set_socket_strerror("connection rejected, getpeername() failed: %s", error_message);
		return FAIL;
	}

	/* store getpeername() result to have IP address in numerical form for security check */
	memcpy(&s->peer_info, &sa, (size_t)sz);

	/* store IP address as a text string for error reporting */

#ifdef HAVE_IPV6
	if (0 != zbx_getnameinfo((struct sockaddr *)&sa, s->peer, sizeof(s->peer), NULL, 0, NI_NUMERICHOST))
	{
		error_message = strerror_from_system(zbx_socket_last_error());
		zbx_set_socket_strerror("connection rejected, getnameinfo() failed: %s", error_message);
		return FAIL;
	}
#else
	zbx_strscpy(s->peer, inet_ntoa(sa.sin_addr));
#endif
	return SUCCEED;
}

#ifndef _WINDOWS
/******************************************************************************
 *                                                                            *
 * Purpose: retrieve 'hostent' by IP address                                  *
 *                                                                            *
 ******************************************************************************/
void	zbx_gethost_by_ip(const char *ip, char *host, size_t hostlen)
{
	struct addrinfo	hints, *ai = NULL;

	assert(ip);

	memset(&hints, 0, sizeof(hints));
#ifdef HAVE_IPV6
	hints.ai_family = PF_UNSPEC;
#else
	hints.ai_family = AF_INET;
#endif

	if (0 != getaddrinfo(ip, NULL, &hints, &ai))
	{
		host[0] = '\0';
		goto out;
	}

	if (0 != getnameinfo(ai->ai_addr, ai->ai_addrlen, host, hostlen, NULL, 0, NI_NAMEREQD))
	{
		host[0] = '\0';
		goto out;
	}
out:
	if (NULL != ai)
		freeaddrinfo(ai);
}

/******************************************************************************
 *                                                                            *
 * Purpose: retrieve IP address by host name                                  *
 *                                                                            *
 ******************************************************************************/
void	zbx_getip_by_host(const char *host, char *ip, size_t iplen)
{
	struct addrinfo	hints, *ai = NULL;

	assert(ip);

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = PF_UNSPEC;

	if (0 != getaddrinfo(host, NULL, &hints, &ai))
	{
		ip[0] = '\0';
		goto out;
	}

	switch(ai->ai_addr->sa_family) {
		case AF_INET:
			inet_ntop(AF_INET, &(((struct sockaddr_in *)ai->ai_addr)->sin_addr), ip, iplen);
			break;
		case AF_INET6:
			inet_ntop(AF_INET6, &(((struct sockaddr_in *)ai->ai_addr)->sin_addr), ip, iplen);
			break;
		default:
			ip[0] = '\0';
			goto out;
	}
out:
	if (NULL != ai)
		freeaddrinfo(ai);
}

#endif	/* _WINDOWS */

#ifdef _WINDOWS
/******************************************************************************
 *                                                                            *
 * Purpose: check Windows version                                             *
 *                                                                            *
 * Parameters: major    - [IN] major windows version                          *
 *             minor    - [IN] minor windows version                          *
 *             servpack - [IN] service pack version                           *
 *                                                                            *
 * Return value: SUCCEED - Windows version matches input parameters           *
 *                         or greater                                         *
 *               FAIL    - Windows version is older                           *
 *                                                                            *
 * Comments: This is reimplementation of IsWindowsVersionOrGreater() from     *
 *           Version Helper API. We need it because the original function is  *
 *           only available in newer Windows toolchains (VS2013+)             *
 *                                                                            *
 ******************************************************************************/
static int zbx_is_win_ver_or_greater(zbx_uint32_t major, zbx_uint32_t minor, zbx_uint32_t servpack)
{
	OSVERSIONINFOEXW vi = { sizeof(vi), major, minor, 0, 0, { 0 }, servpack, 0 };

	/* no need to test for an error, check VersionHelpers.h and usage examples */

	return VerifyVersionInfoW(&vi, VER_MAJORVERSION | VER_MINORVERSION | VER_SERVICEPACKMAJOR,
			VerSetConditionMask(VerSetConditionMask(VerSetConditionMask(0,
			VER_MAJORVERSION, VER_GREATER_EQUAL),
			VER_MINORVERSION, VER_GREATER_EQUAL),
			VER_SERVICEPACKMAJOR, VER_GREATER_EQUAL)) ? SUCCEED : FAIL;
}
#endif

/******************************************************************************
 *                                                                            *
 * Purpose: Initialize Windows Sockets APIs                                   *
 *                                                                            *
 * Parameters: error - [OUT] the error message                                *
 *                                                                            *
 * Return value: SUCCEED or FAIL - an error occurred                          *
 *                                                                            *
 ******************************************************************************/
#ifdef _WINDOWS
int	zbx_socket_start(char **error)
{
	WSADATA	sockInfo;
	int	ret;

	if (0 != (ret = WSAStartup(MAKEWORD(2, 2), &sockInfo)))
	{
		*error = zbx_dsprintf(*error, "Cannot initialize Winsock DLL: %s", strerror_from_system(ret));
		return FAIL;
	}

	return SUCCEED;
}
#endif

/******************************************************************************
 *                                                                            *
 * Purpose: initialize socket                                                 *
 *                                                                            *
 ******************************************************************************/
static void	zbx_socket_clean(zbx_socket_t *s)
{
	memset(s, 0, sizeof(zbx_socket_t));

	s->buf_type = ZBX_BUF_TYPE_STAT;
}

/******************************************************************************
 *                                                                            *
 * Purpose: free socket's dynamic buffer                                      *
 *                                                                            *
 ******************************************************************************/
static void	zbx_socket_free(zbx_socket_t *s)
{
	if (ZBX_BUF_TYPE_DYN == s->buf_type)
		zbx_free(s->buffer);
}

/******************************************************************************
 *                                                                            *
 * Purpose: create socket poll error message                                  *
 *                                                                            *
 ******************************************************************************/
char 	*socket_poll_error(short revents)
{
	char	*str = NULL;
	size_t	str_alloc = 0, str_offset = 0;
	char	delim = '(';

	zbx_strcpy_alloc(&str, &str_alloc, &str_offset, "connection error ");

	if (0 != (revents & POLLERR))
	{
		zbx_snprintf_alloc(&str, &str_alloc, &str_offset, "%c%s", delim, "POLLERR");
		delim = ',';
	}

	if (0 != (revents & POLLHUP))
	{
		zbx_snprintf_alloc(&str, &str_alloc, &str_offset, "%c%s", delim, "POLLHUP");
		delim = ',';
	}

	if (0 != (revents & POLLNVAL))
		zbx_snprintf_alloc(&str, &str_alloc, &str_offset, "%c%s", delim, "POLLNVAL");

	zbx_chrcpy_alloc(&str, &str_alloc, &str_offset, ')');

	return str;
}

/******************************************************************************
 *                                                                            *
 * Purpose: connect to the specified address with an optional timeout value   *
 *                                                                            *
 * Parameters: s       - [IN] socket descriptor                               *
 *             addr    - [IN] the address                                     *
 *             addrlen - [IN] the length of addr structure                    *
 *             error   - [OUT] the error message                              *
 *                                                                            *
 * Return value: SUCCEED - connected successfully                             *
 *               FAIL - an error occurred                                     *
 *                                                                            *
 * Comments: Windows connect implementation uses internal timeouts which      *
 *           cannot be changed. Because of that in Windows use nonblocking    *
 *           connect, then wait for connection the specified timeout period   *
 *           and if successful change socket back to blocking mode.           *
 *                                                                            *
 ******************************************************************************/
static int	zbx_socket_connect(zbx_socket_t *s, const struct sockaddr *addr, socklen_t addrlen, char **error)
{
	int		rc;
	zbx_pollfd_t	pd;

	if (ZBX_PROTO_ERROR == connect(s->socket, addr, addrlen) && SUCCEED != zbx_socket_had_nonblocking_error())
	{
		*error = zbx_dsprintf(*error, "cannot connect to address: %s",
				strerror_from_system(zbx_socket_last_error()));
		return FAIL;
	}

	pd.fd = s->socket;
	pd.events = POLLOUT;

	while (0 >= (rc = zbx_socket_poll(&pd, 1, ZBX_SOCKET_POLL_TIMEOUT)))
	{
		if (-1 == rc && SUCCEED != zbx_socket_had_nonblocking_error())
		{
			*error = zbx_strdup(NULL, "cannot wait for connection");
			return FAIL;
		}

		if (SUCCEED != zbx_socket_check_deadline(s))
		{
			*error = zbx_strdup(NULL, "connection timed out");
			return FAIL;
		}
	}

	if (POLLOUT != (pd.revents & (POLLOUT | POLLERR | POLLHUP | POLLNVAL)))
	{
		*error = socket_poll_error(pd.revents);
		zabbix_log(LOG_LEVEL_DEBUG, "poll(POLLOUT) failed with revents 0x%x", (unsigned)pd.revents);
		return FAIL;
	}

	s->connection_type = ZBX_TCP_SEC_UNENCRYPTED;

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Purpose: connect the socket of the specified type to external host         *
 *                                                                            *
 * Parameters: s - [OUT] socket descriptor                                    *
 *                                                                            *
 * Return value: SUCCEED - connected successfully                             *
 *               FAIL - an error occurred                                     *
 *                                                                            *
 ******************************************************************************/
static int	zbx_socket_create(zbx_socket_t *s, int type, const char *source_ip, const char *ip, unsigned short port,
		int timeout, unsigned int tls_connect, const char *tls_arg1, const char *tls_arg2)
{
	int		ret = FAIL, flags;
	struct addrinfo	*ai = NULL, hints;
	struct addrinfo	*ai_bind = NULL;
	char		service[8], *error = NULL;
	void		(*func_socket_close)(zbx_socket_t *s);
#if defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
	const char	*server_name = NULL;
#endif
	zbx_socket_clean(s);

	if (SOCK_DGRAM == type && (ZBX_TCP_SEC_TLS_CERT == tls_connect || ZBX_TCP_SEC_TLS_PSK == tls_connect))
	{
		THIS_SHOULD_NEVER_HAPPEN;
		return FAIL;
	}
#if defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
	if (ZBX_TCP_SEC_TLS_PSK == tls_connect && '\0' == *tls_arg1)
	{
		zbx_set_socket_strerror("cannot connect with PSK: PSK not available");
		return FAIL;
	}
#else
	if (ZBX_TCP_SEC_TLS_CERT == tls_connect || ZBX_TCP_SEC_TLS_PSK == tls_connect)
	{
		zbx_set_socket_strerror("support for TLS was not compiled in");
		return FAIL;
	}
#endif
	s->timeout = timeout;

	if (SUCCEED == zbx_is_ip4(ip))
		flags = AI_NUMERICHOST;
#ifdef HAVE_IPV6
	else if (SUCCEED == zbx_is_ip6(ip))
		flags = AI_NUMERICHOST;
#endif
	else
		flags = 0;

	zbx_snprintf(service, sizeof(service), "%hu", port);
	tcp_init_hints(&hints, type, flags);

	if (0 != getaddrinfo(ip, service, &hints, &ai))
	{
		tcp_set_socket_strerror_from_getaddrinfo(ip);
		goto out;
	}

	if (ZBX_SOCKET_ERROR == (s->socket = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol)))
	{
		zbx_set_socket_strerror("cannot create socket [[%s]:%hu]: %s",
				ip, port, strerror_from_system(zbx_socket_last_error()));
		goto out;
	}

#if !defined(_WINDOWS) && !SOCK_CLOEXEC
	if (-1 == fcntl(s->socket, F_SETFD, FD_CLOEXEC))
	{
		zbx_set_socket_strerror("failed to set the FD_CLOEXEC file descriptor flag on socket [[%s]:%hu]: %s",
				ip, port, strerror_from_system(zbx_socket_last_error()));
	}
#endif
	func_socket_close = (SOCK_STREAM == type ? zbx_tcp_close : zbx_udp_close);

	if (NULL != source_ip)
	{
		tcp_init_hints(&hints, type, AI_NUMERICHOST);

		if (0 != getaddrinfo(source_ip, NULL, &hints, &ai_bind))
		{
			tcp_set_socket_strerror_from_getaddrinfo(source_ip);
			func_socket_close(s);
			goto out;
		}

		if (ZBX_PROTO_ERROR == zbx_bind(s->socket, ai_bind->ai_addr, ai_bind->ai_addrlen))
		{
			zbx_set_socket_strerror("bind() failed: %s", strerror_from_system(zbx_socket_last_error()));
			func_socket_close(s);
			goto out;
		}
	}

	if (SUCCEED != socket_set_nonblocking(s->socket))
	{
		zbx_set_socket_strerror("setting non-blocking mode for [[%s]:%hu] failed: %s",
				NULL != ip ? ip : "-", port,
				strerror_from_system(zbx_socket_last_error()));
		func_socket_close(s);
		goto out;
	}

	zbx_socket_set_deadline(s, timeout);

	if (SUCCEED != zbx_socket_connect(s, ai->ai_addr, (socklen_t)ai->ai_addrlen, &error))
	{
		func_socket_close(s);
		zbx_set_socket_strerror("cannot connect to [[%s]:%hu]: %s", ip, port, error);
		zbx_free(error);
		goto out;
	}

#if defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
	if (NULL != ip && SUCCEED != zbx_is_ip(ip))
	{
		server_name = ip;
	}

	if ((ZBX_TCP_SEC_TLS_CERT == tls_connect || ZBX_TCP_SEC_TLS_PSK == tls_connect) &&
			SUCCEED != zbx_tls_connect(s, tls_connect, tls_arg1, tls_arg2, server_name, &error))
	{
		zbx_tcp_close(s);
		zbx_set_socket_strerror("TCP successful, cannot establish TLS to [[%s]:%hu]: %s", ip, port, error);
		zbx_free(error);
		goto out;
	}
#else
	ZBX_UNUSED(tls_arg1);
	ZBX_UNUSED(tls_arg2);
#endif
	zbx_strlcpy(s->peer, ip, sizeof(s->peer));

	ret = SUCCEED;
out:
	if (NULL != ai)
		freeaddrinfo(ai);

	if (NULL != ai_bind)
		freeaddrinfo(ai_bind);

	return ret;
}

int	zbx_tcp_connect(zbx_socket_t *s, const char *source_ip, const char *ip, unsigned short port, int timeout,
		unsigned int tls_connect, const char *tls_arg1, const char *tls_arg2)
{
	if (ZBX_TCP_SEC_UNENCRYPTED != tls_connect && ZBX_TCP_SEC_TLS_CERT != tls_connect &&
			ZBX_TCP_SEC_TLS_PSK != tls_connect)
	{
		THIS_SHOULD_NEVER_HAPPEN;
		return FAIL;
	}

	return zbx_socket_create(s, SOCK_STREAM, source_ip, ip, port, timeout, tls_connect, tls_arg1, tls_arg2);
}

ssize_t	zbx_tcp_write(zbx_socket_t *s, const char *buf, size_t len)
{
	zbx_pollfd_t	pd;
	ssize_t		n, offset = 0;

#if defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
	if (NULL != s->tls_ctx)	/* TLS connection */
	{
		char	*error = NULL;

		if (ZBX_PROTO_ERROR == (n = zbx_tls_write(s, buf, len, &error)))
		{
			zbx_set_socket_strerror("%s", error);
			zbx_free(error);
		}

		return n;
	}
#endif

	if (0 < (n = ZBX_TCP_WRITE(s->socket, buf, len)) && (size_t)n == len)
		return n;

	pd.fd = s->socket;
	pd.events = POLLOUT;

	while (1)
	{
		if (0 > n)
		{
			int	rc;

			if (SUCCEED != zbx_socket_had_nonblocking_error())
			{
				zbx_set_socket_strerror("cannot write data: %s",
						strerror_from_system(zbx_socket_last_error()));
				return ZBX_PROTO_ERROR;
			}

			if (-1 == (rc = zbx_socket_poll(&pd, 1, ZBX_SOCKET_POLL_TIMEOUT)))
			{
				if (SUCCEED != zbx_socket_had_nonblocking_error())
				{
					zbx_set_socket_strerror("cannot wait for socket: %s",
							strerror_from_system(zbx_socket_last_error()));
					return ZBX_PROTO_ERROR;
				}
			}
			else if (0 != rc && 0 == (pd.revents & POLLOUT))
			{
				char	*errmsg;

				errmsg = socket_poll_error(pd.revents);
				zbx_set_socket_strerror("%s", errmsg);
				zbx_free(errmsg);

				zabbix_log(LOG_LEVEL_DEBUG, "poll(POLLOUT) failed with revents 0x%x",
						(unsigned)pd.revents);

				return ZBX_PROTO_ERROR;
			}
		}
		else
		{
			offset += n;

			if (offset == (ssize_t)len)
				break;
		}

		if (SUCCEED != zbx_socket_check_deadline(s))
		{
			zbx_set_socket_strerror("write timeout");
			return ZBX_PROTO_ERROR;
		}

		n = ZBX_TCP_WRITE(s->socket, buf + offset, (len - (size_t)offset));
	}

	return offset;
}

/******************************************************************************
 *                                                                            *
 * Purpose: send data                                                         *
 *                                                                            *
 * Return value: SUCCEED - success                                            *
 *               FAIL - an error occurred                                     *
 *                                                                            *
 * Comments:                                                                  *
 *     RFC 5246 "The Transport Layer Security (TLS) Protocol. Version 1.2"    *
 *     says: "The record layer fragments information blocks into TLSPlaintext *
 *     records carrying data in chunks of 2^14 bytes or less.".               *
 *                                                                            *
 *     This function combines sending of Zabbix protocol header (5 bytes),    *
 *     data length (8 bytes or 16 bytes for large packet) and at least part   *
 *     of the message into one block of up to 16384 bytes for efficiency.     *
 *     The same is applied for sending unencrypted messages.                  *
 *                                                                            *
 ******************************************************************************/

#define ZBX_TCP_HEADER_DATA	"ZBXD"
#define ZBX_TCP_HEADER_LEN	ZBX_CONST_STRLEN(ZBX_TCP_HEADER_DATA)

int	zbx_tcp_send_ext(zbx_socket_t *s, const char *data, size_t len, size_t reserved, unsigned char flags,
		int timeout)
{
#define ZBX_TLS_MAX_REC_LEN	16384

	ssize_t			bytes_sent, written = 0;
	size_t			send_bytes, offset, send_len = len;
	int			ret = SUCCEED;
	char			*compressed_data = NULL;
	const zbx_uint64_t	max_uint32 = ~(zbx_uint32_t)0;

	if (0 != timeout)
		zbx_socket_set_deadline(s, timeout);

	if (0 != (flags & ZBX_TCP_PROTOCOL))
	{
		size_t	take_bytes;
		char	header_buf[ZBX_TLS_MAX_REC_LEN];	/* Buffer is allocated on stack with a hope that it   */
								/* will be short-lived in CPU cache. Static buffer is */
								/* not used on purpose.				      */

		if (ZBX_MAX_RECV_LARGE_DATA_SIZE < len)
		{
			zbx_set_socket_strerror("cannot send data: message size " ZBX_FS_UI64 " exceeds the maximum"
					" size " ZBX_FS_UI64 " bytes.", len, ZBX_MAX_RECV_LARGE_DATA_SIZE);
			ret = FAIL;
			goto cleanup;
		}

		if (ZBX_MAX_RECV_LARGE_DATA_SIZE < reserved)
		{
			zbx_set_socket_strerror("cannot send data: uncompressed message size " ZBX_FS_UI64
					" exceeds the maximum size " ZBX_FS_UI64 " bytes.", reserved,
					ZBX_MAX_RECV_LARGE_DATA_SIZE);
			ret = FAIL;
			goto cleanup;
		}

		if (0 != (flags & ZBX_TCP_COMPRESS))
		{
			/* compress if not compressed yet */
			if (0 == reserved)
			{
				if (SUCCEED != zbx_compress(data, len, &compressed_data, &send_len))
				{
					zbx_set_socket_strerror("cannot compress data: %s", zbx_compress_strerror());
					ret = FAIL;
					goto cleanup;
				}

				data = compressed_data;
				reserved = len;
			}
		}

		memcpy(header_buf, ZBX_TCP_HEADER_DATA, ZBX_CONST_STRLEN(ZBX_TCP_HEADER_DATA));
		offset = ZBX_CONST_STRLEN(ZBX_TCP_HEADER_DATA);

		if (max_uint32 <= len || max_uint32 <= reserved)
			flags |= ZBX_TCP_LARGE;

		header_buf[offset++] = flags;

		if (0 != (flags & ZBX_TCP_LARGE))
		{
			zbx_uint64_t	len64_le;

			len64_le = zbx_htole_uint64((zbx_uint64_t)send_len);
			memcpy(header_buf + offset, &len64_le, sizeof(len64_le));
			offset += sizeof(len64_le);

			len64_le = zbx_htole_uint64((zbx_uint64_t)reserved);
			memcpy(header_buf + offset, &len64_le, sizeof(len64_le));
			offset += sizeof(len64_le);
		}
		else
		{
			zbx_uint32_t	len32_le;

			len32_le = zbx_htole_uint32((zbx_uint32_t)send_len);
			memcpy(header_buf + offset, &len32_le, sizeof(len32_le));
			offset += sizeof(len32_le);

			len32_le = zbx_htole_uint32((zbx_uint32_t)reserved);
			memcpy(header_buf + offset, &len32_le, sizeof(len32_le));
			offset += sizeof(len32_le);
		}

		take_bytes = MIN(send_len, ZBX_TLS_MAX_REC_LEN - offset);
		memcpy(header_buf + offset, data, take_bytes);

		send_bytes = offset + take_bytes;

		if (ZBX_PROTO_ERROR == (written = zbx_tcp_write(s, header_buf, send_bytes)))
		{
			ret = FAIL;
			goto cleanup;
		}

		written -= (ssize_t)offset;
	}

	while (written < (ssize_t)send_len)
	{
		if (ZBX_TCP_SEC_UNENCRYPTED == s->connection_type)
			send_bytes = send_len - (size_t)written;
		else
			send_bytes = MIN(ZBX_TLS_MAX_REC_LEN, send_len - (size_t)written);

		if (ZBX_PROTO_ERROR == (bytes_sent = zbx_tcp_write(s, data + written, send_bytes)))
		{
			ret = FAIL;
			goto cleanup;
		}
		written += bytes_sent;
	}
cleanup:
	zbx_free(compressed_data);

	if (0 != timeout)
		zbx_socket_set_deadline(s, 0);

	return ret;

#undef ZBX_TLS_MAX_REC_LEN
}

/******************************************************************************
 *                                                                            *
 * Purpose: close open TCP socket                                             *
 *                                                                            *
 ******************************************************************************/
void	zbx_tcp_close(zbx_socket_t *s)
{
	zbx_tcp_unaccept(s);

	zbx_socket_free(s);
	zbx_socket_close(s->socket);
}

/******************************************************************************
 *                                                                            *
 * Purpose: return address family                                             *
 *                                                                            *
 * Parameters: addr - [IN] address or hostname                                *
 *             family - [OUT] address family                                  *
 *             error - [OUT] error string                                     *
 *             max_error_len - [IN] error string length                       *
 *                                                                            *
 * Return value: SUCCEED - success                                            *
 *               FAIL - an error occurred                                     *
 *                                                                            *
 ******************************************************************************/
#ifdef HAVE_IPV6
int	get_address_family(const char *addr, int *family, char *error, int max_error_len)
{
	struct addrinfo	hints, *ai = NULL;
	int		err, res = FAIL;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = PF_UNSPEC;
	hints.ai_flags = 0;
	hints.ai_socktype = SOCK_STREAM;

	if (0 != (err = getaddrinfo(addr, NULL, &hints, &ai)))
	{
		zbx_snprintf(error, max_error_len, "%s: [%d] %s", addr, err, gai_strerror(err));
		goto out;
	}

	if (PF_INET != ai->ai_family && PF_INET6 != ai->ai_family)
	{
		zbx_snprintf(error, max_error_len, "%s: unsupported address family", addr);
		goto out;
	}

	*family = (int)ai->ai_family;

	res = SUCCEED;
out:
	if (NULL != ai)
		freeaddrinfo(ai);

	return res;
}
#endif	/* HAVE_IPV6 */

static void	tcp_set_socket_strerror_from_getaddrinfo(const char *ip)
{
#if defined(_WINDOWS)
		zbx_set_socket_strerror("getaddrinfo() failed for '%s': %s",
				ip, strerror_from_system(WSAGetLastError()));
#else
#if defined(HAVE_HSTRERROR)
		zbx_set_socket_strerror("getaddrinfo() failed for '%s': [%d] %s",
				ip, h_errno, hstrerror(h_errno));
#else
		zbx_set_socket_strerror("getaddrinfo() failed for '%s': [%d]",
				ip, h_errno);
#endif
#endif
}

/******************************************************************************
 *                                                                            *
 * Purpose: initialize hints for getaddrinfo() call                           *
 *                                                                            *
 ******************************************************************************/
static void	tcp_init_hints(struct addrinfo *hints, int socktype, int flags)
{
	memset(hints, 0, sizeof(struct addrinfo));

#if defined(HAVE_IPV6)
	hints->ai_family = PF_UNSPEC;
#else
	hints->ai_family =  PF_INET;
#endif
	hints->ai_socktype = socktype;
	hints->ai_flags = flags;
}

/******************************************************************************
 *                                                                            *
 * Purpose: set non-blocking socket operation                                 *
 *                                                                            *
 ******************************************************************************/
static int	socket_set_nonblocking(ZBX_SOCKET s)
{
#if defined(_WINDOWS)
	u_long	value = 1;

	if (0 != ioctlsocket(s, FIONBIO, (unsigned long*)&value))
		return FAIL;
#else
	if (-1 == fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK))
		return FAIL;
#endif
	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Purpose: check if the last socket error was because of non-blocking socket *
 *                                                                            *
 ******************************************************************************/
int	zbx_socket_had_nonblocking_error(void)
{
#ifndef _WINDOWS
	switch (errno)
	{
		case EINTR:
		case EAGAIN:
		case EINPROGRESS:
			return SUCCEED;
		default:
			return FAIL;
	}
#else
	switch (WSAGetLastError())
	{
		case 0:
		case WSAEINPROGRESS:
		case WSAEWOULDBLOCK:
			return SUCCEED;
		default:
			return FAIL;
	}
#endif
}

#if defined(_WINDOWS)

/******************************************************************************
 *                                                                            *
 * Purpose: poll() function emulation for windows                             *
 *                                                                            *
 * Comments: WSAPoll() does not fully behave like poll() and also is not      *
 *           supported on older (xp64/server2003) systems                     *
 *                                                                            *
 ******************************************************************************/
int	zbx_socket_poll(zbx_pollfd_t* fds, unsigned long fds_num, int timeout)
{
	fd_set		fds_read;
	fd_set		fds_write;
	fd_set		fds_err;
	int		ret;
	unsigned long	i;
	struct timeval	tv;

	FD_ZERO(&fds_read);
	FD_ZERO(&fds_write);
	FD_ZERO(&fds_err);

	for (i = 0; i < fds_num; i++)
	{
		fds[i].revents = 0;

		if (fds[i].events & (POLLRDNORM | POLLIN))
			FD_SET(fds[i].fd, &fds_read);

		if (fds[i].events & (POLLWRNORM | POLLOUT))
			FD_SET(fds[i].fd, &fds_write);

		FD_SET(fds[i].fd, &fds_err);
	}

	tv.tv_sec = timeout / 1000;
	tv.tv_usec = (timeout % 1000) * 1000;

	if (0 >= (ret = select(0, &fds_read, &fds_write, &fds_err, &tv)))
		return ret;

	ret = 0;

	for (i = 0; i < fds_num; i++)
	{
		if (FD_ISSET(fds[i].fd, &fds_read))
			fds[i].revents |= (fds[i].events & (POLLRDNORM | POLLIN));

		if (FD_ISSET(fds[i].fd, &fds_write))
			fds[i].revents |= (fds[i].events & (POLLWRNORM | POLLOUT));

		if (FD_ISSET(fds[i].fd, &fds_err))
			fds[i].revents = POLLERR;

		if (0 != fds[i].revents)
			ret++;
	}

	return ret;
}

#endif

/******************************************************************************
 *                                                                            *
 * Purpose: inspect data in socket buffer without reading it                  *
 *                                                                            *
 ******************************************************************************/
static ssize_t	tcp_peek(zbx_socket_t *s, char *buffer, size_t size)
{
	ssize_t		n;
	zbx_pollfd_t	pd;

	if (0 <= (n = ZBX_TCP_RECV(s->socket, buffer, size, MSG_PEEK)))
		return n;

	if (SUCCEED != zbx_socket_had_nonblocking_error())
		return FAIL;

	pd.fd = s->socket;
	pd.events = POLLIN;

	while (1)
	{
		int	rc;

		if (-1 == (rc = zbx_socket_poll(&pd, 1, ZBX_SOCKET_POLL_TIMEOUT)))
		{
			if (SUCCEED != zbx_socket_had_nonblocking_error())
				return FAIL;
		}

		if (0 >= rc)
		{
			if (SUCCEED != zbx_socket_check_deadline(s))
				return TIMEOUT_ERROR;

			continue;
		}

		if (0 == (pd.revents & POLLIN))
			return FAIL;

		if (0 <= (n = ZBX_TCP_RECV(s->socket, buffer, size, MSG_PEEK)))
			break;

		if (SUCCEED != zbx_socket_had_nonblocking_error())
			return FAIL;
	}

	return n;
}

/******************************************************************************
 *                                                                            *
 * Purpose: read data from socket                                             *
 *                                                                            *
 ******************************************************************************/
static ssize_t	tcp_read(zbx_socket_t *s, char *buffer, size_t size)
{
	ssize_t		n;
	zbx_pollfd_t	pd;

	if (0 <= (n = ZBX_TCP_READ(s->socket, buffer, size)))
		return n;

	if (SUCCEED != zbx_socket_had_nonblocking_error())
	{
		zbx_set_socket_strerror("cannot read from socket: %s",
				strerror_from_system(zbx_socket_last_error()));
		return ZBX_PROTO_ERROR;
	}

	pd.fd = s->socket;
	pd.events = POLLIN;

	while (1)
	{
		int	rc;

		if (-1 == (rc = zbx_socket_poll(&pd, 1, ZBX_SOCKET_POLL_TIMEOUT)))
		{
			if (SUCCEED != zbx_socket_had_nonblocking_error())
			{
				zbx_set_socket_strerror("cannot wait for socket: %s",
						strerror_from_system(zbx_socket_last_error()));
				return ZBX_PROTO_ERROR;
			}
		}

		if (SUCCEED != zbx_socket_check_deadline(s))
		{
			zbx_set_socket_strerror("read timeout");
			return ZBX_PROTO_ERROR;
		}

		if (0 >= rc)
			continue;

		if (0 == (pd.revents & POLLIN))
		{
			char	*errmsg;

			errmsg = socket_poll_error(pd.revents);
			zbx_set_socket_strerror("%s", errmsg);
			zbx_free(errmsg);

			zabbix_log(LOG_LEVEL_DEBUG, "poll(POLLIN) failed with revents 0x%x", (unsigned)pd.revents);

			return ZBX_PROTO_ERROR;
		}

		if (0 <= (n = ZBX_TCP_READ(s->socket, buffer, size)))
			break;

		if (SUCCEED != zbx_socket_had_nonblocking_error())
		{
			zbx_set_socket_strerror("cannot read from socket: %s",
					strerror_from_system(zbx_socket_last_error()));
			return ZBX_PROTO_ERROR;
		}
	}

	return n;
}

static int	tcp_err_in_use(void)
{
#if defined(_WINDOWS)
	return WSAEADDRINUSE == zbx_socket_last_error() ? SUCCEED : FAIL;
#else
	return EADDRINUSE == zbx_socket_last_error() ? SUCCEED : FAIL;
#endif
}

/******************************************************************************
 *                                                                            *
 * Purpose: create socket for listening                                       *
 *                                                                            *
 * Return value: SUCCEED - success                                            *
 *               FAIL - an error occurred                                     *
 *                                                                            *
 ******************************************************************************/
int	zbx_tcp_listen(zbx_socket_t *s, const char *listen_ip, unsigned short listen_port, int timeout)
{
	struct addrinfo	hints, *ai = NULL, *current_ai;
	char		port[8], *ip, *ips, *delim;
	int		i, err, on = 1, ret = FAIL;

#if defined(_WINDOWS)
	/* WSASocket() option to prevent inheritance is available on */
	/* Windows Server 2008 R2 SP1 or newer and on Windows 7 SP1 or newer */
	static ZBX_THREAD_LOCAL int	no_inherit_wsapi = -1;

	if (-1 == no_inherit_wsapi)
	{
		/* Both Windows 7 and Windows 2008 R2 are 0x0601 */
		no_inherit_wsapi = zbx_is_win_ver_or_greater((_WIN32_WINNT_WIN7 >> 8) & 0xff,
				_WIN32_WINNT_WIN7 & 0xff, 1) == SUCCEED;
	}
#endif

	zbx_socket_clean(s);
	s->timeout = timeout;

	tcp_init_hints(&hints, SOCK_STREAM, AI_NUMERICHOST | AI_PASSIVE);
	zbx_snprintf(port, sizeof(port), "%hu", listen_port);

	ip = ips = (NULL == listen_ip ? NULL : strdup(listen_ip));

	while (1)
	{
		delim = (NULL == ip ? NULL : strchr(ip, ','));
		if (NULL != delim)
			*delim = '\0';

		if (0 != (err = getaddrinfo(ip, port, &hints, &ai)))
		{
			zbx_set_socket_strerror("cannot resolve address [[%s]:%s]: [%d] %s",
					NULL != ip ? ip : "-", port, err, gai_strerror(err));
			goto out;
		}

		for (current_ai = ai; NULL != current_ai; current_ai = current_ai->ai_next)
		{
			if (ZBX_SOCKET_COUNT == s->num_socks)
			{
				zbx_set_socket_strerror("not enough space for socket [[%s]:%s]",
						NULL != ip ? ip : "-", port);
				goto out;
			}

			if (PF_INET != current_ai->ai_family && PF_INET6 != current_ai->ai_family)
				continue;

#if defined(_WINDOWS)
			/* WSA_FLAG_NO_HANDLE_INHERIT prevents socket inheritance if we call CreateProcess() */
			/* later on. If it's not available we still try to avoid inheritance by calling  */
			/* SetHandleInformation() below. WSA_FLAG_OVERLAPPED is not mandatory but strongly */
			/* recommended for every socket */
			s->sockets[s->num_socks] = WSASocket(current_ai->ai_family, current_ai->ai_socktype,
					current_ai->ai_protocol, NULL, 0,
					(0 != no_inherit_wsapi ? WSA_FLAG_NO_HANDLE_INHERIT : 0) |
					WSA_FLAG_OVERLAPPED);
			if (ZBX_SOCKET_ERROR == s->sockets[s->num_socks])
			{
				zbx_set_socket_strerror("WSASocket() for [[%s]:%s] failed: %s",
						NULL != ip ? ip : "-", port,
						strerror_from_system(zbx_socket_last_error()));

				if (WSAEAFNOSUPPORT == zbx_socket_last_error())
					continue;

				goto out;
			}

			/* If WSA_FLAG_NO_HANDLE_INHERIT not available, prevent listening socket from */
			/* inheritance with the old API. Disabling handle inheritance in WSASocket() instead of */
			/* SetHandleInformation() is preferred because it provides atomicity and gets the job done */
			/* on systems with non-IFS LSPs installed. So there is a chance that the socket will be still */
			/* inherited on Windows XP with 3rd party firewall/antivirus installed */
			if (0 == no_inherit_wsapi && 0 == SetHandleInformation((HANDLE)s->sockets[s->num_socks],
					HANDLE_FLAG_INHERIT, 0))
			{
				zabbix_log(LOG_LEVEL_WARNING, "SetHandleInformation() failed: %s",
						strerror_from_system(GetLastError()));
			}

			/* prevent other processes from binding to the same port */
			/* SO_EXCLUSIVEADDRUSE is mutually exclusive with SO_REUSEADDR */
			/* on Windows SO_REUSEADDR has different semantics than on Unix */
			/* https://msdn.microsoft.com/en-us/library/windows/desktop/ms740621(v=vs.85).aspx */
			if (ZBX_PROTO_ERROR == setsockopt(s->sockets[s->num_socks], SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
					(void *)&on, sizeof(on)))
			{
				zbx_set_socket_strerror("setsockopt() with %s for [[%s]:%s] failed: %s",
						"SO_EXCLUSIVEADDRUSE", NULL != ip ? ip : "-", port,
						strerror_from_system(zbx_socket_last_error()));
			}

#else
			if (ZBX_SOCKET_ERROR == (s->sockets[s->num_socks] =
					socket(current_ai->ai_family, current_ai->ai_socktype | SOCK_CLOEXEC,
					current_ai->ai_protocol)))
			{
				zbx_set_socket_strerror("socket() for [[%s]:%s] failed: %s",
						NULL != ip ? ip : "-", port,
						strerror_from_system(zbx_socket_last_error()));

				if (EAFNOSUPPORT == zbx_socket_last_error())
					continue;

				goto out;
			}

#	if !SOCK_CLOEXEC
			if (-1 == fcntl(s->sockets[s->num_socks], F_SETFD, FD_CLOEXEC))
			{
				zbx_set_socket_strerror("failed to set the FD_CLOEXEC file descriptor flag on "
						"socket [[%s]:%s]: %s", NULL != ip ? ip : "-", port,
						strerror_from_system(zbx_socket_last_error()));
			}
#	endif

			/* enable address reuse */
			/* this is to immediately use the address even if it is in TIME_WAIT state */
			/* http://www-128.ibm.com/developerworks/linux/library/l-sockpit/index.html */
			if (ZBX_PROTO_ERROR == setsockopt(s->sockets[s->num_socks], SOL_SOCKET, SO_REUSEADDR,
					(void *)&on, sizeof(on)))
			{
				zbx_set_socket_strerror("setsockopt() with %s for [[%s]:%s] failed: %s",
						"SO_REUSEADDR", NULL != ip ? ip : "-", port,
						strerror_from_system(zbx_socket_last_error()));
			}
#endif

#if defined(IPPROTO_IPV6) && defined(IPV6_V6ONLY)
			if (PF_INET6 == current_ai->ai_family &&
					ZBX_PROTO_ERROR == setsockopt(s->sockets[s->num_socks], IPPROTO_IPV6,
					IPV6_V6ONLY, (void *)&on, sizeof(on)))
			{
				zbx_set_socket_strerror("setsockopt() with %s for [[%s]:%s] failed: %s",
						"IPV6_V6ONLY", NULL != ip ? ip : "-", port,
						strerror_from_system(zbx_socket_last_error()));
			}
#endif
			if (ZBX_PROTO_ERROR == zbx_bind(s->sockets[s->num_socks], current_ai->ai_addr,
								current_ai->ai_addrlen))
			{
				zbx_set_socket_strerror("bind() for [[%s]:%s] failed: %s",
						NULL != ip ? ip : "-", port,
						strerror_from_system(zbx_socket_last_error()));
				zbx_socket_close(s->sockets[s->num_socks]);

				if (SUCCEED == tcp_err_in_use())
					continue;
				else
					goto out;
			}

			if (ZBX_PROTO_ERROR == listen(s->sockets[s->num_socks], CONFIG_TCP_MAX_BACKLOG_SIZE))
			{
				zbx_set_socket_strerror("listen() for [[%s]:%s] failed: %s",
						NULL != ip ? ip : "-", port,
						strerror_from_system(zbx_socket_last_error()));
				zbx_socket_close(s->sockets[s->num_socks]);
				goto out;
			}

			if (SUCCEED != socket_set_nonblocking(s->sockets[s->num_socks]))
			{
				zbx_set_socket_strerror("setting non-blocking mode for [[%s]:%s] failed: %s",
						NULL != ip ? ip : "-", port,
						strerror_from_system(zbx_socket_last_error()));
				zbx_socket_close(s->sockets[s->num_socks]);
				goto out;
			}

			s->num_socks++;
		}

		if (NULL != ai)
		{
			freeaddrinfo(ai);
			ai = NULL;
		}

		if (NULL == ip || NULL == delim)
			break;

		*delim = ',';
		ip = delim + 1;
	}

	if (0 == s->num_socks)
	{
		zbx_set_socket_strerror("zbx_tcp_listen() fatal error: unable to serve on any address [[%s]:%hu]",
				NULL != listen_ip ? listen_ip : "-", listen_port);
		goto out;
	}

	ret = SUCCEED;
out:
	if (NULL != ips)
		zbx_free(ips);

	if (NULL != ai)
		freeaddrinfo(ai);

	if (SUCCEED != ret)
	{
		for (i = 0; i < s->num_socks; i++)
			zbx_socket_close(s->sockets[i]);
	}

	return ret;
}

void	zbx_tcp_unlisten(zbx_socket_t *s)
{
	int	i;

	for (i = 0; i < s->num_socks; i++)
		zbx_socket_close(s->sockets[i]);
}

/******************************************************************************
 *                                                                            *
 * Purpose: permits an incoming connection attempt on a socket                *
 *                                                                            *
 * Parameters: s              - [IN/OUT] socket to listen                     *
 *             tls_accept     - [IN] TLS configuration                        *
 *             poll_timeout   - [IN] milliseconds to wait for connection      *
 *                                  (0 - don't wait, -1 - wait forever        *
 *                                                                            *
 * Return value: SUCCEED       - success                                      *
 *               FAIL          - an error occurred                            *
 *               TIMEOUT_ERROR - no connections for the timeout period        *
 *                                                                            *
 ******************************************************************************/
int	zbx_tcp_accept(zbx_socket_t *s, unsigned int tls_accept, int poll_timeout)
{
	ZBX_SOCKADDR	serv_addr;
	ZBX_SOCKET	accepted_socket;
	ZBX_SOCKLEN_T	nlen;
	int		i, ret = FAIL;
	ssize_t		res;
	char		buf;	/* 1 byte buffer */
	zbx_pollfd_t	*pds;

	zbx_tcp_unaccept(s);

	pds = (zbx_pollfd_t *)zbx_malloc(NULL, sizeof(zbx_pollfd_t) * (size_t)s->num_socks);

	for (i = 0; i < s->num_socks; i++)
	{
		pds[i].fd = s->sockets[i];
		pds[i].events = POLLIN;
	}

	if (ZBX_PROTO_ERROR == (ret = zbx_socket_poll(pds, (unsigned long)s->num_socks, poll_timeout * 1000)))
	{
		if (SUCCEED == zbx_socket_had_nonblocking_error())
			ret = TIMEOUT_ERROR;
		else
			zbx_set_socket_strerror("poll() failed: %s", strerror_from_system(zbx_socket_last_error()));

		goto out;
	}

	if (0 == ret)
	{
		ret = TIMEOUT_ERROR;
		goto out;
	}

	for (i = 0; i < s->num_socks; i++)
	{
		if (0 != (pds[i].revents & POLLIN))
			break;
	}

	if (i == s->num_socks)
	{
		zbx_set_socket_strerror("incoming connection has failed");
		goto out;
	}

	/* Since this socket was returned by poll, we know we have */
	/* a connection waiting and that this accept() will not block. */
	nlen = sizeof(serv_addr);
	if (ZBX_SOCKET_ERROR == (accepted_socket = (ZBX_SOCKET)accept(s->sockets[i], (struct sockaddr *)&serv_addr,
			&nlen)))
	{
		if (SUCCEED == zbx_socket_had_nonblocking_error())
			ret = TIMEOUT_ERROR;
		else
			zbx_set_socket_strerror("accept() failed: %s", strerror_from_system(zbx_socket_last_error()));

		goto out;
	}

	s->socket_orig = s->socket;	/* remember main socket */
	s->socket = accepted_socket;	/* replace socket to accepted */
	s->accepted = 1;

	if (SUCCEED != socket_set_nonblocking(accepted_socket))
	{
		zbx_set_socket_strerror("failed to set socket non-blocking mode: %s",
				strerror_from_system(zbx_socket_last_error()));
		zbx_tcp_unaccept(s);
		goto out;
	}

	if (SUCCEED != zbx_socket_peer_ip_save(s))
	{
		/* cannot get peer IP address */
		zbx_tcp_unaccept(s);
		goto out;
	}

	zbx_socket_set_deadline(s, s->timeout);

	if (FAIL == (res = tcp_peek(s, &buf, 1)) || TIMEOUT_ERROR == res)
	{
		zbx_set_socket_strerror("from %s: reading first byte from connection failed: %s", s->peer,
				strerror_from_system(zbx_socket_last_error()));
		zbx_tcp_unaccept(s);
		goto out;
	}

	/* if the 1st byte is 0x16 then assume it's a TLS connection */
	if (1 == res && '\x16' == buf)
	{
#if defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
		if (0 != (tls_accept & (ZBX_TCP_SEC_TLS_CERT | ZBX_TCP_SEC_TLS_PSK)))
		{
			char	*error = NULL;

			if (SUCCEED != zbx_tls_accept(s, tls_accept, &error))
			{
				zbx_set_socket_strerror("from %s: %s", s->peer, error);
				zbx_tcp_unaccept(s);
				zbx_free(error);
				goto out;
			}
		}
		else
		{
			zbx_set_socket_strerror("from %s: TLS connections are not allowed", s->peer);
			zbx_tcp_unaccept(s);
			goto out;
		}
#else
		zbx_set_socket_strerror("from %s: support for TLS was not compiled in", s->peer);
		zbx_tcp_unaccept(s);
		goto out;
#endif
	}
	else
	{
		if (0 == (tls_accept & ZBX_TCP_SEC_UNENCRYPTED))
		{
			zbx_set_socket_strerror("from %s: unencrypted connections are not allowed", s->peer);
			zbx_tcp_unaccept(s);
			goto out;
		}

		s->connection_type = ZBX_TCP_SEC_UNENCRYPTED;
	}

	zbx_socket_set_deadline(s, 0);

	ret = SUCCEED;
out:
	zbx_free(pds);

	return ret;
}

/******************************************************************************
 *                                                                            *
 * Purpose: close accepted connection                                         *
 *                                                                            *
 ******************************************************************************/
void	zbx_tcp_unaccept(zbx_socket_t *s)
{
#if defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
	zbx_tls_close(s);
#endif
	if (!s->accepted) return;

	shutdown(s->socket, 2);

	zbx_socket_free(s);
	zbx_socket_close(s->socket);

	s->socket = s->socket_orig;	/* restore main socket */
	s->socket_orig = ZBX_SOCKET_ERROR;
	s->accepted = 0;
}

/******************************************************************************
 *                                                                            *
 * Purpose: finds the next line in socket data buffer                         *
 *                                                                            *
 * Parameters: s - [IN] the socket                                            *
 *                                                                            *
 * Return value: A pointer to the next line or NULL if the socket data buffer *
 *               contains no more lines.                                      *
 *                                                                            *
 ******************************************************************************/
static const char	*zbx_socket_find_line(zbx_socket_t *s)
{
	char	*ptr, *line = NULL;

	if (NULL == s->next_line)
		return NULL;

	/* check if the buffer contains the next line */
	if ((size_t)(s->next_line - s->buffer) <= s->read_bytes && NULL != (ptr = strchr(s->next_line, '\n')))
	{
		line = s->next_line;
		s->next_line = ptr + 1;

		if (ptr > line && '\r' == *(ptr - 1))
			ptr--;

		*ptr = '\0';
	}

	return line;
}

/******************************************************************************
 *                                                                            *
 * Purpose: reads next line from a socket                                     *
 *                                                                            *
 * Parameters: s - [IN] the socket                                            *
 *                                                                            *
 * Return value: a pointer to the line in socket buffer or NULL if there are  *
 *               no more lines (socket was closed or an error occurred)       *
 *                                                                            *
 * Comments: Lines larger than 64KB are truncated.                            *
 *                                                                            *
 ******************************************************************************/
const char	*zbx_tcp_recv_line(zbx_socket_t *s)
{
#define ZBX_TCP_LINE_LEN	(64 * ZBX_KIBIBYTE)

	char		buffer[ZBX_STAT_BUF_LEN], *ptr = NULL;
	const char	*line;
	ssize_t		nbytes;
	size_t		alloc = 0, offset = 0, line_length, left;

	/* check if the buffer already contains the next line */
	if (NULL != (line = zbx_socket_find_line(s)))
		return line;

	/* Find the size of leftover data from the last read line operation and copy */
	/* the leftover data to the static buffer and reset the dynamic buffer.      */
	/* Because we are reading data in ZBX_STAT_BUF_LEN chunks the leftover       */
	/* data will always fit the static buffer.                                   */
	if (NULL != s->next_line)
	{
		left = (size_t)(s->read_bytes - (size_t)(s->next_line - s->buffer));
		memmove(s->buf_stat, s->next_line, left);
	}
	else
		left = 0;

	s->read_bytes = left;
	s->next_line = s->buf_stat;

	zbx_socket_free(s);
	s->buf_type = ZBX_BUF_TYPE_STAT;
	s->buffer = s->buf_stat;

	/* read more data into static buffer */
	if (ZBX_PROTO_ERROR == (nbytes = tcp_read(s, s->buf_stat + left, ZBX_STAT_BUF_LEN - left - 1)))
		goto out;

	s->buf_stat[left + (size_t)nbytes] = '\0';

	if (0 == nbytes)
	{
		/* Socket was closed before newline was found. If we have data in buffer  */
		/* return it with success. Otherwise return failure.                      */
		line = 0 != s->read_bytes ? s->next_line : NULL;
		s->next_line += s->read_bytes;

		goto out;
	}

	s->read_bytes += (size_t)nbytes;

	/* check if the static buffer now contains the next line */
	if (NULL != (line = zbx_socket_find_line(s)))
		goto out;

	/* copy the static buffer data into dynamic buffer */
	s->buf_type = ZBX_BUF_TYPE_DYN;
	s->buffer = NULL;
	zbx_strncpy_alloc(&s->buffer, &alloc, &offset, s->buf_stat, s->read_bytes);
	line_length = s->read_bytes;

	/* Read data into dynamic buffer until newline has been found. */
	/* Lines larger than ZBX_TCP_LINE_LEN bytes will be truncated. */
	do
	{
		if (ZBX_PROTO_ERROR == (nbytes = tcp_read(s, buffer, ZBX_STAT_BUF_LEN - 1)))
			goto out;

		if (0 == nbytes)
		{
			/* socket was closed before newline was found, just return the data we have */
			line = 0 != s->read_bytes ? s->buffer : NULL;
			s->next_line = s->buffer + s->read_bytes;

			goto out;
		}

		buffer[nbytes] = '\0';
		ptr = strchr(buffer, '\n');

		if (s->read_bytes + (size_t)nbytes < ZBX_TCP_LINE_LEN && s->read_bytes == line_length)
		{
			zbx_strncpy_alloc(&s->buffer, &alloc, &offset, buffer, (size_t)nbytes);
			s->read_bytes += (size_t)nbytes;
		}
		else
		{
			if (0 != (left = (NULL == ptr ? ZBX_TCP_LINE_LEN - s->read_bytes :
					MIN(ZBX_TCP_LINE_LEN - s->read_bytes, (size_t)(ptr - buffer)))))
			{
				/* fill the string to the defined limit */
				zbx_strncpy_alloc(&s->buffer, &alloc, &offset, buffer, left);
				s->read_bytes += left;
			}

			/* if the line exceeds the defined limit then truncate it by skipping data until the newline */
			if (NULL != ptr)
			{
				zbx_strncpy_alloc(&s->buffer, &alloc, &offset, ptr, (size_t)(nbytes - (ptr - buffer)));
				s->read_bytes += (size_t)(nbytes - (ptr - buffer));
			}
		}

		line_length += (size_t)nbytes;

	}
	while (NULL == ptr);

	s->next_line = s->buffer;
	line = zbx_socket_find_line(s);
out:
	return line;
}

ssize_t	zbx_tcp_read(zbx_socket_t *s, char *buf, size_t len)
{
#if defined(HAVE_GNUTLS) || defined(HAVE_OPENSSL)
	ssize_t	res;

	if (NULL != s->tls_ctx)	/* TLS connection */
	{
		char	*error = NULL;

		if (ZBX_PROTO_ERROR == (res = zbx_tls_read(s, buf, len, &error)))
		{
			zbx_set_socket_strerror("%s", error);
			zbx_free(error);
		}

		return res;
	}
#endif
	return tcp_read(s, buf, len);
}

/******************************************************************************
 *                                                                            *
 * Purpose: sets deadline for socket operations                               *
 *                                                                            *
 ******************************************************************************/
void	zbx_socket_set_deadline(zbx_socket_t *s, int timeout)
{
	if (0 == timeout)
	{
		s->deadline.sec = 0;
		s->deadline.ns = 0;
		return;
	}

	zbx_ts_get_deadline(&s->deadline, timeout);
}

/******************************************************************************
 *                                                                            *
 * Purpose: check if deadline has not been reached                            *
 *                                                                            *
 ******************************************************************************/
int	zbx_socket_check_deadline(zbx_socket_t *s)
{
	if (0 == s->deadline.sec)
		return SUCCEED;

	return zbx_ts_check_deadline(&s->deadline);
}

/******************************************************************************
 *                                                                            *
 * Purpose: receive data                                                      *
 *                                                                            *
 * Return value: number of bytes received - success,                          *
 *               FAIL - an error occurred                                     *
 *                                                                            *
 ******************************************************************************/
ssize_t	zbx_tcp_recv_ext(zbx_socket_t *s, int timeout, unsigned char flags)
{
#define ZBX_TCP_EXPECT_HEADER		1
#define ZBX_TCP_EXPECT_VERSION		2
#define ZBX_TCP_EXPECT_VERSION_VALIDATE	3
#define ZBX_TCP_EXPECT_LENGTH		4
#define ZBX_TCP_EXPECT_SIZE		5

	ssize_t		nbytes;
	size_t		buf_dyn_bytes = 0, buf_stat_bytes = 0, offset = 0;
	zbx_uint64_t	expected_len = 16 * ZBX_MEBIBYTE, reserved = 0, max_len;
	unsigned char	expect = ZBX_TCP_EXPECT_HEADER;
	int		protocol_version;
#if defined(_WINDOWS)
	max_len = ZBX_MAX_RECV_DATA_SIZE;
#else
	max_len = 0 != (flags & ZBX_TCP_LARGE) ? ZBX_MAX_RECV_LARGE_DATA_SIZE : ZBX_MAX_RECV_DATA_SIZE;
#endif
	zbx_socket_free(s);

	s->buf_type = ZBX_BUF_TYPE_STAT;
	s->buffer = s->buf_stat;

	if (0 != timeout)
		zbx_socket_set_deadline(s, timeout);

	while (0 != (nbytes = zbx_tcp_read(s, s->buf_stat + buf_stat_bytes, sizeof(s->buf_stat) - buf_stat_bytes)))
	{
		if (ZBX_PROTO_ERROR == nbytes)
			goto out;

		if (ZBX_BUF_TYPE_STAT == s->buf_type)
			buf_stat_bytes += (size_t)nbytes;
		else
		{
			if (buf_dyn_bytes + (size_t)nbytes <= expected_len)
				memcpy(s->buffer + buf_dyn_bytes, s->buf_stat, (size_t)nbytes);
			buf_dyn_bytes += (size_t)nbytes;
		}

		if (buf_stat_bytes + buf_dyn_bytes >= expected_len)
			break;

		if (ZBX_TCP_EXPECT_HEADER == expect)
		{
			if (ZBX_TCP_HEADER_LEN > buf_stat_bytes)
			{
				if (0 == strncmp(s->buf_stat, ZBX_TCP_HEADER_DATA, buf_stat_bytes))
					continue;

				break;
			}
			else
			{
				if (0 != strncmp(s->buf_stat, ZBX_TCP_HEADER_DATA, ZBX_TCP_HEADER_LEN))
				{
					/* invalid header, abort receiving */
					break;
				}

				expect = ZBX_TCP_EXPECT_VERSION;
				offset += ZBX_TCP_HEADER_LEN;
			}
		}

		if (ZBX_TCP_EXPECT_VERSION == expect)
		{
			if (offset + 1 > buf_stat_bytes)
				continue;

			expect = ZBX_TCP_EXPECT_VERSION_VALIDATE;
			protocol_version = s->buf_stat[ZBX_TCP_HEADER_LEN];

			if (0 == (protocol_version & ZBX_TCP_PROTOCOL) ||
					protocol_version > (ZBX_TCP_PROTOCOL | ZBX_TCP_COMPRESS | flags))
			{
				/* invalid protocol version, abort receiving */
				break;
			}
			s->protocol = protocol_version;
			expect = ZBX_TCP_EXPECT_LENGTH;
			offset++;
		}

		if (ZBX_TCP_EXPECT_LENGTH == expect)
		{
			if (0 != (protocol_version & ZBX_TCP_LARGE))
			{
				zbx_uint64_t	len64_le;

				if (offset + 2 * sizeof(len64_le) > buf_stat_bytes)
					continue;

				memcpy(&len64_le, s->buf_stat + offset, sizeof(len64_le));
				offset += sizeof(len64_le);
				expected_len = zbx_letoh_uint64(len64_le);

				memcpy(&len64_le, s->buf_stat + offset, sizeof(len64_le));
				offset += sizeof(len64_le);
				reserved = zbx_letoh_uint64(len64_le);
			}
			else
			{
				zbx_uint32_t	len32_le;

				if (offset + 2 * sizeof(len32_le) > buf_stat_bytes)
					continue;

				memcpy(&len32_le, s->buf_stat + offset, sizeof(len32_le));
				offset += sizeof(len32_le);
				expected_len = zbx_letoh_uint32(len32_le);

				memcpy(&len32_le, s->buf_stat + offset, sizeof(len32_le));
				offset += sizeof(len32_le);
				reserved = zbx_letoh_uint32(len32_le);
			}

			if (max_len < expected_len)
			{
				zabbix_log(LOG_LEVEL_WARNING, "Message size " ZBX_FS_UI64 " from %s exceeds the "
						"maximum size " ZBX_FS_UI64 " bytes. Message ignored.", expected_len,
						s->peer, max_len);
				nbytes = ZBX_PROTO_ERROR;
				goto out;
			}

			/* compressed protocol stores uncompressed packet size in the reserved data */
			if (max_len < reserved)
			{
				zabbix_log(LOG_LEVEL_WARNING, "Uncompressed message size " ZBX_FS_UI64 " from %s"
						" exceeds the maximum size " ZBX_FS_UI64 " bytes. Message ignored.",
						reserved, s->peer, max_len);
				nbytes = ZBX_PROTO_ERROR;
				goto out;
			}

			if (sizeof(s->buf_stat) > expected_len)
			{
				buf_stat_bytes -= offset;
				memmove(s->buf_stat, s->buf_stat + offset, buf_stat_bytes);
			}
			else
			{
				s->buf_type = ZBX_BUF_TYPE_DYN;
				s->buffer = (char *)zbx_malloc(NULL, expected_len + 1);
				buf_dyn_bytes = buf_stat_bytes - offset;
				buf_stat_bytes = 0;
				memcpy(s->buffer, s->buf_stat + offset, buf_dyn_bytes);
			}

			expect = ZBX_TCP_EXPECT_SIZE;

			if (buf_stat_bytes + buf_dyn_bytes >= expected_len)
				break;
		}
	}

	if (ZBX_TCP_EXPECT_SIZE == expect)
	{
		if (buf_stat_bytes + buf_dyn_bytes == expected_len)
		{
			if (0 != (protocol_version & ZBX_TCP_COMPRESS))
			{
				char	*out;
				size_t	out_size = reserved;

				out = (char *)zbx_malloc(NULL, reserved + 1);
				if (FAIL == zbx_uncompress(s->buffer, buf_stat_bytes + buf_dyn_bytes, out, &out_size))
				{
					zbx_free(out);
					zbx_set_socket_strerror("cannot uncompress data: %s", zbx_compress_strerror());
					nbytes = ZBX_PROTO_ERROR;
					goto out;
				}

				if (out_size != reserved)
				{
					zbx_free(out);
					zbx_set_socket_strerror("size of uncompressed data is less than expected");
					nbytes = ZBX_PROTO_ERROR;
					goto out;
				}

				if (ZBX_BUF_TYPE_DYN == s->buf_type)
					zbx_free(s->buffer);

				s->buf_type = ZBX_BUF_TYPE_DYN;
				s->buffer = out;
				s->read_bytes = reserved;

				zabbix_log(LOG_LEVEL_TRACE, "%s(): received " ZBX_FS_SIZE_T " bytes with"
						" compression ratio %.1f", __func__,
						(zbx_fs_size_t)(buf_stat_bytes + buf_dyn_bytes),
						(double)reserved / (double)(buf_stat_bytes + buf_dyn_bytes));
			}
			else
				s->read_bytes = buf_stat_bytes + buf_dyn_bytes;

			s->buffer[s->read_bytes] = '\0';
		}
		else
		{
			if (buf_stat_bytes + buf_dyn_bytes < expected_len)
			{
				zabbix_log(LOG_LEVEL_WARNING, "Message from %s is shorter than expected " ZBX_FS_UI64
						" bytes. Message ignored.", s->peer, (zbx_uint64_t)expected_len);
			}
			else
			{
				zabbix_log(LOG_LEVEL_WARNING, "Message from %s is longer than expected " ZBX_FS_UI64
						" bytes. Message ignored.", s->peer, (zbx_uint64_t)expected_len);
			}

			nbytes = ZBX_PROTO_ERROR;
		}
	}
	else if (ZBX_TCP_EXPECT_LENGTH == expect)
	{
		zabbix_log(LOG_LEVEL_WARNING, "Message from %s is missing data length. Message ignored.", s->peer);
		nbytes = ZBX_PROTO_ERROR;
	}
	else if (ZBX_TCP_EXPECT_VERSION == expect)
	{
		zabbix_log(LOG_LEVEL_WARNING, "Message from %s is missing protocol version. Message ignored.",
				s->peer);
		nbytes = ZBX_PROTO_ERROR;
	}
	else if (ZBX_TCP_EXPECT_VERSION_VALIDATE == expect)
	{
		zabbix_log(LOG_LEVEL_WARNING, "Message from %s is using unsupported protocol version \"%d\"."
				" Message ignored.", s->peer, protocol_version);
		nbytes = ZBX_PROTO_ERROR;
	}
	else if (0 != buf_stat_bytes)
	{
		zabbix_log(LOG_LEVEL_WARNING, "Message from %s is missing header. Message ignored.", s->peer);
		nbytes = ZBX_PROTO_ERROR;
	}
	else
	{
		s->read_bytes = 0;
		s->buffer[s->read_bytes] = '\0';
	}
out:
	if (0 != timeout)
		zbx_socket_set_deadline(s, 0);

	return (ZBX_PROTO_ERROR == nbytes ? FAIL : (ssize_t)(s->read_bytes + offset));

#undef ZBX_TCP_EXPECT_HEADER
#undef ZBX_TCP_EXPECT_LENGTH
#undef ZBX_TCP_EXPECT_SIZE
}

/******************************************************************************
 *                                                                            *
 * Purpose: receive data till connection is closed                            *
 *                                                                            *
 * Return value: number of bytes received - success,                          *
 *               FAIL - an error occurred                                     *
 *                                                                            *
 ******************************************************************************/
ssize_t	zbx_tcp_recv_raw_ext(zbx_socket_t *s, int timeout)
{
	ssize_t		nbytes;
	size_t		allocated = 8 * ZBX_STAT_BUF_LEN, buf_dyn_bytes = 0, buf_stat_bytes = 0;
	zbx_uint64_t	expected_len = 16 * ZBX_MEBIBYTE;

	zbx_socket_free(s);

	s->buf_type = ZBX_BUF_TYPE_STAT;
	s->buffer = s->buf_stat;

	if (0 != timeout)
		zbx_socket_set_deadline(s, timeout);

	while (0 != (nbytes = zbx_tcp_read(s, s->buf_stat + buf_stat_bytes, sizeof(s->buf_stat) - buf_stat_bytes)))
	{
		if (ZBX_PROTO_ERROR == nbytes)
			goto out;

		if (ZBX_BUF_TYPE_STAT == s->buf_type)
			buf_stat_bytes += (size_t)nbytes;
		else
		{
			if (buf_dyn_bytes + (size_t)nbytes >= allocated)
			{
				while (buf_dyn_bytes + (size_t)nbytes >= allocated)
					allocated *= 2;
				s->buffer = (char *)zbx_realloc(s->buffer, allocated);
			}

			memcpy(s->buffer + buf_dyn_bytes, s->buf_stat, (size_t)nbytes);
			buf_dyn_bytes += (size_t)	nbytes;
		}

		if (buf_stat_bytes + buf_dyn_bytes >= expected_len)
			break;

		if (sizeof(s->buf_stat) == buf_stat_bytes)
		{
			s->buf_type = ZBX_BUF_TYPE_DYN;
			s->buffer = (char *)zbx_malloc(NULL, allocated);
			buf_dyn_bytes = sizeof(s->buf_stat);
			buf_stat_bytes = 0;
			memcpy(s->buffer, s->buf_stat, sizeof(s->buf_stat));
		}
	}

	if (buf_stat_bytes + buf_dyn_bytes >= expected_len)
	{
		zabbix_log(LOG_LEVEL_WARNING, "Message from %s is longer than " ZBX_FS_UI64 " bytes allowed for"
				" plain text. Message ignored.", s->peer, expected_len);
		nbytes = ZBX_PROTO_ERROR;
		goto out;
	}

	s->read_bytes = buf_stat_bytes + buf_dyn_bytes;
	s->buffer[s->read_bytes] = '\0';
out:

	if (0 != timeout)
		zbx_socket_set_deadline(s, 0);

	return (ZBX_PROTO_ERROR == nbytes ? FAIL : (ssize_t)(s->read_bytes));
}

static int	subnet_match(int af, unsigned int prefix_size, const void *address1, const void *address2)
{
	unsigned char	netmask[16] = {0};
	int		i, j, bytes;

	if (af == AF_INET)
	{
		if (prefix_size > ZBX_IPV4_MAX_CIDR_PREFIX)
			return FAIL;
		bytes = 4;
	}
	else
	{
		if (prefix_size > ZBX_IPV6_MAX_CIDR_PREFIX)
			return FAIL;
		bytes = 16;
	}

	/* CIDR notation to subnet mask */
	for (i = (int)prefix_size, j = 0; i > 0 && j < bytes; i -= 8, j++)
		netmask[j] = (unsigned char)(i >= 8 ? 0xFF : ~((1 << (8 - i)) - 1));

	/* The result of the bitwise AND operation of IP address and the subnet mask is the network prefix. */
	/* All hosts on a subnetwork have the same network prefix. */
	for (i = 0; i < bytes; i++)
	{
		if ((((const unsigned char *)address1)[i] & netmask[i]) !=
				(((const unsigned char *)address2)[i] & netmask[i]))
		{
			return FAIL;
		}
	}

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Purpose: check if the address belongs to the given subnet                  *
 *                                                                            *
 * Parameters: prefix_size - [IN] subnet prefix size                          *
 *             current_ai  - [IN] subnet                                      *
 *             name        - [IN] address                                     *
 *             ipv6v4_mode - [IN] compare IPv6 IPv4-mapped address with       *
 *                                IPv4 addresses only                         *
 *                                                                            *
 * Return value: SUCCEED - address belongs to the subnet                      *
 *               FAIL - otherwise                                             *
 *                                                                            *
 ******************************************************************************/
#ifndef HAVE_IPV6
int	zbx_ip_cmp(unsigned int prefix_size, const struct addrinfo *current_ai, ZBX_SOCKADDR name, int ipv6v4_mode)
{
	struct sockaddr_in	*name4 = (struct sockaddr_in *)&name,
				*ai_addr4 = (struct sockaddr_in *)current_ai->ai_addr;

	ZBX_UNUSED(ipv6v4_mode);

	return subnet_match(current_ai->ai_family, prefix_size, &name4->sin_addr.s_addr, &ai_addr4->sin_addr.s_addr);
}
#else
int	zbx_ip_cmp(unsigned int prefix_size, const struct addrinfo *current_ai, ZBX_SOCKADDR name, int ipv6v4_mode)
{
	/* Network Byte Order is ensured */
	/* IPv4-compatible, the first 96 bits are zeros */
	const unsigned char	ipv4_compat_mask[12] = {0};
	/* IPv4-mapped, the first 80 bits are zeros, 16 next - ones */
	const unsigned char	ipv4_mapped_mask[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 255, 255};

	struct sockaddr_in	*name4 = (struct sockaddr_in *)&name,
				*ai_addr4 = (struct sockaddr_in *)current_ai->ai_addr;
	struct sockaddr_in6	*name6 = (struct sockaddr_in6 *)&name,
				*ai_addr6 = (struct sockaddr_in6 *)current_ai->ai_addr;

#ifdef HAVE_SOCKADDR_STORAGE_SS_FAMILY
	if (current_ai->ai_family == name.ss_family)
#else
	if (current_ai->ai_family == name.__ss_family)
#endif
	{
		switch (current_ai->ai_family)
		{
			case AF_INET:
				if (SUCCEED == subnet_match(current_ai->ai_family, prefix_size, &name4->sin_addr.s_addr,
						&ai_addr4->sin_addr.s_addr))
				{
					return SUCCEED;
				}
				break;
			case AF_INET6:
				if ((0 == ipv6v4_mode || 0 != memcmp(name6->sin6_addr.s6_addr, ipv4_mapped_mask, 12)) &&
						SUCCEED == subnet_match(current_ai->ai_family, prefix_size,
						name6->sin6_addr.s6_addr,
						ai_addr6->sin6_addr.s6_addr))
				{
					return SUCCEED;
				}
				break;
		}
	}
	else
	{
		unsigned char	ipv6_compat_address[16], ipv6_mapped_address[16];

		if (AF_INET == current_ai->ai_family)
		{
			/* incoming AF_INET6, must see whether it is compatible or mapped */

			if (((0 == memcmp(name6->sin6_addr.s6_addr, ipv4_mapped_mask, 12)) ||
					(0 == ipv6v4_mode && 0 == memcmp(name6->sin6_addr.s6_addr,
					ipv4_compat_mask, 12))) && SUCCEED == subnet_match(AF_INET, prefix_size,
					&name6->sin6_addr.s6_addr[12], &ai_addr4->sin_addr.s_addr))
			{
				return SUCCEED;
			}
		}
		else if (AF_INET6 == current_ai->ai_family && 0 == ipv6v4_mode)
		{
			/* incoming AF_INET, must see whether the given is compatible or mapped */

			memcpy(ipv6_compat_address, ipv4_compat_mask, sizeof(ipv4_compat_mask));
			memcpy(&ipv6_compat_address[sizeof(ipv4_compat_mask)], &name4->sin_addr.s_addr, 4);

			memcpy(ipv6_mapped_address, ipv4_mapped_mask, sizeof(ipv4_mapped_mask));
			memcpy(&ipv6_mapped_address[sizeof(ipv4_mapped_mask)], &name4->sin_addr.s_addr, 4);

			if (SUCCEED == subnet_match(AF_INET6, prefix_size,
					&ai_addr6->sin6_addr.s6_addr, ipv6_compat_address) ||
					SUCCEED == subnet_match(AF_INET6, prefix_size,
					&ai_addr6->sin6_addr.s6_addr, ipv6_mapped_address))
			{
				return SUCCEED;
			}
		}
	}
	return FAIL;
}
#endif

int	validate_cidr(const char *ip, const char *cidr, void *value)
{
	if (SUCCEED == zbx_is_ip4(ip))
		return zbx_is_uint_range(cidr, value, 0, ZBX_IPV4_MAX_CIDR_PREFIX);
#ifdef HAVE_IPV6
	if (SUCCEED == zbx_is_ip6(ip))
		return zbx_is_uint_range(cidr, value, 0, ZBX_IPV6_MAX_CIDR_PREFIX);
#endif
	return FAIL;
}

int	zbx_validate_peer_list(const char *peer_list, char **error)
{
	char	*start, *end, *cidr_sep;
	char	tmp[MAX_STRING_LEN];

	zbx_strscpy(tmp, peer_list);

	for (start = tmp; '\0' != *start;)
	{
		if (NULL != (end = strchr(start, ',')))
			*end = '\0';

		if (NULL != (cidr_sep = strchr(start, '/')))
		{
			*cidr_sep = '\0';

			if (FAIL == validate_cidr(start, cidr_sep + 1, NULL))
			{
				*cidr_sep = '/';
				*error = zbx_dsprintf(NULL, "\"%s\"", start);
				return FAIL;
			}
		}
		else if (FAIL == zbx_is_supported_ip(start) && FAIL == zbx_validate_hostname(start))
		{
			*error = zbx_dsprintf(NULL, "\"%s\"", start);
			return FAIL;
		}

		if (NULL != end)
			start = end + 1;
		else
			break;
	}

	return SUCCEED;
}

/******************************************************************************
 *                                                                            *
 * Purpose: check if connection initiator is in list of peers                 *
 *                                                                            *
 * Parameters: s         - [IN] socket descriptor                             *
 *             peer_list - [IN] comma-delimited list of allowed peers.        *
 *                              NULL not allowed. Empty string results in     *
 *                              return value FAIL.                            *
 *                                                                            *
 * Return value: SUCCEED - connection allowed                                 *
 *               FAIL - connection is not allowed                             *
 *                                                                            *
 * Comments: standard, compatible and IPv4-mapped addresses are treated       *
 *           the same: 127.0.0.1 == ::127.0.0.1 == ::ffff:127.0.0.1           *
 *                                                                            *
 ******************************************************************************/
int	zbx_tcp_check_allowed_peers(const zbx_socket_t *s, const char *peer_list)
{
	char	*start = NULL, *end = NULL, *cidr_sep, tmp[MAX_STRING_LEN];
	int	prefix_size;

	/* examine list of allowed peers which may include DNS names, IPv4/6 addresses and addresses in CIDR notation */

	zbx_strscpy(tmp, peer_list);

	for (start = tmp; '\0' != *start;)
	{
		struct addrinfo	hints, *ai = NULL, *current_ai;

		prefix_size = -1;

		if (NULL != (end = strchr(start, ',')))
			*end = '\0';

		if (NULL != (cidr_sep = strchr(start, '/')))
		{
			*cidr_sep = '\0';

			/* validate_cidr() may overwrite 'prefix_size' */
			if (SUCCEED != validate_cidr(start, cidr_sep + 1, &prefix_size))
				*cidr_sep = '/';	/* CIDR is only supported for IP */
		}

		memset(&hints, 0, sizeof(hints));
		hints.ai_family = AF_UNSPEC;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;

		if (0 == getaddrinfo(start, NULL, &hints, &ai))
		{
			for (current_ai = ai; NULL != current_ai; current_ai = current_ai->ai_next)
			{
				int	prefix_size_current = prefix_size;

				if (-1 == prefix_size_current)
				{
					prefix_size_current = (current_ai->ai_family == AF_INET ?
							ZBX_IPV4_MAX_CIDR_PREFIX : ZBX_IPV6_MAX_CIDR_PREFIX);
				}

				if (SUCCEED == zbx_ip_cmp((unsigned int)prefix_size_current, current_ai, s->peer_info,
						0))
				{
					freeaddrinfo(ai);
					return SUCCEED;
				}
			}
			freeaddrinfo(ai);
		}

		if (NULL != end)
			start = end + 1;
		else
			break;
	}

	zbx_set_socket_strerror("connection from \"%s\" rejected, allowed hosts: \"%s\"", s->peer, peer_list);

	return FAIL;
}

/******************************************************************************
 *                                                                            *
 * Purpose: translate connection type code to name                            *
 *                                                                            *
 ******************************************************************************/
const char	*zbx_tcp_connection_type_name(unsigned int type)
{
	switch (type)
	{
		case ZBX_TCP_SEC_UNENCRYPTED:
			return "unencrypted";
		case ZBX_TCP_SEC_TLS_CERT:
			return "TLS with certificate";
		case ZBX_TCP_SEC_TLS_PSK:
			return "TLS with PSK";
		default:
			return "unknown";
	}
}

int	zbx_udp_connect(zbx_socket_t *s, const char *source_ip, const char *ip, unsigned short port, int timeout)
{
	return zbx_socket_create(s, SOCK_DGRAM, source_ip, ip, port, timeout, ZBX_TCP_SEC_UNENCRYPTED, NULL, NULL);
}

int	zbx_udp_send(zbx_socket_t *s, const char *data, size_t data_len, int timeout)
{
	ssize_t		offset = 0, n;
	zbx_pollfd_t	pd;

	zbx_socket_set_deadline(s, timeout);

	pd.fd = s->socket;
	pd.events = POLLOUT;

	while (offset < (ssize_t)data_len)
	{
		if (ZBX_PROTO_ERROR == (n = zbx_sendto(s->socket, data + offset, data_len - (size_t)offset, 0, NULL,
				0)))
		{
			int	rc;

			if (SUCCEED != zbx_socket_had_nonblocking_error())
			{
				zbx_set_socket_strerror("sendto() failed: %s",
						strerror_from_system(zbx_socket_last_error()));
				return FAIL;
			}

			if (-1 == (rc = zbx_socket_poll(&pd, 1, ZBX_SOCKET_POLL_TIMEOUT)))
			{
				if (SUCCEED == zbx_socket_had_nonblocking_error())
					continue;

				zbx_set_socket_strerror("cannot wait for socket: %s",
						strerror_from_system(zbx_socket_last_error()));
				return FAIL;
			}

			if (0 != rc && 0 == (pd.revents & POLLOUT))
			{
				char	*errmsg;

				errmsg = socket_poll_error(pd.revents);
				zbx_set_socket_strerror("%s", errmsg);
				zbx_free(errmsg);

				zabbix_log(LOG_LEVEL_DEBUG, "poll(POLLOUT) failed with revents 0x%x",
						(unsigned)pd.revents);

				return FAIL;
			}
		}
		else
			offset += n;

		if (SUCCEED != zbx_socket_check_deadline(s))
		{
			zbx_set_socket_strerror("send timeout");
			return FAIL;
		}
	}

	return SUCCEED;
}

int	zbx_udp_recv(zbx_socket_t *s, int timeout)
{
	char	buffer[65508];	/* maximum payload for UDP over IPv4 is 65507 bytes */

	ssize_t		n;
	zbx_pollfd_t	pd;

	zbx_socket_set_deadline(s, timeout);

	pd.fd = s->socket;
	pd.events = POLLIN;

	zbx_socket_free(s);

	while (0 >= (n = recvfrom(s->socket, buffer, sizeof(buffer) - 1, 0, NULL, NULL)))
	{
		int	rc;

		if (0 == n)
		{
			zbx_set_socket_strerror("connection shutdown");
			return FAIL;
		}

		if (SUCCEED != zbx_socket_had_nonblocking_error())
		{
			zbx_set_socket_strerror("recvfrom() failed: %s",
					strerror_from_system(zbx_socket_last_error()));
			return FAIL;
		}

		if (-1 == (rc = zbx_socket_poll(&pd, 1, ZBX_SOCKET_POLL_TIMEOUT)))
		{
			if (SUCCEED != zbx_socket_had_nonblocking_error())
			{
				zbx_set_socket_strerror("cannot wait for socket: %s",
						strerror_from_system(zbx_socket_last_error()));
				return FAIL;
			}
		}

		if (SUCCEED != zbx_socket_check_deadline(s))
		{
			zbx_set_socket_strerror("recv timeout");
			return FAIL;
		}

		if (0 >= rc)
			continue;

		if (0 == (pd.revents & POLLIN))
		{
			char	*errmsg;

			errmsg = socket_poll_error(pd.revents);
			zbx_set_socket_strerror("%s", errmsg);
			zbx_free(errmsg);

			zabbix_log(LOG_LEVEL_DEBUG, "poll(POLLIN) failed with revents 0x%x",
					(unsigned)pd.revents);

			return FAIL;
		}
	}

	if (sizeof(s->buf_stat) > (size_t)n)
	{
		s->buf_type = ZBX_BUF_TYPE_STAT;
		s->buffer = s->buf_stat;
	}
	else
	{
		s->buf_type = ZBX_BUF_TYPE_DYN;
		s->buffer = (char *)zbx_malloc(s->buffer, (size_t)n + 1);
	}

	memcpy(s->buffer, buffer, (size_t)n);
	s->buffer[n] = '\0';
	s->read_bytes = (size_t)n;

	return SUCCEED;
}

void	zbx_udp_close(zbx_socket_t *s)
{
	zbx_socket_free(s);
	zbx_socket_close(s->socket);
}
