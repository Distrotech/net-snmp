/*******************************
 *
 *	ucd_session.c
 *
 *	Net-SNMP library - UCD compatability interface
 *
 *	Session-handling routines
 *
 *******************************/

#include <config.h>

#if HAVE_STDLIB_H
#include <stdlib.h>
#endif
#if HAVE_STRING_H
#include <string.h>
#else
#include <strings.h>
#endif


#include <net-snmp/var_api.h>
#include <net-snmp/protocol_api.h>
#include <net-snmp/community_api.h>
#include <net-snmp/snmpv3.h>
#include <net-snmp/error.h>
#include <ucd/ucd_api.h>

#include "transport/snmp_transport.h"
#include "session/session.h"
#include "ucd/ucd_convert.h"
#include "snmpv3/snmpv3.h"

#include "snmpv3.h"
#include "tools.h"


int
snmp_sess_select_info(void *void_sess,
                 int *numfds,
		 fd_set *fdset,
		 struct timeval *timeout,
		 int *block);


       /***************
        *
        *  Internal session list handling
        *
        ***************/


struct _ucd_session_list {
   struct _ucd_session_list *next, *prev;
   struct snmp_session      *ucd_sess;
          netsnmp_session   *net_sess;
};
struct _ucd_session_list *Sessions = NULL;


struct _ucd_session_list*
find_link_from_net_session(netsnmp_session *net_sess)
{
    struct _ucd_session_list *sess;

    if ((NULL == net_sess) ||
        (NULL == Sessions)) {
        return NULL;
    }

    for (sess = Sessions; sess; sess = sess->next) {
        if (net_sess == sess->net_sess) {
            return sess;
        }
    }
    return NULL;
}

struct snmp_session*
net_to_ucd_session(netsnmp_session *net_sess)
{
    struct _ucd_session_list *sess;
    sess = find_link_from_net_session(net_sess);

    if (NULL != sess) {
        return sess->ucd_sess;
    }
    return NULL;
}
  

struct _ucd_session_list*
find_link_from_ucd_session(struct snmp_session *ucd_sess)
{
    struct _ucd_session_list *sess;

    if ((NULL == ucd_sess) ||
        (NULL == Sessions)) {
        return NULL;
    }

    for (sess = Sessions; sess; sess = sess->next) {
        if (ucd_sess == sess->ucd_sess) {
            return sess;
        }
    }
    return NULL;
}


netsnmp_session*
ucd_to_net_session(struct snmp_session *ucd_sess)
{
    struct _ucd_session_list *sess;
    sess = find_link_from_ucd_session(ucd_sess);

    if (NULL != sess) {
        return sess->net_sess;
    }
    return NULL;
}


int
ucd_net_session_add_link(struct snmp_session *ucd_sess, netsnmp_session *net_sess)
{
    struct _ucd_session_list *sess;

    if ((NULL == ucd_sess) ||
        (NULL == net_sess)) {
        return -1;
    }

    sess = (struct _ucd_session_list *)calloc(1, sizeof(struct _ucd_session_list));
    if (NULL == sess) {
        return -1;
    }
    sess->ucd_sess = ucd_sess;
    sess->net_sess = net_sess;
    if (Sessions) {
        Sessions->prev = sess;
    }
    sess->next = Sessions;
    Sessions   = sess;
    return 0;
}

int
ucd_net_session_remove_link(struct _ucd_session_list *sess)
{
    if (NULL == sess) {
        return -1;
    }

    if (sess->prev) {
        sess->prev->next = sess->next;
    }
    if (sess->next) {
        sess->next->prev = sess->prev;
    }
    if (sess == Sessions) {
        Sessions = sess->next;
    }

    free(sess);
    return 0;
}


       /***************
        *
        *  UCD-SNMP single-session API compatability routines
        *
        ***************/


struct _ucd_callback {
   snmp_callback ucd_handler;
   void         *ucd_magic;
};

int
UcdCallbackConverter(int operation, netsnmp_session *session, int reqid, void *pdu_ptr, void *magic)
{
    netsnmp_pdu     *net_pdu = (netsnmp_pdu *)pdu_ptr;
    struct _ucd_callback *cb = (struct _ucd_callback *)magic;
    struct snmp_pdu *ucd_pdu;
    struct snmp_session *ucd_sess;
    int ret;

    if ((NULL == cb) ||
        (NULL == cb->ucd_handler)) {
        return -1;	/* Don't know which UCD-style callback to call */
    }

    ucd_pdu  = ucd_revert_pdu(net_pdu);
    ucd_sess = net_to_ucd_session(session);

    ret = cb->ucd_handler(operation, ucd_sess, reqid, ucd_pdu, cb->ucd_magic);
    free( ucd_pdu );		/* XXX ??? */
}


void *
snmp_sess_add(struct snmp_session   *ucd_sess,
                     snmp_transport *transport,
    int (*fpre_parse) (struct snmp_session *, snmp_transport *, void *, int),
    int (*fpost_parse) (struct snmp_session *, struct snmp_pdu *, int))
{
    netsnmp_session  *net_sess;
    struct _ucd_callback *cb;

    NetSnmpCallback *callback = NULL;
    void            *cb_magic = NULL;

    if ((NULL == ucd_sess) ||
        (NULL == transport)) {
        return NULL;
    }

    net_sess = session_new(ucd_sess->version, transport);
    if (NULL == net_sess) {
        return NULL;
    }

    /*
     *  Set up any administrative information provided
     */
    if (0 != ucd_sess->community_len) {
        net_sess->read_community = comminfo_create(ucd_sess->community,
                                                   ucd_sess->community_len);
    }
    net_sess->v3info   = ucd_session_v3info(  ucd_sess, net_sess->v3info);
    net_sess->sm_info  = (void*)ucd_session_userinfo(ucd_sess, net_sess->v3info,
                                                        net_sess->sm_info);

	/* XXX - need to handle {fpre,fpost}_parse hooks */
    /*
     *  ... and set up a callback to invoke the original UCD-style callback routine
     */
    if (NULL != ucd_sess->callback) {
        cb = (struct _ucd_callback *)calloc(1, sizeof(struct _ucd_callback *));
        if (NULL == cb) {
            return NULL;
        }
        cb->ucd_handler = ucd_sess->callback;
        cb->ucd_magic   = ucd_sess->callback_magic;
        callback = UcdCallbackConverter;
        cb_magic = (void *)cb;
    }

    net_sess->hooks = hooks_new(NULL, NULL, NULL, NULL, NULL, callback, cb_magic);
    return (void*)net_sess;
}


void *
snmp_sess_open(struct snmp_session *ucd_sess)
{
    snmp_transport   *transport;
    netsnmp_session  *net_sess;

    if (NULL == ucd_sess) {
        return NULL;
    }

    transport = snmp_tdomain_transport(ucd_sess->peername, ucd_sess->local_port, "udp");
    net_sess = (netsnmp_session*)snmp_sess_add(ucd_sess, transport, NULL, NULL);

    if (NULL == net_sess) {
        snmp_transport_free(transport);
        return NULL;
    }

    return (void*)net_sess;
}


void *
snmp_sess_pointer(struct snmp_session *ucd_sess)
{
    struct _ucd_session_list *sess;
    sess = find_link_from_ucd_session(ucd_sess);

    if (NULL != sess) {
        return (void*)sess->net_sess;
    }
    return NULL;
}


void *
snmp_sess_session(void *void_sess)
{
    struct _ucd_session_list *sess;
    netsnmp_session *net_sess = (netsnmp_session *)void_sess;
    sess = find_link_from_net_session(net_sess);

    if (NULL != sess) {
        return sess->ucd_sess;
    }
    return NULL;
}


int
snmp_sess_close(void *void_sess)
{
    struct _ucd_session_list *sess;
    netsnmp_session *net_sess = (netsnmp_session *)void_sess;
    sess = find_link_from_net_session(net_sess);	/* XXX ??? */

    if (NULL != sess) {
        ucd_net_session_remove_link(sess);
    }

    session_close(net_sess);
    return 1;
}

int
snmp_sess_send(void *void_sess, struct snmp_pdu *ucd_pdu)
{
    netsnmp_session *net_sess = (netsnmp_session *)void_sess;
    netsnmp_pdu *net_pdu;
    int ret;

    if (NULL == net_sess) {
        return 0;
    }

    net_pdu = ucd_convert_pdu(ucd_pdu);
    if (NULL == net_pdu) {
        return 0;
    }

    ret = session_send(net_sess, net_pdu, NULL, NULL);
    if (0 > ret) {	/* XXX - assuming -1 => failure */
        return 0;
    }

	/*
	 * XXX - What's needed in terms of cleaning up ???
	 */
    return ret;
}


/****
	snmp_sess_async_send
 ****/


int
snmp_sess_read(void *void_sess, fd_set *fdset)
{
    netsnmp_session *net_sess = (netsnmp_session *)void_sess;

    if (NULL == net_sess) {
        return -1;
    }
    return session_read(net_sess, fdset);
}


void
snmp_sess_timeout(void *void_sess)
{
    netsnmp_session *net_sess = (netsnmp_session *)void_sess;
    session_timeout(net_sess);
}


int
snmp_sess_select_info(void *void_sess,
                 int *numfds,
		 fd_set *fdset,
		 struct timeval *timeout,
		 int *block)
{
    netsnmp_session *net_sess = (netsnmp_session *)void_sess;
    session_list_select(net_sess, numfds, fdset, timeout, block);
    return 1;		/* XXX - needs to return # active */
}


/*****
	snmp_open_ex()
	snmp_add()
	snmp_sess_add()
	snmp_sess_add_ex()
 *****/



       /***************
        *
        *  UCD-SNMP traditional session API compatability routines
        *
        ***************/


netsnmp_session *Sessions_head = NULL;
netsnmp_session *Sessions_tail = NULL;


struct snmp_session *
snmp_add(struct snmp_session   *session,
                snmp_transport *transport,
    int (*fpre_parse) (struct snmp_session *, snmp_transport *, void *, int),
    int (*fpost_parse) (struct snmp_session *, struct snmp_pdu *, int))
{
    netsnmp_session     *net_sess;
    struct snmp_session *ucd_sess;

    net_sess = (netsnmp_session*)snmp_sess_add(session, transport, fpre_parse, fpost_parse);
    if (NULL == net_sess) {
        return NULL;
    }

    ucd_sess = (struct snmp_session *)calloc(1, sizeof(struct snmp_session));
    if (NULL == ucd_sess) {
        session_free(net_sess);
        return NULL;
    }

    if (0 > ucd_net_session_add_link(ucd_sess, net_sess)) {
        session_free(net_sess);
        free(ucd_sess);
        return NULL;
    }

    if (NULL == Sessions_tail) {
        Sessions_head = net_sess;
        Sessions_tail = net_sess;
    } else {
        Sessions_tail->next = net_sess;
        net_sess->prev = Sessions_tail;
        Sessions_tail = net_sess;
    }
    return ucd_sess;
}


struct snmp_session *snmp_add_full(struct snmp_session *in_session,
                                   struct _snmp_transport *transport,
          int (*fpre_parse) (struct snmp_session *, struct _snmp_transport *, void *, int),
          int (*fparse) (struct snmp_session *, struct snmp_pdu *, u_char *, size_t),
          int (*fpost_parse) (struct snmp_session *, struct snmp_pdu *, int),
          int (*fbuild) (struct snmp_session *, struct snmp_pdu *, u_char *, size_t *),
          int (*frbuild)(struct snmp_session *, struct snmp_pdu *, u_char **, size_t *, size_t *),
          int (*fcheck) (u_char *, size_t),
          struct snmp_pdu * (*fcreate_pdu) (struct _snmp_transport *, void *, size_t))
{
    return snmp_add(in_session, transport, fpre_parse, fpost_parse);
}


struct snmp_session *
snmp_open(struct snmp_session *session)
{
    struct snmp_session     *new_sess;
           snmp_transport   *transport;

    if (NULL == session) {
        return NULL;
    }

    transport = snmp_tdomain_transport(session->peername, session->local_port, "udp");
    new_sess  = snmp_add(session, transport, NULL, NULL);

    if (NULL == new_sess) {
        snmp_transport_free(transport);
        return NULL;
    }

    return new_sess;
}


int 
snmp_close(struct snmp_session *ucd_sess)
{
    struct _ucd_session_list *sess;
    netsnmp_session          *net_sess;
    sess = find_link_from_ucd_session(ucd_sess);

    if (NULL != sess) {
        net_sess = sess->net_sess;
        ucd_net_session_remove_link(sess);
        session_close(net_sess);
        return 1;
    }
    return 0;
}


int
snmp_close_sessions( void )
{
    struct _ucd_session_list *slp;

/*  snmp_res_lock(MT_LIBRARY_ID, MT_LIB_SESSION);	*/
    while ( Sessions ) {
        slp = Sessions;
        Sessions = Sessions->next;
        if (Sessions) {
	    Sessions->prev = NULL;
        }
        session_close(slp->net_sess);
        free(slp);
    }
/*  snmp_res_unlock(MT_LIBRARY_ID, MT_LIB_SESSION);	*/
    return 1;
}


int
snmp_send(struct snmp_session *ucd_sess,
	  struct snmp_pdu     *ucd_pdu)
{
    struct _ucd_session_list *sess;
    netsnmp_pdu              *net_pdu;
    int ret;

    sess = find_link_from_ucd_session(ucd_sess);
    if (NULL == sess) {
        return 0;
    }

    net_pdu = ucd_convert_pdu(ucd_pdu);
    if (NULL == net_pdu) {
        return 0;
    }

    ret = session_send(sess->net_sess, net_pdu, NULL, NULL);
    if (0 > ret) {	/* XXX - assuming -1 => failure */
        return 0;
    }

	/*
	 * XXX - What's needed in terms of cleaning up ???
	 */
    return ret;
}


/****
	snmp_async_send()
 ****/


void
snmp_read(fd_set *fdset)
{
    struct _ucd_session_list *slp;
/*  snmp_res_lock(MT_LIBRARY_ID, MT_LIB_SESSION);	*/
    for(slp = Sessions; slp; slp = slp->next){
        session_read(slp->net_sess, fdset);
    }
/*  snmp_res_unlock(MT_LIBRARY_ID, MT_LIB_SESSION);	*/
}


int
snmp_select_info(int *numfds,
		 fd_set *fdset,
		 struct timeval *timeout,
		 int *block)
{
    return snmp_sess_select_info((void *)Sessions_head, numfds, fdset, timeout, block);
}


void
snmp_timeout (void)
{
    struct _ucd_session_list *slp;
/*  snmp_res_lock(MT_LIBRARY_ID, MT_LIB_SESSION);	*/
    for(slp = Sessions; slp; slp = slp->next) {
	session_timeout(slp->net_sess);
    }
/*  snmp_res_unlock(MT_LIBRARY_ID, MT_LIB_SESSION);	*/
}


#define STAT_SUCCESS 0
#define STAT_TIMEOUT 1
#define STAT_ERROR   2

int
snmp_synch_response(struct snmp_session *ucd_sess,
                    struct snmp_pdu  *ucd_pdu,
                    struct snmp_pdu **ucd_resp)
{
    struct _ucd_session_list *sess;
    netsnmp_pdu              *net_pdu, *net_resp;

    sess = find_link_from_ucd_session(ucd_sess);
    if (NULL == sess) {
        return STAT_ERROR;
    }

    net_pdu = ucd_convert_pdu(ucd_pdu);
    if (NULL == net_pdu) {
        return STAT_ERROR;
    }

    net_resp = synch_response(sess->net_sess, net_pdu);
/*  ret = synch_response(sess->net_sess, net_pdu, &net_resp);	*/
    if (NULL == net_resp) {
        return STAT_ERROR;
    }

    if (NULL != ucd_resp) {
        *ucd_resp = ucd_revert_pdu(net_resp);
    }
    return STAT_SUCCESS;
}

