#ifndef _SNMPAAL5PVCDOMAIN_H
#define _SNMPAAL5PVCDOMAIN_H

#ifdef SNMP_TRANSPORT_AAL5PVC_DOMAIN

#if HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#include <atm.h>

#include "snmp_transport.h"
#include "asn1.h"

const oid ucdSnmpAal5PvcDomain[9];  /* = { UCDAVIS_MIB, 251, 3 }; */

snmp_transport		*snmp_aal5pvc_transport	(struct sockaddr_atmpvc *addr,
						 int local);

/*  "Constructor" for transport domain object.  */

void			 snmp_aal5pvc_ctor	(void);

#endif/*SNMP_TRANSPORT_AAL5PVC_DOMAIN*/

#endif/*_SNMPAAL5PVCDOMAIN_H*/
