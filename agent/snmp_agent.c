/*
 * snmp_agent.c
 *
 * Simple Network Management Protocol (RFC 1067).
 */
/***********************************************************
	Copyright 1988, 1989 by Carnegie Mellon University

                      All Rights Reserved

Permission to use, copy, modify, and distribute this software and its 
documentation for any purpose and without fee is hereby granted, 
provided that the above copyright notice appear in all copies and that
both that copyright notice and this permission notice appear in 
supporting documentation, and that the name of CMU not be
used in advertising or publicity pertaining to distribution of the
software without specific, written prior permission.  

CMU DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE, INCLUDING
ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO EVENT SHALL
CMU BE LIABLE FOR ANY SPECIAL, INDIRECT OR CONSEQUENTIAL DAMAGES OR
ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS,
WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION,
ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS
SOFTWARE.
******************************************************************/

#include <config.h>

#include <sys/types.h>
#ifdef HAVE_LIMITS_H
#include <limits.h>
#endif
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#if HAVE_STRING_H
#include <string.h>
#endif
#if TIME_WITH_SYS_TIME
# ifdef WIN32
#  include <sys/timeb.h>
# else
#  include <sys/time.h>
# endif
# include <time.h>
#else
# if HAVE_SYS_TIME_H
#  include <sys/time.h>
# else
#  include <time.h>
# endif
#endif
#if HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#if HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#include <errno.h>
#if HAVE_WINSOCK_H
#include <winsock.h>
#endif

#if HAVE_DMALLOC_H
#include <dmalloc.h>
#endif

#define SNMP_NEED_REQUEST_LIST
#include "mibincl.h"
#include "snmp_client.h"
#include "snmp_alarm.h"

#include "snmpd.h"
#include "mibgroup/struct.h"
#include "mibgroup/util_funcs.h"
#include "mib_module_config.h"

#include "default_store.h"
#include "system.h"
#include "ds_agent.h"
#include "snmp_agent.h"
#include "snmp_alarm.h"
#include "vacm.h"

#include "snmp_transport.h"
#include "snmpUDPDomain.h"
#ifdef SNMP_TRANSPORT_UNIX_DOMAIN
#include "snmpUnixDomain.h"
#endif
#ifdef SNMP_TRANSPORT_TCP_DOMAIN
#include "snmpTCPDomain.h"
#endif
#ifdef SNMP_TRANSPORT_AAL5PVC_DOMAIN
#include "snmpAAL5PVCDomain.h"
#endif
#ifdef SNMP_TRANSPORT_IPX_DOMAIN
#include "snmpIPXDomain.h"
#endif
#ifdef USING_AGENTX_PROTOCOL_MODULE
#include "agentx/protocol.h"
#endif

#ifdef USING_AGENTX_MASTER_MODULE
#include "agentx/master.h"
#endif

#define SNMP_ADDRCACHE_SIZE 10

struct addrCache {
  char *addr;
  enum { SNMP_ADDRCACHE_UNUSED = 0,
	 SNMP_ADDRCACHE_USED   = 1,
	 SNMP_ADDRCACHE_OLD    = 2 } status;
};

static struct addrCache	addrCache[SNMP_ADDRCACHE_SIZE];
int lastAddrAge = 0;
int log_addresses = 0;



typedef struct _agent_nsap {
  int			handle;
  snmp_transport       *t;
  void		       *s;	/*  Opaque internal session pointer.  */
  struct _agent_nsap   *next;
} agent_nsap;

static	agent_nsap		*agent_nsap_list = NULL;
static struct agent_snmp_session *agent_session_list = NULL;
static struct agent_snmp_session *agent_delegated_list = NULL;


static void dump_var(oid *, size_t, int, void *, size_t);
int snmp_check_packet(struct snmp_session*, struct _snmp_transport *,
		      void *, int);
int snmp_check_parse(struct snmp_session*, struct snmp_pdu*, int);
void delete_subtree_cache(struct agent_snmp_session  *asp);
int handle_pdu(struct agent_snmp_session  *asp);
int wrap_up_request(struct agent_snmp_session *asp, int status);
int check_delayed_request(struct agent_snmp_session  *asp);
int handle_getnext_loop(struct agent_snmp_session  *asp);
int handle_set_loop(struct agent_snmp_session  *asp);

static void dump_var (
    oid *var_name,
    size_t var_name_len,
    int statType,
    void *statP,
    size_t statLen)
{
  size_t buf_len = SPRINT_MAX_LEN, out_len = 0;
  struct variable_list temp_var;
  u_char *buf = (u_char *)malloc(SPRINT_MAX_LEN);
  
  if (buf) {
    temp_var.type = statType;
    temp_var.val.string = (u_char *)statP;
    temp_var.val_len = statLen;
    sprint_realloc_variable(&buf, &buf_len, &out_len, 1,
			    var_name, var_name_len, &temp_var);
    snmp_log(LOG_INFO, "    >> %s\n", buf);
    free(buf);
  }
}


int getNextSessID()
{
    static int SessionID = 0;

    return ++SessionID;
}

int
agent_check_and_process(int block) {
  int numfds;
  fd_set fdset;
  struct timeval timeout = { LONG_MAX, 0 }, *tvp = &timeout;
  int count;
  int fakeblock=0;
  
  numfds = 0;
  FD_ZERO(&fdset);
  snmp_select_info(&numfds, &fdset, tvp, &fakeblock);
  if (block != 0 && fakeblock != 0) {
    /*  There are no alarms registered, and the caller asked for blocking, so
	let select() block forever.  */

    tvp = NULL;
  } else if (block != 0 && fakeblock == 0) {
    /*  The caller asked for blocking, but there is an alarm due sooner than
	LONG_MAX seconds from now, so use the modified timeout returned by
	snmp_select_info as the timeout for select().  */

  } else if (block == 0) {
    /*  The caller does not want us to block at all.  */

    tvp->tv_sec  = 0;
    tvp->tv_usec = 0;
  }

  count = select(numfds, &fdset, 0, 0, tvp);

  if (count > 0) {
    /* packets found, process them */
    snmp_read(&fdset);
  } else switch(count) {
    case 0:
      snmp_timeout();
      break;
    case -1:
      if (errno != EINTR) {
        snmp_log_perror("select");
      }
      return -1;
    default:
      snmp_log(LOG_ERR, "select returned %d\n", count);
      return -1;
  }  /* endif -- count>0 */

  /*  Run requested alarms.  */
  run_alarms();

  return count;
}



/*  Set up the address cache.  */
void snmp_addrcache_initialise(void)
{
  int i = 0;
  
  for (i = 0; i < SNMP_ADDRCACHE_SIZE; i++) {
    addrCache[i].addr = NULL;
    addrCache[i].status = SNMP_ADDRCACHE_UNUSED;
  }
}



/*  Age the entries in the address cache.  */

void snmp_addrcache_age(void)
{
  int i = 0;
  
  lastAddrAge = 0;
  for (i = 0; i < SNMP_ADDRCACHE_SIZE; i++) {
    if (addrCache[i].status == SNMP_ADDRCACHE_OLD) {
      addrCache[i].status = SNMP_ADDRCACHE_UNUSED;
      if (addrCache[i].addr != NULL) {
	free(addrCache[i].addr);
	addrCache[i].addr = NULL;
      }
    }
    if (addrCache[i].status == SNMP_ADDRCACHE_USED) {
      addrCache[i].status = SNMP_ADDRCACHE_OLD;
    }
  }
}

/*******************************************************************-o-******
 * snmp_check_packet
 *
 * Parameters:
 *	session, transport, transport_data, transport_data_length
 *      
 * Returns:
 *	1	On success.
 *	0	On error.
 *
 * Handler for all incoming messages (a.k.a. packets) for the agent.  If using
 * the libwrap utility, log the connection and deny/allow the access. Print
 * output when appropriate, and increment the incoming counter.
 *
 */

int
snmp_check_packet(struct snmp_session *session, snmp_transport *transport,
		  void *transport_data, int transport_data_length)
{
  char *addr_string = NULL;
  int i = 0;

  /*
   * Log the message and/or dump the message.
   * Optionally cache the network address of the sender.
   */

  if (transport != NULL && transport->f_fmtaddr != NULL) {
    /*  Okay I do know how to format this address for logging.  */
    addr_string = transport->f_fmtaddr(transport, transport_data,
				       transport_data_length);
    /*  Don't forget to free() it.  */
  } else {
    /*  Don't know how to format the address for logging.  */
    addr_string = strdup("<UNKNOWN>");
  }

#ifdef  USE_LIBWRAP
  if (hosts_ctl("snmpd", addr_string, addr_string, STRING_UNKNOWN)) {
    snmp_log(allow_severity, "Connection from %s\n", addr_string);
  } else {
    snmp_log(deny_severity, "Connection from %s REFUSED\n", addr_string);
    if (addr_string != NULL) {
      free(addr_string);
    }
    return 0;
  }
#endif/*USE_LIBWRAP*/

  snmp_increment_statistic(STAT_SNMPINPKTS);

  if (log_addresses || ds_get_boolean(DS_APPLICATION_ID, DS_AGENT_VERBOSE)) {
    
    for (i = 0; i < SNMP_ADDRCACHE_SIZE; i++) {
      if ((addrCache[i].status != SNMP_ADDRCACHE_UNUSED) &&
	  (strcmp(addrCache[i].addr, addr_string) == 0)) {
	break;
      }
    }

    if (i >= SNMP_ADDRCACHE_SIZE ||
	ds_get_boolean(DS_APPLICATION_ID, DS_AGENT_VERBOSE)){
      /*  Address wasn't in the cache, so log the packet...  */
      snmp_log(LOG_INFO, "Received SNMP packet(s) from %s\n", addr_string);
      /*  ...and try to cache the address.  */
      for (i = 0; i < SNMP_ADDRCACHE_SIZE; i++) {
	if (addrCache[i].status == SNMP_ADDRCACHE_UNUSED) {
	  if (addrCache[i].addr != NULL) {
	    free(addrCache[i].addr);
	  }
	  addrCache[i].addr   = addr_string;
	  addrCache[i].status = SNMP_ADDRCACHE_USED;
	  addr_string         = NULL;	/* Don't free this 'temporary' string
					   since it's now part of the cache */
	  break;
	}
      }
      if (i >= SNMP_ADDRCACHE_SIZE) {
	/*  We didn't find a free slot to cache the address.  Perhaps we
	    should be using an LRU replacement policy here or something.  Oh
	    well.  */
	DEBUGMSGTL(("snmp_check_packet", "cache overrun"));
      }
    } else {
      addrCache[i].status = SNMP_ADDRCACHE_USED;
    }
  }

  if (addr_string != NULL) {
    free(addr_string);
    addr_string = NULL;
  }
  return 1;
}


int snmp_check_parse(struct snmp_session *session, struct snmp_pdu *pdu,
		     int result)
{
  if (result == 0) {
    if (ds_get_boolean(DS_APPLICATION_ID, DS_AGENT_VERBOSE) &&
	snmp_get_do_logging()) {
      struct variable_list *var_ptr;
	    
      switch (pdu->command) {
      case SNMP_MSG_GET:
	snmp_log(LOG_DEBUG, "  GET message\n"); break;
      case SNMP_MSG_GETNEXT:
	snmp_log(LOG_DEBUG, "  GETNEXT message\n"); break;
      case SNMP_MSG_RESPONSE:
	snmp_log(LOG_DEBUG, "  RESPONSE message\n"); break;
      case SNMP_MSG_SET:
	snmp_log(LOG_DEBUG, "  SET message\n"); break;
      case SNMP_MSG_TRAP:
	snmp_log(LOG_DEBUG, "  TRAP message\n"); break;
      case SNMP_MSG_GETBULK:
	snmp_log(LOG_DEBUG, "  GETBULK message, non-rep=%d, max_rep=%d\n",
		 pdu->errstat, pdu->errindex); break;
      case SNMP_MSG_INFORM:
	snmp_log(LOG_DEBUG, "  INFORM message\n"); break;
      case SNMP_MSG_TRAP2:
	snmp_log(LOG_DEBUG, "  TRAP2 message\n"); break;
      case SNMP_MSG_REPORT:
	snmp_log(LOG_DEBUG, "  REPORT message\n"); break;
      }
	     
      for (var_ptr = pdu->variables; var_ptr != NULL;
	   var_ptr=var_ptr->next_variable) {
	size_t c_oidlen = 256, c_outlen = 0;
	u_char *c_oid = (u_char *)malloc(c_oidlen);

	if (c_oid) {
	  if (!sprint_realloc_objid(&c_oid, &c_oidlen, &c_outlen, 1,
				    var_ptr->name, var_ptr->name_length)) {
	    snmp_log(LOG_DEBUG, "    -- %s [TRUNCATED]\n", c_oid);
	  } else {
	    snmp_log(LOG_DEBUG, "    -- %s\n", c_oid);
	  }
	  free(c_oid);
	}
      }
    }
    return 1;
  }
  return 0; /* XXX: does it matter what the return value is?  Yes: if we
	       return 0, then the PDU is dumped.  */
}


/* Global access to the primary session structure for this agent.
   for Index Allocation use initially. */

/*  I don't understand what this is for at the moment.  AFAICS as long as it
    gets set and points at a session, that's fine.  ???  */

struct snmp_session *main_session = NULL;



/*  Set up an agent session on the given transport.  Return a handle
    which may later be used to de-register this transport.  A return
    value of -1 indicates an error.  */

int	register_agent_nsap	(snmp_transport *t)
{
  struct snmp_session *s, *sp = NULL;
  agent_nsap *a = NULL, *n = NULL, **prevNext = &agent_nsap_list;
  int handle = 0;
  void *isp = NULL;

  if (t == NULL) {
    return -1;
  }

  DEBUGMSGTL(("register_agent_nsap", "fd %d\n", t->sock));

  n = (agent_nsap *)malloc(sizeof(agent_nsap));
  if (n == NULL) {
    return -1;
  }
  s = (struct snmp_session *)malloc(sizeof(struct snmp_session));
  if (s == NULL) {
    free(n);
    return -1;
  }
  memset(s, 0, sizeof(struct snmp_session));
  snmp_sess_init(s);

  /*  Set up the session appropriately for an agent.  */

  s->version         = SNMP_DEFAULT_VERSION;
  s->callback        = handle_snmp_packet;
  s->authenticator   = NULL;
  s->flags           = ds_get_int(DS_APPLICATION_ID, DS_AGENT_FLAGS);
  s->isAuthoritative = SNMP_SESS_AUTHORITATIVE;

  sp  = snmp_add(s, t, snmp_check_packet, snmp_check_parse);
  if (sp == NULL) {
    free(s);
    free(n);
    return -1;
  }

  isp = snmp_sess_pointer(sp);
  if (isp == NULL) {	/*  over-cautious  */
    free(s);
    free(n);
    return -1;
  }

  n->s    = isp;
  n->t    = t;

  if (main_session == NULL) {
    main_session = snmp_sess_session(isp);
  }

  for (a = agent_nsap_list; a != NULL && handle+1 >= a->handle; a = a->next) {
    handle = a->handle;
    prevNext = &(a->next);
  }

  if (handle < INT_MAX) {
    n->handle = handle + 1;
    n->next   = a;
    *prevNext = n;
    free(s);
    return n->handle;
  } else {
    free(s);
    free(n);
    return -1;
  }
}



void	deregister_agent_nsap	(int handle)
{
  agent_nsap *a = NULL, **prevNext = &agent_nsap_list;
  int main_session_deregistered = 0;

  DEBUGMSGTL(("deregister_agent_nsap", "handle %d\n", handle));

  for (a = agent_nsap_list; a != NULL && a->handle < handle; a = a->next) {
    prevNext = &(a->next);
  }

  if (a != NULL && a->handle == handle) {
    *prevNext = a->next;
    if (main_session == snmp_sess_session(a->s)) {
      main_session_deregistered = 1;
    }
    snmp_close(snmp_sess_session(a->s));
    /*  The above free()s the transport and session pointers.  */
    free(a);
  }

  /*  If we've deregistered the session that main_session used to point to,
      then make it point to another one, or in the last resort, make it equal
      to NULL.  Basically this shouldn't ever happen in normal operation
      because main_session starts off pointing at the first session added by
      init_master_agent(), which then discards the handle.  */

  if (main_session_deregistered) {
    if (agent_nsap_list != NULL) {
      DEBUGMSGTL(("snmp_agent",
		  "WARNING: main_session pointer changed from %p to %p\n",
		  main_session, snmp_sess_session(agent_nsap_list->s)));
      main_session = snmp_sess_session(agent_nsap_list->s);
    } else {
      DEBUGMSGTL(("snmp_agent",
		  "WARNING: main_session pointer changed from %p to NULL\n",
		  main_session));
      main_session = NULL;
    }
  }
}



/* 

   This function has been modified to use the experimental register_agent_nsap
   interface.  The major responsibility of this function now is to interpret a
   string specified to the agent (via -p on the command line, or from a
   configuration file) as a list of agent NSAPs on which to listen for SNMP
   packets.  Typically, when you add a new transport domain "foo", you add
   code here such that if the "foo" code is compiled into the agent
   (SNMP_TRANSPORT_FOO_DOMAIN is defined), then a token of the form
   "foo:bletch-3a0054ef%wob&wob" gets turned into the appropriate transport
   descriptor.  register_agent_nsap is then called with that transport
   descriptor and sets up a listening agent session on it.

   Everything then works much as normal: the agent runs in an infinite loop
   (in the snmpd.c/receive()routine), which calls snmp_read() when a request
   is readable on any of the given transports.  This routine then traverses
   the library 'Sessions' list to identify the relevant session and eventually
   invokes '_sess_read'.  This then processes the incoming packet, calling the
   pre_parse, parse, post_parse and callback routines in turn.

   JBPN 20001117
*/

int
init_master_agent(void)
{
  snmp_transport *transport;
  char *cptr, *cptr2;
  char buf[SPRINT_MAX_LEN];

  if (ds_get_boolean(DS_APPLICATION_ID, DS_AGENT_ROLE) != MASTER_AGENT) {
    DEBUGMSGTL(("snmp_agent", "init_master_agent; not master agent\n"));
    return 0; /*  No error if ! MASTER_AGENT  */
  }

#ifdef USING_AGENTX_MASTER_MODULE
    if ( ds_get_boolean(DS_APPLICATION_ID, DS_AGENT_AGENTX_MASTER) == 1 )
        real_init_master();
#endif

  /*  Have specific agent ports been specified?  */
  cptr = ds_get_string(DS_APPLICATION_ID, DS_AGENT_PORTS);

  if (cptr) {
    sprintf(buf, "%s", cptr);
  } else {
    /*  No, so just specify the default port.  */
    if (ds_get_int(DS_APPLICATION_ID, DS_AGENT_FLAGS) &
	SNMP_FLAGS_STREAM_SOCKET) {
      sprintf(buf, "tcp:%d", SNMP_PORT);
    } else {
      sprintf(buf, "udp:%d", SNMP_PORT);
    }
  }

  DEBUGMSGTL(("snmp_agent", "final port spec: %s\n", buf));
  cptr = strtok(buf, ",");
  while (cptr) {
    /*  Specification format: 
	
	NONE:			  (a pseudo-transport)
	UDP:[address:]port        (also default if no transport is specified)
	TCP:[address:]port	  (if supported)
	Unix:pathname		  (if supported)
	AAL5PVC:itf.vpi.vci 	  (if supported)
        IPX:[network]:node[/port] (if supported)

    */

    DEBUGMSGTL(("snmp_agent", "installing master agent on port %s\n", cptr));

    if (!cptr || !(*cptr)) {
      snmp_log(LOG_ERR, "improper port specification\n");
      return 1;
    }

    /*  Transport type specifier?  */

    if ((cptr2 = strchr(cptr, ':')) != NULL) {
      if (strncasecmp(cptr, "none", 4) == 0) {
	DEBUGMSGTL(("snmp_agent",
		 "init_master_agent; pseudo-transport \"none\" requested\n"));
	return 0;
      } else if (strncasecmp(cptr, "tcp", 3) == 0) {
#ifdef SNMP_TRANSPORT_TCP_DOMAIN
	struct sockaddr_in addr;
	if (snmp_sockaddr_in(&addr, cptr2+1, 0)) {
	  transport = snmp_tcp_transport(&addr, 1);
	} else {
	  snmp_log(LOG_ERR,
		   "Badly formatted IP address (should be [a.b.c.d:]p)\n");
	  return 1;
	}
#else
	snmp_log(LOG_ERR, "No support for requested TCP domain\n");
	return 1;
#endif
      } else if (strncasecmp(cptr, "ipx", 3) == 0) {
#ifdef SNMP_TRANSPORT_IPX_DOMAIN
	struct sockaddr_ipx addr;
	if (snmp_sockaddr_ipx(&addr, cptr2+1)) {
	  transport = snmp_ipx_transport(&addr, 1);
	} else {
	  snmp_log(LOG_ERR,
	       "Badly formatted IPX address (should be [net]:node[/port])\n");
	  return 1;
	}
#else
	snmp_log(LOG_ERR, "No support for requested IPX domain\n");
#endif
      } else if (strncasecmp(cptr, "aal5pvc", 7) == 0) {
#ifdef SNMP_TRANSPORT_AAL5PVC_DOMAIN
	struct sockaddr_atmpvc addr;
	if (sscanf(cptr2+1, "%d.%d.%d", &(addr.sap_addr.itf),
		   &(addr.sap_addr.vpi), &(addr.sap_addr.vci))==3) {
	  addr.sap_family = AF_ATMPVC;
	  transport = snmp_aal5pvc_transport(&addr, 1);
	} else {
	  snmp_log(LOG_ERR,
		 "Badly formatted AAL5 PVC address (should be itf.vpi.vci)\n");
	  return 1;
	}
#else
	snmp_log(LOG_ERR, "No support for requested AAL5 PVC domain\n");
	return 1;
#endif
      } else if (strncasecmp(cptr, "unix", 4) == 0) {
#ifdef SNMP_TRANSPORT_UNIX_DOMAIN
	struct sockaddr_un addr;
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, cptr2+1, 
		sizeof(addr) - (size_t) (((struct sockaddr_un *)0)->sun_path));
	transport = snmp_unix_transport(&addr, 1);
#else
	snmp_log(LOG_ERR, "No support for requested Unix domain\n");
	return 1;
#endif
      } else if (strncasecmp(cptr, "udp", 3) == 0) {
	struct sockaddr_in addr;
	if (snmp_sockaddr_in(&addr, cptr2+1, 0)) {
	  transport = snmp_udp_transport(&addr, 1);
	} else {
	  snmp_log(LOG_ERR,
		   "Badly formatted IP address (should be [a.b.c.d:]p)\n");
	  return 1;
	}
      } else {
	snmp_log(LOG_ERR, "Unknown transport domain \"%s\"\n", cptr);
	return 1;
      }
    } else {
      /*  No transport type specifier; default to UDP.  */
      struct sockaddr_in addr;
      if (snmp_sockaddr_in(&addr, cptr, 0)) {
	transport = snmp_udp_transport(&addr, 1);
      } else {
	snmp_log(LOG_ERR,
		 "Badly formatted IP address (should be [a.b.c.d:]p)\n");
	return 1;
      }
    }

    if (transport == NULL) {
      snmp_log(LOG_ERR, "Error opening specified transport \"%s\"\n", cptr);
      return 1;
    }

    if (register_agent_nsap(transport) < 0) {
      snmp_log(LOG_ERR,
	     "Error registering specified transport \"%s\" as an agent NSAP\n",
	       cptr);
      return 1;
    } else {
      DEBUGMSGTL(("snmp_agent",
		  "init_master_agent; \"%s\" registered as an agent NSAP\n",
		  cptr));
    }

    /*  Next transport please...  */
    cptr = strtok(NULL, ",");
  }
  return 0;
}



struct agent_snmp_session  *
init_agent_snmp_session( struct snmp_session *session, struct snmp_pdu *pdu )
{
    struct agent_snmp_session  *asp;

    asp = (struct agent_snmp_session *) calloc(1, sizeof( struct agent_snmp_session ));

    if ( asp == NULL )
	return NULL;
    asp->session = session;
    asp->pdu      = snmp_clone_pdu(pdu);
    asp->orig_pdu = snmp_clone_pdu(pdu);
    asp->rw      = READ;
    asp->exact   = TRUE;
    asp->outstanding_requests = NULL;
    asp->next    = NULL;
    asp->mode    = RESERVE1;
    asp->status  = SNMP_ERR_NOERROR;
    asp->index   = 0;

    asp->start = asp->pdu->variables;
    asp->end   = asp->pdu->variables;
    if ( asp->end != NULL )
	while ( asp->end->next_variable != NULL )
	    asp->end = asp->end->next_variable;

    return asp;
}

void
free_agent_snmp_session(struct agent_snmp_session *asp)
{
    if (!asp)
	return;
    if (asp->orig_pdu)
	snmp_free_pdu(asp->orig_pdu);
    if (asp->pdu)
	snmp_free_pdu(asp->pdu);
    if (asp->treecache) {
        delete_subtree_cache(asp);
        free(asp->treecache);
    }
    free(asp);
}

int
check_for_delegated(struct agent_snmp_session *asp) {
    int i;
    request_info *request;
    
    for(i = 0; i <= asp->treecache_num; i++) {
        for(request = asp->treecache[i]->requests_begin; request;
            request = request->next) {
            if (request->delegated)
                return 1;
        }
    }
    return 0;
}


int
wrap_up_request(struct agent_snmp_session *asp, int status) {
    struct variable_list *var_ptr;
    int i;

    /*
     * May need to "dumb down" a SET error status for a
     * v1 query.  See RFC2576 - section 4.3
     */
    if (( asp->pdu                          ) &&
        ( asp->pdu->command == SNMP_MSG_SET ) &&
        ( asp->pdu->version == SNMP_VERSION_1 )) {
        switch ( status ) {
            case SNMP_ERR_WRONGVALUE:
            case SNMP_ERR_WRONGENCODING:
            case SNMP_ERR_WRONGTYPE:
            case SNMP_ERR_WRONGLENGTH:
            case SNMP_ERR_INCONSISTENTVALUE:
                status = SNMP_ERR_BADVALUE;
                break;
            case SNMP_ERR_NOACCESS:
            case SNMP_ERR_NOTWRITABLE:
            case SNMP_ERR_NOCREATION:
            case SNMP_ERR_INCONSISTENTNAME:
            case SNMP_ERR_AUTHORIZATIONERROR:
                status = SNMP_ERR_NOSUCHNAME;
                break;
            case SNMP_ERR_RESOURCEUNAVAILABLE:
            case SNMP_ERR_COMMITFAILED:
            case SNMP_ERR_UNDOFAILED:
                status = SNMP_ERR_GENERR;
                break;
        }
    }
    /*
     * Similarly we may need to "dumb down" v2 exception
     *  types to throw an error for a v1 query.
     *  See RFC2576 - section 4.1.2.3
     */
    if (( asp->pdu                          ) &&
        ( asp->pdu->command != SNMP_MSG_SET ) &&
        ( asp->pdu->version == SNMP_VERSION_1 )) {
        for ( var_ptr = asp->pdu->variables, i=1 ;
              var_ptr != NULL ;
              var_ptr = var_ptr->next_variable, i++ ) {
            switch ( var_ptr->type ) {
                case SNMP_NOSUCHOBJECT:
                case SNMP_NOSUCHINSTANCE:
                case SNMP_ENDOFMIBVIEW:
                case ASN_COUNTER64:
                    status = SNMP_ERR_NOSUCHNAME;
                    asp->index=i;
                    break;
            }
        }
    }
    if (( status == SNMP_ERR_NOERROR ) && ( asp->pdu )) {
        snmp_increment_statistic_by(
            (asp->pdu->command == SNMP_MSG_SET ?
             STAT_SNMPINTOTALSETVARS : STAT_SNMPINTOTALREQVARS ),
            count_varbinds( asp->pdu->variables ));
    }
    else {
        /*
         * Use a copy of the original request
         *   to report failures.
         */
        snmp_free_pdu( asp->pdu );
        asp->pdu = asp->orig_pdu;
        asp->orig_pdu = NULL;
    }
    if ( asp->pdu ) {
        asp->pdu->command  = SNMP_MSG_RESPONSE;
        asp->pdu->errstat  = asp->status;
        asp->pdu->errindex = asp->index;
        if (! snmp_send( asp->session, asp->pdu ))
            snmp_free_pdu(asp->pdu);
        snmp_increment_statistic(STAT_SNMPOUTPKTS);
        snmp_increment_statistic(STAT_SNMPOUTGETRESPONSES);
        asp->pdu = NULL;
        remove_and_free_agent_snmp_session(asp);
    }
    return 1;
}


#define NEWAPI

void
dump_sess_list(void)
{
    struct agent_snmp_session *a;
    
    DEBUGMSGTL(("snmp_agent", "DUMP agent_sess_list -> "));
    for (a = agent_session_list; a != NULL; a = a->next) {
	DEBUGMSG(("snmp_agent", "%08p[session %08p] -> ", a, a->session));
    }
    DEBUGMSG(("snmp_agent", "[NIL]\n"));
}

void
remove_and_free_agent_snmp_session(struct agent_snmp_session *asp)
{
    struct agent_snmp_session *a, **prevNext = &agent_session_list;

    DEBUGMSGTL(("snmp_agent", "REMOVE %08p\n", asp));

    for (a = agent_session_list; a != NULL; a = *prevNext) {
	if (a == asp) {
	    *prevNext = a->next;
	    a->next = NULL;
	    free_agent_snmp_session(a);
	    asp = NULL;
	    break;
	} else {
	    prevNext = &(a->next);
	}
    }

    if (a == NULL && asp != NULL) {
	/*  We coulnd't find it on the list, so free it anyway.  */
	free_agent_snmp_session(asp);
    }
}

void
free_agent_snmp_session_by_session(struct snmp_session *sess,
				   void (*free_request)(struct request_list *))
{
    struct agent_snmp_session *a, *next, **prevNext = &agent_session_list;

    DEBUGMSGTL(("snmp_agent", "REMOVE session == %08p\n", sess));

    for (a = agent_session_list; a != NULL; a = next) {
	if (a->session == sess) {
	    *prevNext = a->next;
	    next = a->next;
	    if (a->outstanding_requests != NULL && free_request) {
		struct request_list *r = a->outstanding_requests, *s = NULL;

		DEBUGMSGTL(("snmp_agent", "  has outstanding requests\n"));
		for (; r != NULL; r = s) {
		    s = r->next_request;
		    free_request(r);
		}
	    }
	    free_agent_snmp_session(a);
	} else {
	    prevNext = &(a->next);
	    next = a->next;
	}
    }
}

int
handle_snmp_packet(int op, struct snmp_session *session, int reqid,
                   struct snmp_pdu *pdu, void *magic)
{
    struct agent_snmp_session  *asp;
    int status;
    struct variable_list *var_ptr;
#ifndef NEWAPI
    int allDone, i;
    struct variable_list *var_ptr2;
#endif

    if (op != SNMP_CALLBACK_OP_RECEIVED_MESSAGE) {
      return 1;
    }

    if ( magic == NULL ) {
        /* new request */
	asp = init_agent_snmp_session( session, pdu );
	status = SNMP_ERR_NOERROR;
    } else {
        /* old request with updated information */
	asp = (struct agent_snmp_session *)magic;
        status =   asp->status;
    }

    if (asp->outstanding_requests != NULL) {
      	DEBUGMSGTL(("snmp_handle_packet", "asp->requests != NULL\n"));
	return 1;
    }

    if ( check_access(pdu) != 0) {
        /* access control setup is incorrect */
	send_easy_trap(SNMP_TRAP_AUTHFAIL, 0);
        if (asp->pdu->version != SNMP_VERSION_1 && asp->pdu->version != SNMP_VERSION_2c) {
            asp->pdu->errstat = SNMP_ERR_AUTHORIZATIONERROR;
            asp->pdu->command = SNMP_MSG_RESPONSE;
            snmp_increment_statistic(STAT_SNMPOUTPKTS);
            if (! snmp_send( asp->session, asp->pdu ))
	        snmp_free_pdu(asp->pdu);
	    asp->pdu = NULL;
	    remove_and_free_agent_snmp_session(asp);
            return 1;
        } else {
            /* drop the request */
            remove_and_free_agent_snmp_session( asp );
            return 0;
        }
    }

#ifdef NEWAPI
    status = handle_pdu(asp);
    DEBUGIF("results") {
        DEBUGMSGTL(("results","request results (status = %d): \n", status));
        for(var_ptr = asp->pdu->variables; var_ptr;
            var_ptr = var_ptr->next_variable) {
            char buf[SPRINT_MAX_LEN];
            sprint_variable(buf, var_ptr->name, var_ptr->name_length, var_ptr);
            DEBUGMSGTL(("results","  %s\n", buf));
        }
    }
#else
    switch (pdu->command) {
    case SNMP_MSG_GET:
	if ( asp->mode != RESERVE1 )
	    break;			/* Single pass */
        snmp_increment_statistic(STAT_SNMPINGETREQUESTS);
	status = handle_next_pass( asp );
	asp->mode = RESERVE2;
	break;

    case SNMP_MSG_GETNEXT:
	if ( asp->mode != RESERVE1 )
	    break;			/* Single pass */
        snmp_increment_statistic(STAT_SNMPINGETNEXTS);
	asp->exact   = FALSE;
	status = handle_next_pass( asp );
	asp->mode = RESERVE2;
	break;

    case SNMP_MSG_GETBULK:
	    /*
	     * GETBULKS require multiple passes. The first pass handles the
	     * explicitly requested varbinds, and subsequent passes append
	     * to the existing var_op_list.  Each pass (after the first)
	     * uses the results of the preceeding pass as the input list
	     * (delimited by the start & end pointers.
	     * Processing is terminated if all entries in a pass are
	     * EndOfMib, or the maximum number of repetitions are made.
	     */
	if ( asp->mode == RESERVE1 ) {
            snmp_increment_statistic(STAT_SNMPINGETREQUESTS);
	    asp->exact   = FALSE;
		    /*
		     * Limit max repetitions to something reasonable
		     *	XXX: We should figure out what will fit somehow...
		     */
	    if ( asp->pdu->errindex > 100 )
	        asp->pdu->errindex = 100;
    
		    /*
		     * If max-repetitions is 0, we shouldn't
		     *   process the non-nonrepeaters at all
		     *   so set up 'asp->end' accordingly
		     */
	    if ( asp->pdu->errindex == 0 ) {
		if ( asp->pdu->errstat == 0 ) {
				/* Nothing to do at all */
		    snmp_free_varbind(asp->pdu->variables);
		    asp->pdu->variables=NULL;
		    asp->start=NULL;
		}
		else {
		    asp->end   = asp->pdu->variables;
		    i = asp->pdu->errstat;
		    while ( --i > 0 ) 
			if ( asp->end )
			    asp->end = asp->end->next_variable;
		    snmp_free_varbind(asp->end->next_variable);
		    asp->end->next_variable = NULL;
		}
	    }

	    status = handle_next_pass( asp );	/* First pass */
	    asp->mode = RESERVE2;
	    if ( status != SNMP_ERR_NOERROR )
	        break;
    
	    while ( asp->pdu->errstat-- > 0 )	/* Skip non-repeaters */
	    {
	        if ( NULL != asp->start )	/* if there are variables ... */
		    asp->start = asp->start->next_variable;
	    }
	    asp->pdu->errindex--;           /* Handled first repetition */

	    if ( asp->outstanding_requests != NULL )
		return 1;
	}

	if ( NULL != asp->start )	/* if there are variables ... */
	while ( asp->pdu->errindex-- > 0 ) {	/* Process repeaters */
		/*
		 * Add new variable structures for the
		 * repeating elements, ready for the next pass.
		 * Also check that these are not all EndOfMib
		 */
	    allDone = TRUE;		/* Check for some content */
	    for ( var_ptr = asp->start;
		  var_ptr != asp->end->next_variable;
		  var_ptr = var_ptr->next_variable ) {
				/* XXX: we don't know the size of the next
					OID, so assume the maximum length */
		if ( var_ptr->type != SNMP_ENDOFMIBVIEW ) {
		    var_ptr2 = snmp_add_null_var(asp->pdu, var_ptr->name, MAX_OID_LEN);
		    for ( i=var_ptr->name_length ; i<MAX_OID_LEN ; i++)
			var_ptr2->name[i] = 0;
		    var_ptr2->name_length = var_ptr->name_length;

		    allDone = FALSE;
		}
	    }
	    if ( allDone )
		break;

	    asp->start = asp->end->next_variable;
	    while ( asp->end->next_variable != NULL )
		asp->end = asp->end->next_variable;
	    
	    status = handle_next_pass( asp );
	    if ( status != SNMP_ERR_NOERROR )
		break;
	    if ( asp->outstanding_requests != NULL )
		return 1;
	}
	break;

    case SNMP_MSG_SET:
    	    /*
	     * SETS require 3-4 passes through the var_op_list.  The first two
	     * passes verify that all types, lengths, and values are valid
	     * and may reserve resources and the third does the set and a
	     * fourth executes any actions.  Then the identical GET RESPONSE
	     * packet is returned.
	     * If either of the first two passes returns an error, another
	     * pass is made so that any reserved resources can be freed.
	     * If the third pass returns an error, another pass is made so that
	     * any changes can be reversed.
	     * If the fourth pass (or any of the error handling passes)
	     * return an error, we'd rather not know about it!
	     */
	if ( asp->mode == RESERVE1 ) {
            snmp_increment_statistic(STAT_SNMPINSETREQUESTS);
	    asp->rw      = WRITE;

	    status = handle_next_pass( asp );

	    if ( status != SNMP_ERR_NOERROR )
	        asp->mode = FREE;
	    else
	        asp->mode = RESERVE2;

	    if ( asp->outstanding_requests != NULL )
		return 1;
	}

	if ( asp->mode == RESERVE2 ) {
	    status = handle_next_pass( asp );

	    if ( status != SNMP_ERR_NOERROR )
	        asp->mode = FREE;
	    else
	        asp->mode = ACTION;

	    if ( asp->outstanding_requests != NULL )
		return 1;
	}

	if ( asp->mode == ACTION ) {
	    status = handle_next_pass( asp );

	    if ( status != SNMP_ERR_NOERROR )
	        asp->mode = UNDO;
	    else
	        asp->mode = COMMIT;

	    if ( asp->outstanding_requests != NULL )
		return 1;
	}

	if ( asp->mode == COMMIT ) {
	    status = handle_next_pass( asp );

	    if ( status != SNMP_ERR_NOERROR ) {
		status    = SNMP_ERR_COMMITFAILED;
	        asp->mode = FINISHED_FAILURE;
	    }
	    else
	        asp->mode = FINISHED_SUCCESS;

	    if ( asp->outstanding_requests != NULL )
		return 1;
	}

	if ( asp->mode == UNDO ) {
	    if (handle_next_pass( asp ) != SNMP_ERR_NOERROR )
		status = SNMP_ERR_UNDOFAILED;

	    asp->mode = FINISHED_FAILURE;
	    break;
	}

	if ( asp->mode == FREE ) {
	    (void) handle_next_pass( asp );
	    break;
	}

	break;

    case SNMP_MSG_RESPONSE:
        snmp_increment_statistic(STAT_SNMPINGETRESPONSES);
	remove_and_free_agent_snmp_session( asp );
	return 0;
    case SNMP_MSG_TRAP:
    case SNMP_MSG_TRAP2:
        snmp_increment_statistic(STAT_SNMPINTRAPS);
	remove_and_free_agent_snmp_session( asp );
	return 0;
    default:
        snmp_increment_statistic(STAT_SNMPINASNPARSEERRS);
	remove_and_free_agent_snmp_session( asp );
	return 0;
    }

#endif /* !NEWAPI */
    if (check_for_delegated(asp)) {
        /* add to delegated request chain */
	asp->status = status;
	asp->next = agent_delegated_list;
	agent_delegated_list = asp;
    }
    else if ( asp->outstanding_requests != NULL ) {
        /* WWW: move or kill this stuff */
	struct agent_snmp_session *a = agent_session_list;
	asp->status = status;
	/*  Careful not to add duplicates.  */

	for (; a != NULL; a = a->next) {
	    if (a == asp) {
		break;
	    }
	}
	if (a == NULL) {
	    asp->next = agent_session_list;
	    agent_session_list = asp;
	    DEBUGMSGTL(("snmp_agent", "ADDED %08p agent_sess_list -> ", asp));
	    for (a = agent_session_list; a != NULL; a = a->next) {
		DEBUGMSG(("snmp_agent", "%08p -> ", a));
	    }
	    DEBUGMSG(("snmp_agent", "[NIL]\n"));
	} else {
	    DEBUGMSGTL(("snmp_agent", "DID NOT ADD %08p (duplicate)\n", asp));
	}
    }
    
    else {
        /* if we don't have anything outstanding (delegated), wrap up */
        if (!check_for_delegated(asp))
            return wrap_up_request(asp, status);
    }

    DEBUGMSGTL(("snmp_agent", "end of handle_snmp_packet, asp = %08p\n", asp));
    return 1;
}

int
handle_next_pass(struct agent_snmp_session  *asp)
{
    int status;
    struct request_list *req_p, *next_req;


        if ( asp->outstanding_requests != NULL )
	    return SNMP_ERR_NOERROR;
	status = handle_var_list( asp );
        if ( asp->outstanding_requests != NULL ) {
	    if ( status == SNMP_ERR_NOERROR ) {
		/* Send out any subagent requests */
		for ( req_p = asp->outstanding_requests ;
			req_p != NULL ; req_p = next_req ) {

		    next_req = req_p->next_request;
		    if ( snmp_async_send( req_p->session,  req_p->pdu,
				      req_p->callback, req_p->cb_data ) == 0) {
				/*
				 * Send failed - call callback to handle this
				 */
			(void)req_p->callback( SNMP_CALLBACK_OP_SEND_FAILED,
					 req_p->session,
					 req_p->pdu->reqid,
					 req_p->pdu,
					 req_p->cb_data );
			return SNMP_ERR_GENERR;
		    }
		}
	    }
	    else {
	    	/* discard outstanding requests */
		for ( req_p = asp->outstanding_requests ;
			req_p != NULL ; req_p = next_req ) {
			
			next_req = req_p->next_request;
			free( req_p );
		}
		asp->outstanding_requests = NULL;
	    }
	}
	return status;
}

	/*
	 *  Private structure to save the results of a getStatPtr call.
	 *  This data can then be used to avoid repeating this call on
	 *  subsequent SET handling passes.
	 */
struct saved_var_data {
    WriteMethod *write_method;
    u_char	*statP;
    u_char	statType;
    size_t	statLen;
    u_short	acl;
};

int
add_varbind_to_cache(struct agent_snmp_session  *asp, int vbcount,
                     struct variable_list *varbind_ptr, struct subtree *tp) {
    request_info *request;
    int cacheid;
    tree_cache *tmpc;

    if (tp == NULL) {
        /* no appropriate registration found */
        /* make up the response ourselves */
        switch(asp->pdu->command) {
            case SNMP_MSG_GETNEXT:
            case SNMP_MSG_GETBULK:
                varbind_ptr->type = SNMP_ENDOFMIBVIEW;
                break;
                    
            case SNMP_MSG_SET:
                return SNMP_NOSUCHOBJECT;

            case SNMP_MSG_GET:
                varbind_ptr->type = SNMP_NOSUCHOBJECT;
                break;

            default:
                return SNMPERR_GENERR; /* shouldn't get here */
        }
    } else {
        if (asp->pdu->command != SNMP_MSG_SET)
            varbind_ptr->type = ASN_NULL;

        /* malloc the request structure */
        request = SNMP_MALLOC_TYPEDEF(request_info);
        if (request == NULL)
            return SNMP_ERR_GENERR;
        request->index = vbcount;

        /* place them in a cache */
        if (tp->cacheid > -1 && tp->cacheid <= asp->treecache_num &&
            asp->treecache[tp->cacheid]->subtree == tp) {
            /* we have already added a request to this tree
                   pointer before */
					cacheid = tp->cacheid;

        } else {
            cacheid = ++(asp->treecache_num);
            /* new slot needed */
            if (asp->treecache_num >= asp->treecache_len) {
                /* exapand cache array */
                /* WWW: non-linear expansion needed (with cap) */
                asp->treecache_len = (asp->treecache_len + 16);
                asp->treecache = realloc(asp->treecache,
                                         sizeof(tree_cache *) *
                                         asp->treecache_len);
                if (asp->treecache == NULL)
                    return SNMP_ERR_GENERR;
            }
            tmpc = (tree_cache *) calloc(1, sizeof(tree_cache));
            asp->treecache[cacheid] = tmpc;
            asp->treecache[cacheid]->subtree = tp;
            asp->treecache[cacheid]->requests_begin = request;
            tp->cacheid = cacheid;
        }

        /* link into chain */
        if (asp->treecache[cacheid]->requests_end)
            asp->treecache[cacheid]->requests_end->next = request;
        request->prev =
            asp->treecache[cacheid]->requests_end;
        asp->treecache[cacheid]->requests_end = request;

        /* add the given request to the list of requests they need
               to handle results for */
        request->requestvb = varbind_ptr;
    }
    return SNMP_ERR_NOERROR;
}

/* check the ACM(s) for the results on each of the varbinds.
   If ACM disallows it, replace the value with type

   Returns number of varbinds with ACM errors
*/
int
check_acm(struct agent_snmp_session  *asp, u_char type) {
    int view;
    int i;
    request_info *request;
    int ret = 0;
    struct variable_list *vb;
    
    for(i = 0; i <= asp->treecache_num; i++) {
        for(request = asp->treecache[i]->requests_begin;
            request; request = request->next) {
            /* for each request, run it through in_a_view() */
            vb = request->requestvb;
            if (vb->type == ASN_NULL) /* not yet processed */
                continue;
            view = in_a_view(vb->name, &vb->name_length, asp->pdu, vb->type);

            /* if a ACM error occurs, mark it as type passed in */
            if (view != VACM_SUCCESS) {
                ret++;
                snmp_set_var_typed_value(vb, type, NULL, 0);
            }
        }
    }
    return ret;
}


int
create_subtree_cache(struct agent_snmp_session  *asp) {
    struct subtree *tp;
    struct variable_list *varbind_ptr;
    int ret;
    int view;
    int vbcount = 0;

    if (asp->treecache == NULL &&
        asp->treecache_len == 0) {
        asp->treecache_len = 16;
        asp->treecache = malloc(sizeof(tree_cache *) * asp->treecache_len);
        if (asp->treecache == NULL)
            return SNMP_ERR_GENERR;
    }
    asp->treecache_num = -1;

    /* collect varbinds into their registered trees */

    for(varbind_ptr = asp->start; varbind_ptr;
        varbind_ptr = varbind_ptr->next_variable) {

        /* count the varbinds */
        ++vbcount;
        
        if (varbind_ptr->type != ASN_NULL &&  /* skip previously answered */
            asp->pdu->command != SNMP_MSG_SET)
            continue;

        /* find the owning tree */
        tp = find_subtree(varbind_ptr->name, varbind_ptr->name_length, NULL);

        /* check access control */
        switch(asp->pdu->command) {
            case SNMP_MSG_GET:
                view = in_a_view(varbind_ptr->name, &varbind_ptr->name_length,
                                 asp->pdu, varbind_ptr->type);
                if (view != VACM_SUCCESS)
                    snmp_set_var_typed_value(varbind_ptr, SNMP_NOSUCHOBJECT,
                                             NULL, 0);
                break;

            case SNMP_MSG_SET:
                view = in_a_view(varbind_ptr->name, &varbind_ptr->name_length,
                                 asp->pdu, varbind_ptr->type);
                if (view != VACM_SUCCESS)
                    return SNMP_ERR_NOTWRITABLE;
                break;

            case SNMP_MSG_GETNEXT:
            default:
                view = VACM_SUCCESS;
                /* WWW: check VACM here to see if "tp" is even worthwhile */
        }
        if (view == VACM_SUCCESS) {
            ret = add_varbind_to_cache(asp, vbcount, varbind_ptr, tp);
            if (ret != SNMP_ERR_NOERROR)
                return ret;
        }
    }

    return SNMPERR_SUCCESS;
}

/* this function is only applicable in getnext like contexts */
int
reassign_requests(struct agent_snmp_session  *asp) {
    /* assume all the requests have been filled or rejected by the
       subtrees, so reassign the rejected ones to the next subtree in
       the chain */

    int i, ret;
    request_info *request, *lastreq = NULL;

    /* get old info */
    tree_cache **old_treecache = asp->treecache;
    int old_treecache_num = asp->treecache_num;

    /* malloc new space */
    asp->treecache =
        (tree_cache **) malloc(sizeof(tree_cache *) * asp->treecache_len);
    asp->treecache_num = -1;

    for(i = 0; i <= old_treecache_num; i++) {
        for(request = old_treecache[i]->requests_begin; request;
            request = request->next) {

            if (lastreq)
                free(lastreq);
            lastreq = request;
            
            if (request->requestvb->type == ASN_NULL) {
                ret = add_varbind_to_cache(asp, request->index,
                                           request->requestvb,
                                           old_treecache[i]->subtree->next);
                if (ret != SNMP_ERR_NOERROR)
                    return ret; /* WWW: mem leak */
            } else if (request->requestvb->type == ASN_PRIV_RETRY) {
                /* re-add the same subtree */
                request->requestvb->type = ASN_NULL;
                ret = add_varbind_to_cache(asp, request->index,
                                           request->requestvb,
                                           old_treecache[i]->subtree);
                if (ret != SNMP_ERR_NOERROR)
                    return ret; /* WWW: mem leak */
            }
        }
    }
    if (lastreq)
        free(lastreq);
    free(old_treecache);
    return SNMP_ERR_NOERROR;
}

void
delete_request_infos(request_info *reqlist) {
    request_info *saveit;
    while(reqlist) {
        /* don't delete varbind */
        saveit = reqlist;
        reqlist = reqlist->next;
        free(reqlist);
    }
}

void
delete_subtree_cache(struct agent_snmp_session  *asp) {
    while(asp->treecache_num > 0) {
        /* don't delete subtrees */
        delete_request_infos(asp->treecache[asp->treecache_num]
                             ->requests_begin);
        free(asp->treecache[asp->treecache_num]);
        asp->treecache_num--;
    }
}

int
check_requests_status(struct agent_snmp_session  *asp, request_info *requests) {
    /* find any errors marked in the requests */
    while(requests) {
        if (requests->status != SNMP_ERR_NOERROR &&
            (asp->index == 0 ||
             requests->index < asp->index)) {
            asp->index = requests->index;
            asp->status = requests->status;
        }
        requests = requests->next;
    }
    return asp->status;
}

int
check_all_requests_status(struct agent_snmp_session  *asp) {
    int i;
    for(i = 0; i <= asp->treecache_num; i++) {
        check_requests_status(asp, asp->treecache[i]->requests_begin);
    }
    return asp->status;
}

int
handle_var_requests(struct agent_snmp_session  *asp) {
    int i, status = SNMP_ERR_NOERROR, final_status = SNMP_ERR_NOERROR;
    handler_registration *reginfo;

    /* create the agent_request_info data */
    if (!asp->reqinfo) {
        asp->reqinfo = SNMP_MALLOC_TYPEDEF(agent_request_info);
        if (!asp->reqinfo)
            return SNMP_ERR_GENERR;
        asp->reqinfo->asp = asp;
    }

    asp->reqinfo->mode = asp->mode;

    /* now, have the subtrees in the cache go search for their results */
    for(i=0; i <= asp->treecache_num; i++) {
        reginfo = asp->treecache[i]->subtree->reginfo;
        status = call_handlers(reginfo, asp->reqinfo,
                               asp->treecache[i]->requests_begin);

        /* find any errors marked in the requests */
        i = check_requests_status(asp, asp->treecache[i]->requests_begin);
        if (i != SNMP_ERR_NOERROR) /* always take lowest varbind if possible */
            status = i;
        
        /* other things we know less about (no index) */
        /* WWW: drop support for this? */
        if (final_status == SNMP_ERR_NOERROR &&
            status != SNMP_ERR_NOERROR) {
            /* we can't break here, since some processing needs to be
               done for all requests anyway (IE, SET handling for UNDO
               needs to be called regardless of previous status
               results.
               WWW:  This should be predictable though and
               breaking should be possible in some cases (eg GET,
               GETNEXT, ...) */
            final_status = status;
        }
    }

   return final_status;
}

/* loop through our sessions known delegated sessions and check to see
   if they've completed yet */ 
void
check_outstanding_agent_requests(int status) {
    struct agent_snmp_session *asp, *prev_asp = NULL;

    for(asp = agent_delegated_list; asp; prev_asp = asp, asp = asp->next) {
        if (!check_for_delegated(asp)) {

            /* we're done with this one, remove from queue */
            if (prev_asp != NULL)
                prev_asp->next = asp->next;
            else
                agent_delegated_list = asp->next;

            /* continue processing or finish up */
            check_delayed_request(asp);
        }
    }
}

/*
 * check_delayed_request(asp)
 *
 * Called to rexamine a set of requests and continue processing them
 * once all the previous (delayed) requests have been handled one way
 * or another.
 */

int
check_delayed_request(struct agent_snmp_session  *asp) {
    int status = SNMP_ERR_NOERROR;
    
    check_all_requests_status(asp); /* update the asp->status */

    switch(asp->mode) {
        case SNMP_MSG_GETNEXT:
            handle_getnext_loop(asp);
            break;

        case SNMP_MSG_GETBULK:
            /* WWW */
            break;

        case SNMP_MSG_SET:
            handle_set_loop(asp);
            if (asp->mode != FINISHED_SUCCESS && asp->mode != FINISHED_FAILURE)
                return SNMP_ERR_NOERROR;
            break;

        default:
            break;
    }

    if ( asp->outstanding_requests != NULL ) {
	asp->status = status;
	asp->next = agent_session_list;
	agent_session_list = asp;
    }
    else {
        /* if we don't have anything outstanding (delegated), wrap up */
        if (!check_for_delegated(asp))
            return wrap_up_request(asp, status);
    }

    return 1;
}

/* it's expected that one pass has been made before entering this function */
int
handle_getnext_loop(struct agent_snmp_session  *asp) {
    int status;
    struct variable_list *var_ptr;
    int count;

    /* loop */
    while (1) {

        /* WWW: check to see that old request didn't pass
           end range */
        /* or, implement in a "bad_handler" helper (heh)? */

        /* check vacm against results */
        check_acm(asp, ASN_PRIV_RETRY);

        /* bail for now if anything is delegated. */
        if (check_for_delegated(asp)) {
                return SNMP_ERR_NOERROR;
        }

        /* need to keep going we're not done yet. */
        for(var_ptr = asp->pdu->variables, count = 0; var_ptr;
            var_ptr = var_ptr->next_variable) {
            count++;
            if (var_ptr->type == ASN_NULL || var_ptr->type == ASN_PRIV_RETRY)
                break;
        }

        /* nothing left, quit now */
        if (!var_ptr)
            break;

        /* never had a request (empty pdu), quit now */
        if (count == 0)
            break;
        
        DEBUGIF("results") {
            DEBUGMSGTL(("results","getnext results, before next pass: \n"));
            for(var_ptr = asp->pdu->variables; var_ptr;
                var_ptr = var_ptr->next_variable) {
                char buf[SPRINT_MAX_LEN];
                sprint_variable(buf, var_ptr->name, var_ptr->name_length, var_ptr);
                DEBUGMSGTL(("results","  %s\n", buf));
            }
        }

        reassign_requests(asp);
        status = handle_var_requests(asp);
        if (status != SNMP_ERR_NOERROR) {
            return status; /* should never really happen */
        }
    }
    return SNMP_ERR_NOERROR;
}

int
handle_set(struct agent_snmp_session  *asp) {
    int status;
    /*
     * SETS require 3-4 passes through the var_op_list.
     * The first two
     * passes verify that all types, lengths, and values are valid
     * and may reserve resources and the third does the set and a
     * fourth executes any actions.  Then the identical GET RESPONSE
     * packet is returned.
     * If either of the first two passes returns an error, another
     * pass is made so that any reserved resources can be freed.
     * If the third pass returns an error, another pass is
     * made so that
     * any changes can be reversed.
     * If the fourth pass (or any of the error handling passes)
     * return an error, we'd rather not know about it!
     */
    switch (asp->mode) {
        case MODE_SET_BEGIN:
            snmp_increment_statistic(STAT_SNMPINSETREQUESTS);
            asp->rw      = WRITE; /* WWW: still needed? */
            asp->mode = MODE_SET_RESERVE1;
            asp->status = SNMP_ERR_NOERROR;
            break;
            
        case MODE_SET_RESERVE1:

            if ( asp->status != SNMP_ERR_NOERROR )
                asp->mode = MODE_SET_FREE;
            else
                asp->mode = MODE_SET_RESERVE2;
            break;

        case MODE_SET_RESERVE2:
            if ( asp->status != SNMP_ERR_NOERROR )
                asp->mode = MODE_SET_FREE;
            else
                asp->mode = MODE_SET_ACTION;
            break;

        case MODE_SET_ACTION:
            if ( asp->status != SNMP_ERR_NOERROR )
                asp->mode = MODE_SET_UNDO;
            else
                asp->mode = MODE_SET_COMMIT;
            break;

        case MODE_SET_COMMIT:
            if ( asp->status != SNMP_ERR_NOERROR ) {
                asp->status    = SNMP_ERR_COMMITFAILED;
                asp->mode = FINISHED_FAILURE;
            }
            else
                asp->mode = FINISHED_SUCCESS;
            break;

        case MODE_SET_UNDO:
            if (asp->status != SNMP_ERR_NOERROR )
                asp->status = SNMP_ERR_UNDOFAILED;

            asp->mode = FINISHED_FAILURE;
            break;

        case MODE_SET_FREE:
            asp->mode = FINISHED_FAILURE;
            break;
    }
    
    if (asp->mode != FINISHED_SUCCESS && asp->mode != FINISHED_FAILURE) {
        DEBUGMSGTL(("agent_set","doing set mode = %d\n",asp->mode));
        status = handle_var_requests( asp );
        if (status != SNMP_ERR_NOERROR && asp->status == SNMP_ERR_NOERROR)
            asp->status = status;
        DEBUGMSGTL(("agent_set","did set mode = %d, status = %d\n",
                    asp->mode, asp->status));
    }
    return asp->status;
}

int
handle_set_loop(struct agent_snmp_session  *asp) {
    while(asp->mode != FINISHED_FAILURE && asp->mode != FINISHED_SUCCESS) {
        handle_set(asp);
        if (check_for_delegated(asp))
            return SNMP_ERR_NOERROR;
    }
    return asp->status;
}

int
handle_pdu(struct agent_snmp_session  *asp) {
    int status;

    /* collect varbinds */
    status = create_subtree_cache(asp);
    if (status != SNMP_ERR_NOERROR)
        return status;

    asp->mode = asp->pdu->command;
    switch(asp->mode) {
        case SNMP_MSG_GET:
            /* increment the message type counter */
            snmp_increment_statistic(STAT_SNMPINGETREQUESTS);

            /* make sure everything is of type ASN_NULL
            snmp_reset_var_types(asp->pdu->variables, ASN_NULL);
						 */
            /* check vacm ahead of time */
            check_acm(asp, SNMP_NOSUCHOBJECT);
                
            /* get the results */
            status = handle_var_requests(asp);

            /* deal with unhandled results -> noSuchObject */
            if (status == SNMP_ERR_NOERROR)
                snmp_replace_var_types(asp->pdu->variables, ASN_NULL,
                                       SNMP_NOSUCHOBJECT);
            break;

        case SNMP_MSG_GETNEXT:
            /* increment the message type counter */
            snmp_increment_statistic(STAT_SNMPINGETNEXTS);

            /* loop through our mib tree till we find an
               appropriate response to return to the caller. */

            /* make sure everything is of type ASN_NULL
            snmp_reset_var_types(asp->pdu->variables, ASN_NULL);
						*/
            /* first pass */
            status = handle_var_requests(asp);
            if (status != SNMP_ERR_NOERROR) {
                return status; /* should never really happen */
            }

            handle_getnext_loop(asp);
            break;

        case SNMP_MSG_SET:
            /* check access permissions first */
            if (check_acm(asp, SNMP_NOSUCHOBJECT))
                return SNMP_ERR_NOTWRITABLE;

            asp->mode = MODE_SET_BEGIN;
            status = handle_set_loop(asp);
            
            break;

        case SNMP_MSG_GETBULK:
            break;
            /* WWW */

        case SNMP_MSG_RESPONSE:
            snmp_increment_statistic(STAT_SNMPINGETRESPONSES);
            return SNMP_ERR_NOERROR;
            
        case SNMP_MSG_TRAP:
        case SNMP_MSG_TRAP2:
            snmp_increment_statistic(STAT_SNMPINTRAPS);
            return SNMP_ERR_NOERROR;
            
        default:
            /* WWW: are reports counted somewhere ? */
            snmp_increment_statistic(STAT_SNMPINASNPARSEERRS);
            return SNMPERR_GENERR; /* shouldn't get here */
            /* WWW */
    }
    return status;
}

int
handle_var_list(struct agent_snmp_session  *asp)
{
    struct variable_list *varbind_ptr;
    int     status;
    int     count;

    count = 0;
    varbind_ptr = asp->start;
    if ( !varbind_ptr ) {
	return SNMP_ERR_NOERROR;
    }

    while (1) {
	count++;
	asp->index = count;
	status = handle_one_var(asp, varbind_ptr);

	if ( status != SNMP_ERR_NOERROR ) {
	    return status;
	}

	if ( varbind_ptr == asp->end )
	     break;
	varbind_ptr = varbind_ptr->next_variable;
   }

   asp->index = 0;
   return SNMP_ERR_NOERROR;
}

static struct agent_snmp_session *current_agent_session = NULL;
struct agent_snmp_session  *
get_current_agent_session() {
    return current_agent_session;
}

void
set_current_agent_session(struct agent_snmp_session  *asp) {
    current_agent_session = asp;
}

int
handle_one_var(struct agent_snmp_session  *asp, struct variable_list *varbind_ptr)
{
    u_char  statType;
    u_char *statP;
    size_t  statLen;
    u_short acl;
    struct saved_var_data *saved;
    WriteMethod *write_method;
    AddVarMethod *add_method;
    int	    noSuchObject = TRUE;
    int     view;
    oid	    save[MAX_OID_LEN];
    size_t  savelen = 0;
    
statp_loop:
	if ( asp->rw == WRITE && varbind_ptr->data != NULL ) {
		/*
		 * SET handling is "multi-pass", so restore
		 *  results from an earlier 'getStatPtr' call
		 *  to avoid repeating this processing.
		 */
	    saved = (struct saved_var_data *) varbind_ptr->data;
	    write_method = saved->write_method;
	    statP        = saved->statP;
	    statType     = saved->statType;
	    statLen      = saved->statLen;
	    acl          = saved->acl;
	}
	else {
		/*
		 *  For exact requests (i.e. GET/SET),
		 *   check whether this object is accessible
		 *   before actually doing any work
		 */
	    if ( asp->exact )
		view = in_a_view(varbind_ptr->name, &varbind_ptr->name_length,
				   asp->pdu, varbind_ptr->type);
	    else
		view = 0;	/* Assume accessible */
	    
            memcpy(save, varbind_ptr->name,
			varbind_ptr->name_length*sizeof(oid));
            savelen = varbind_ptr->name_length;
	    if ( view == 0 ) {
                struct agent_snmp_session  *oldval = get_current_agent_session();
                set_current_agent_session(asp);
	        statP = getStatPtr(  varbind_ptr->name,
			   &varbind_ptr->name_length,
			   &statType, &statLen, &acl,
			   asp->exact, &write_method, asp->pdu, &noSuchObject);
                set_current_agent_session(oldval);
            }
	    else {
		if (view != 5) send_easy_trap(SNMP_TRAP_AUTHFAIL, 0);
		statP        = NULL;
		write_method = NULL;
	    }
	}
			   
		/*
		 * For a valid SET request, having no existing value
		 *  is (possibly) acceptable (and implies creation).
		 * In all other situations, this indicates failure.
		 */
	if (statP == NULL && (asp->rw != WRITE || write_method == NULL)) {
	        /*  Careful -- if the varbind was lengthy, it will have
		    allocated some memory.  */
	        snmp_set_var_value(varbind_ptr, NULL, 0);
		if ( asp->exact ) {
	            if ( noSuchObject == TRUE ){
		        statType = SNMP_NOSUCHOBJECT;
		    } else {
		        statType = SNMP_NOSUCHINSTANCE;
		    }
		} else {
	            statType = SNMP_ENDOFMIBVIEW;
		}
		if (asp->pdu->version == SNMP_VERSION_1) {
		    return SNMP_ERR_NOSUCHNAME;
		}
		else if (asp->rw == WRITE) {
		    return
			( noSuchObject	? SNMP_ERR_NOTWRITABLE
					: SNMP_ERR_NOCREATION ) ;
		}
		else
		    varbind_ptr->type = statType;
	}
                /*
		 * Delegated variables should be added to the
                 *  relevant outgoing request
		 */
        else if ( IS_DELEGATED(statType)) {
                add_method = (AddVarMethod*)statP;
                return (*add_method)( asp, varbind_ptr );
        }
		/*
		 * GETNEXT/GETBULK should just skip inaccessible entries
		 */
	else if (!asp->exact &&
		 (view = in_a_view(varbind_ptr->name, &varbind_ptr->name_length,
				   asp->pdu, varbind_ptr->type))) {

		if (view != 5) send_easy_trap(SNMP_TRAP_AUTHFAIL, 0);
		goto statp_loop;
	}
#ifdef USING_AGENTX_PROTOCOL_MODULE
		/*
		 * AgentX GETNEXT/GETBULK requests need to take
		 *   account of the end-of-range value
		 *
		 * This doesn't really belong here, but it works!
		 */
	else if (!asp->exact && asp->pdu->version == AGENTX_VERSION_1 &&
		 snmp_oid_compare( varbind_ptr->name,
			  	   varbind_ptr->name_length,
				   varbind_ptr->val.objid,
				   varbind_ptr->val_len/sizeof(oid)) > 0 ) {
            	memcpy(varbind_ptr->name, save, savelen*sizeof(oid));
            	varbind_ptr->name_length = savelen;
		varbind_ptr->type = SNMP_ENDOFMIBVIEW;
	}
#endif
		/*
		 * Other access problems are permanent
		 *   (i.e. writing to non-writeable objects)
		 */
	else if ( asp->rw == WRITE && !((acl & 2) && write_method)) {
	    send_easy_trap(SNMP_TRAP_AUTHFAIL, 0);
	    if (asp->pdu->version == SNMP_VERSION_1 ) {
		return SNMP_ERR_NOSUCHNAME;
	    }
	    else {
		return SNMP_ERR_NOTWRITABLE;
	    }
        }
		
	else {
		/*
		 *  Things appear to have worked (so far)
		 *  Dump the current value of this object
		 */
	    if (ds_get_boolean(DS_APPLICATION_ID, DS_AGENT_VERBOSE) && statP)
	        dump_var(varbind_ptr->name, varbind_ptr->name_length,
				statType, statP, statLen);

		/*
		 *  FINALLY we can act on SET requests ....
		 */
	    if ( asp->rw == WRITE ) {
                    struct agent_snmp_session  *oldval =
                        get_current_agent_session();
		    if ( varbind_ptr->data == NULL ) {
				/*
				 * Save the results from 'getStatPtr'
				 *  to avoid repeating this call
				 */
			saved = (struct saved_var_data *)malloc(sizeof(struct saved_var_data));
			if ( saved == NULL ) {
			    return SNMP_ERR_GENERR;
			}
	    		saved->write_method = write_method;
	    		saved->statP        = statP;
	    		saved->statType     = statType;
	    		saved->statLen      = statLen;
	    		saved->acl          = acl;
			varbind_ptr->data = (void *)saved;
		    }
				/*
				 * Call the object's write method
				 */
                    set_current_agent_session(asp);
		    return (*write_method)(asp->mode,
                                               varbind_ptr->val.string,
                                               varbind_ptr->type,
                                               varbind_ptr->val_len, statP,
                                               varbind_ptr->name,
                                               varbind_ptr->name_length);
                    set_current_agent_session(oldval);
	    }
		/*
		 * ... or save the results from assorted GET requests
		 */
	    else {
		snmp_set_var_value(varbind_ptr, statP, statLen);
		varbind_ptr->type = statType;
	    }
	}

	return SNMP_ERR_NOERROR;
	
}

int set_request_error(agent_request_info *reqinfo, request_info *request,
                       int error_value) {
    switch(error_value) {
        case SNMP_NOSUCHOBJECT:
        case SNMP_NOSUCHINSTANCE:
        case SNMP_ENDOFMIBVIEW:
            /* these are exceptions that should be put in the varbind
               in the case of a GET but should be translated for a SET
               into a real error status code and put in the request */
            switch (reqinfo->mode) {
                case MODE_GET:
                    request->requestvb->type = error_value;
                    return error_value;
                    
                case MODE_GETNEXT:
                case MODE_GETBULK:
                    /* ignore these.  They're illegal to set by the
                       client APIs for these modes */
                    return error_value;

                default:
                    request->status = SNMP_ERR_NOSUCHNAME; /* WWW: correct? */
                    return error_value;
            }
            break; /* never get here */

        default:
            if (request->status < 0) {
                /* illegal local error code.  translate to generr */
                /* WWW: full translation map? */
                request->status = SNMP_ERR_GENERR;
            } else {
                /* WWW: translations and mode checking? */
                request->status = error_value;
            }
            return error_value;
    }
    return error_value;
}

int set_all_requests_error(agent_request_info *reqinfo, request_info *requests,
                            int error_value) {
    while(requests) {
        set_request_error(reqinfo, requests, error_value);
        requests = requests->next;
    }
    return error_value;
}

extern struct timeval starttime;

		/* Return the value of 'sysUpTime' at the given marker */
int
marker_uptime( marker_t pm )
{
    int res;
    marker_t start = (marker_t)&starttime;

    res = atime_diff( start, pm );
    return res/10;      /* atime_diff works in msec, not csec */
}

			/* struct timeval equivalents of these */
int timeval_uptime( struct timeval *tv )
{
    return marker_uptime((marker_t)tv);
}

		/* Return the current value of 'sysUpTime' */
int
get_agent_uptime( void ) {

	struct timeval now;
	gettimeofday(&now, NULL);

	return timeval_uptime( &now );
}
