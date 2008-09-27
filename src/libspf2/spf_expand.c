/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of either:
 *
 *   a) The GNU Lesser General Public License as published by the Free
 *	  Software Foundation; either version 2.1, or (at your option) any
 *	  later version,
 *
 *   OR
 *
 *   b) The two-clause BSD license.
 *
 * These licenses can be found with the distribution in the file LICENSES
 */

#include "spf_sys_config.h"


#ifdef STDC_HEADERS
# include <stdio.h>		/* stdin / stdout */
# include <stdlib.h>	   /* malloc / free */
# include <ctype.h>		/* isupper / tolower */
#endif

#ifdef HAVE_STRING_H
# include <string.h>	   /* strstr / strdup */
#else
# ifdef HAVE_STRINGS_H
#  include <strings.h>	   /* strstr / strdup */
# endif
#endif

#if TIME_WITH_SYS_TIME
# include <sys/time.h>
# include <time.h>
#else
# if HAVE_SYS_TIME_H
#  include <sys/time.h>
# else
#  include <time.h>
# endif
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#endif


#include "spf.h"
#include "spf_internal.h"
#include "spf_record.h"


static const char		client_ver_ipv4[] = "in-addr";
static const char		client_ver_ipv6[] = "ip6";


static inline int
SPF_delim_valid(SPF_data_t *d, char c)
{
	return (   ( d->dv.delim_dot   && c == '.' )
			|| ( d->dv.delim_dash  && c == '-' )
			|| ( d->dv.delim_plus  && c == '+' )
			|| ( d->dv.delim_equal && c == '=' )
			|| ( d->dv.delim_bar   && c == '|' )
			|| ( d->dv.delim_under && c == '_' ) );
}

/* This could better collect errors, like the compiler does. */
/* This requires that *bufp be either malloced to *buflenp, or NULL
 */
SPF_errcode_t
SPF_record_expand_data(SPF_server_t *spf_server,
				SPF_request_t *spf_request,
				SPF_response_t *spf_response,
				SPF_data_t *data, size_t data_len,
				char **bufp, size_t *buflenp)
{
	SPF_data_t		*d, *data_end;

	size_t		len;
	const char		*p_err;
	char		*p, *p_end;
	const char		*p_read;
	const char		*p_read_end;
	char		*p_write;
	char		*p2, *p2_end;


	const char		*var;
	char		*munged_var = NULL;
	char		*url_var = NULL;

			/* Pretty-printing buffers. */
	char		ip4_buf[ INET_ADDRSTRLEN ];
	char		ip6_buf[ INET6_ADDRSTRLEN ];
			/* Hex buffer for ipv6 (size in nibbles) */
	char		ip6_rbuf[ sizeof( struct in6_addr ) * 4 + 1 ];

	char		time_buf[ sizeof( "4294967296" ) ]; /* 2^32 seconds max		*/

	int				num_found;
	int				i;


	/*
	 * make sure we were passed valid data to work with
	 */
	SPF_ASSERT_NOTNULL(spf_server);
	SPF_ASSERT_NOTNULL(data);
	SPF_ASSERT_NOTNULL(bufp);
	SPF_ASSERT_NOTNULL(buflenp);

	/* data_end = SPF_mech_end_data( mech ); */ /* doesn't work for mods */
	data_end = (SPF_data_t *)((char *)data + data_len);

	/*
	 * make sure the return buffer is big enough
	 *
	 * find max length of all variables
	 */

	len = 0;
	for( d = data; d < data_end; d = SPF_data_next( d ) )
	{
		switch( d->ds.parm_type )
		{
		case PARM_CIDR:
			break;

		case PARM_STRING:
			len += d->ds.len;
			break;

		case PARM_CLIENT_IP:
			len += sizeof( ip6_rbuf );
			break;

		case PARM_CLIENT_IP_P:
			len += sizeof( ip6_buf );
			break;

		default:
			/* An interpolated variable. Might be any field from
			 * the SPF_request_t */
			if ( spf_request->max_var_len > 8 )
				len += spf_request->max_var_len * 3; /* url encoding				*/
			else
				len += 8 * 3;
			break;
		}
	}
	len += sizeof( '\0' );		/* Strictly, ANSI says this is 1. */

	if ( *buflenp < len )
	{
		char		*new_rec;
		size_t		new_len;

		/* allocate lots so we don't have to remalloc often */
		new_len = len + 64;

		new_rec = realloc( *bufp, new_len );
		if ( new_rec == NULL )
			return SPF_E_NO_MEMORY;

		*bufp = new_rec;
		*buflenp = new_len;
	}
	memset( *bufp, '\0', *buflenp );		/* cheaper than NUL at each step */
	p = *bufp;
	p_end = *bufp + *buflenp;



	/*
	 * expand the data
	 */

	for (d = data; d < data_end; d = SPF_data_next(d)) {
		if (d->dc.parm_type == PARM_CIDR)
			continue;

		if ( d->ds.parm_type == PARM_STRING ) {
			if ( p_end - (p + d->ds.len) <= 0 )
					SPF_error("Failed to allocate enough memory "
								"to expand string.");

			memcpy( p, SPF_data_str( d ), d->ds.len );
			p += d->ds.len;
			continue;
		}

		/* Otherwise, it's a variable. */

		var = NULL;
		switch( d->dv.parm_type )
		{
		case PARM_LP_FROM:		/* local-part of envelope-sender */
			var = spf_request->env_from_lp;
			break;

		case PARM_ENV_FROM:		/* envelope-sender				*/
			var = spf_request->env_from;
			break;

		case PARM_DP_FROM:		/* envelope-domain				*/
			var = spf_request->env_from_dp;
			break;

		case PARM_CUR_DOM:		/* current-domain				*/
			var = spf_request->cur_dom;
			break;

		case PARM_CLIENT_IP:		/* SMTP client IP				*/
			if ( spf_request->client_ver == AF_INET ) {
				p_err = inet_ntop( AF_INET, &spf_request->ipv4,
								   ip4_buf, sizeof( ip4_buf ) );
				var = ip4_buf;
			}
			else if ( spf_request->client_ver == AF_INET6 ) {
				p2 = ip6_rbuf;
				p2_end = p2 + sizeof( ip6_rbuf );

				for( i = 0; i < array_elem( spf_request->ipv6.s6_addr ); i++ )
				{
					p2 += snprintf( p2, p2_end - p2, "%.1x.%.1x.",
									spf_request->ipv6.s6_addr[i] >> 4,
									spf_request->ipv6.s6_addr[i] & 0xf );
				}

				/* squash the final '.' */
				ip6_rbuf[ sizeof( struct in6_addr ) * 4 - 1] = '\0';

				var = ip6_rbuf;
			}
			break;

		case PARM_CLIENT_IP_P:		/* SMTP client IP (pretty)		*/
			if ( spf_request->client_ver == AF_INET )
			{
				p_err = inet_ntop( AF_INET, &spf_request->ipv4,
								   ip4_buf, sizeof( ip4_buf ) );
				var = ip4_buf;
			}
			else if ( spf_request->client_ver == AF_INET6 )
			{
				p_err = inet_ntop( AF_INET6, &spf_request->ipv6,
								   ip6_buf, sizeof( ip6_buf ) );
				var = ip6_buf;
			}
			break;

		case PARM_TIME:				/* time in UTC epoch secs		*/
			snprintf( time_buf, sizeof( time_buf ), "%ld",
					  (long int) time( NULL ) );
			var = time_buf;
			break;

		case PARM_CLIENT_DOM:		/* SMTP client domain name		*/
			var = SPF_request_get_client_dom(spf_request);
			break;

		case PARM_CLIENT_VER:		/* IP ver str - in-addr/ip6		*/
			if ( spf_request->client_ver == AF_INET )
				var = client_ver_ipv4;
			else if ( spf_request->client_ver == AF_INET6 )
				var = client_ver_ipv6;
			break;

		case PARM_HELO_DOM:		/* HELO/EHLO domain				*/
			var = spf_request->helo_dom;
			break;

		case PARM_REC_DOM:		/* receiving domain				*/
			var = SPF_request_get_rec_dom(spf_request);
			break;

		default:
			return SPF_E_INVALID_VAR;
			break;
		}

		if ( var == NULL )
			return SPF_E_UNINIT_VAR;


		/* Now we put 'var' through the munging procedure. */

		len = strlen(var);
		munged_var = (char *)malloc(len + 1);
		if (munged_var == NULL)
			return SPF_E_NO_MEMORY;
		memset(munged_var, 0, len + 1);

		p_read_end = var + len;
		p_write = munged_var;

		/* reverse */

		if ( d->dv.rev ) {
			p_read = p_read_end - 1;

			while ( p_read >= var ) {
				if ( SPF_delim_valid(d, *p_read) ) {
					/* Subtract 1 because p_read points to delim, and
					 * p_read_end points to the following delim. */
					len = p_read_end - p_read - 1;
					memcpy( p_write, p_read + 1, len );
					p_write += len;
					*p_write++ = '.';

					p_read_end = p_read;
				}
				p_read--;
			}

			/* Now p_read_end should point one before the start of the
			 * string. p_read_end might also point there if the string
			 * starts with a delimiter. */
			if (p_read_end >= p_read) {
				len = p_read_end - p_read - 1;
				memcpy( p_write, p_read + 1, len );
				p_write += len;
				*p_write++ = '.';
			}

			/* p_write always points to the 'next' character. */
			p_write--;
			*p_write = '\0';
		}
		else {
			p_read = var;

			while (p_read < p_read_end) {
					if (SPF_delim_valid(d, *p_read))
					*p_write++ = '.';
				else
					*p_write++ = *p_read;
				p_read++;
			}

			*p_write = '\0';
		}

		/* Now munged_var is a copy of var, possibly reversed, and
		 * thus len == strlen(munged_var). However, we continue to
		 * manipulate the underlying munged_var since var is const. */

		/* truncate, from the right hand side. */
		if (d->dv.num_rhs > 0) {
			p_read_end = munged_var + len;		/* const, at '\0' */
			p_write = munged_var + len - 1;
			num_found = 0;
			while (p_write > munged_var) {
				if (*p_write == '.')
					num_found++;
				if (num_found == d->dv.num_rhs)
					break;
					p_write--;
			}
			p_write++;		/* Move to just after the '.' */
			/* This moves the '\0' as well. */
			len = p_read_end - p_write;
			memmove(munged_var, p_write, len + 1);
		}

		var = munged_var;
		/* Now, we have 'var', of length 'len' */

		/* URL encode */

		if (d->dv.url_encode) {
			url_var = malloc(len * 3 + 1);
			if (url_var == NULL) {
				if (munged_var)
					free(munged_var);
				return SPF_E_NO_MEMORY;
			}

			p_read = var;
			p_write = url_var;

			/* escape non-uric characters (rfc2396) */
			while ( *p_read != '\0' )
			{
				if ( isalnum( (unsigned char)( *p_read  ) ) )
					*p_write++ = *p_read++;
				else
				{
					switch( *p_read )
					{
					case '-':
					case '_':
					case '.':
					case '!':
					case '~':
					case '*':
					case '\'':
					case '(':
					case ')':
						*p_write++ = *p_read++;
						break;

					default:
						/* No point doing snprintf with a const '4'
						 * because we know we're going to get 4
						 * characters anyway. */
						sprintf( p_write, "%%%02x", *p_read );
						p_write += 3;
						p_read++;
						break;
					}
				}
			}
			*p_write = '\0';

			var = url_var;
			len = p_write - url_var;		/* Not actually used. */
		}


		/* finish up */
		len = snprintf( p, p_end - p, "%s", var );
		p += len;
		if ( p_end - p <= 0 ) return SPF_E_INTERNAL_ERROR;

		if (munged_var)
			free(munged_var);
		munged_var = NULL;
		if (url_var)
			free(url_var);
		url_var = NULL;
	}

	*p++ = '\0';

	return SPF_E_SUCCESS;
}
