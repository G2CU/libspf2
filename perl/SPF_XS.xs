#include <EXTERN.h>
#include <perl.h>
#include <XSUB.h>

#include "../src/include/spf_server.h"
#include "../src/include/spf_request.h"
#include "../src/include/spf_response.h"

typedef SPF_server_t	*Mail__SPF_XS__Server;
typedef SPF_request_t	*Mail__SPF_XS__Request;
typedef SPF_response_t	*Mail__SPF_XS__Response;

#define EXPORT_INTEGER(x) do { \
								newCONSTSUB(stash, #x, newSViv(x)); \
								av_push(export, newSVpv(#x, strlen(#x))); \
						} while(0)

MODULE = Mail::SPF_XS	PACKAGE = Mail::SPF_XS

PROTOTYPES: ENABLE

BOOT:
{
	HV      *stash;
	AV      *export;

	stash = gv_stashpv("Mail::SPF_XS", TRUE);
	export = get_av("Mail::SPF_XS::EXPORT_OK", TRUE);

	EXPORT_INTEGER(SPF_DNS_RESOLV);
	EXPORT_INTEGER(SPF_DNS_CACHE);
}

MODULE = Mail::SPF_XS	PACKAGE = Mail::SPF_XS::Server

Mail::SPF_XS::Server
new(class, args)
	SV	*class
	HV	*args
	PREINIT:
		SPF_server_t	*spf_server;
	CODE:
		(void)class;
		spf_server = SPF_server_new(SPF_DNS_RESOLV, 0);
		RETVAL = spf_server;
	OUTPUT:
		RETVAL

void
DESTROY(server)
	Mail::SPF_XS::Server	server
	CODE:
		SPF_server_free(server);

Mail::SPF_XS::Response
process(server, request)
	Mail::SPF_XS::Server	server
	Mail::SPF_XS::Request	request
	PREINIT:
		SPF_response_t	*response = NULL;
	CODE:
		request->spf_server = server;
		SPF_request_query_mailfrom(request, &response);
		RETVAL = response;
	OUTPUT:
		RETVAL

MODULE = Mail::SPF_XS	PACKAGE = Mail::SPF_XS::Request

Mail::SPF_XS::Request
new(server, args)
	Mail::SPF_XS::Server	 server
	HV						*args
	PREINIT:
		SV				**svp;
		SPF_request_t	*spf_request;
	CODE:
		spf_request = SPF_request_new(server);
		svp = hv_fetch(args, "ip_address", 10, FALSE);
		if (svp && SvPOK(*svp))
			if (SPF_request_set_ipv4_str(spf_request, SvPV_nolen(*svp)) != SPF_E_SUCCESS)
				croak("Failed to set ipv4 client address");
		// ...
		RETVAL = spf_request;
	OUTPUT:
		RETVAL

void
DESTROY(request)
	Mail::SPF_XS::Request	request
	CODE:
		SPF_request_free(request);

MODULE = Mail::SPF_XS	PACKAGE = Mail::SPF_XS::Response

void
DESTROY(response)
	Mail::SPF_XS::Response	response
	CODE:
		SPF_response_free(response);

