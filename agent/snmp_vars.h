/*
 * Definitions for SNMP (RFC 1067) agent variable finder.
 *
 */
/***********************************************************
	Copyright 1988, 1989 by Carnegie Mellon University
	Copyright 1989	TGV, Incorporated

		      All Rights Reserved

Permission to use, copy, modify, and distribute this software and its
documentation for any purpose and without fee is hereby granted,
provided that the above copyright notice appear in all copies and that
both that copyright notice and this permission notice appear in
supporting documentation, and that the name of CMU and TGV not be used
in advertising or publicity pertaining to distribution of the software
without specific, written prior permission.

CMU AND TGV DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
EVENT SHALL CMU OR TGV BE LIABLE FOR ANY SPECIAL, INDIRECT OR
CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF
USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
PERFORMANCE OF THIS SOFTWARE.
******************************************************************/

struct nlist;

extern long long_return;
extern u_char return_buf[];

extern oid nullOid[];
extern int nullOidLen;

#define INST	0xFFFFFFFF	/* used to fill out the instance field of the variables table */

struct variable {
    u_char	    magic;	    /* passed to function as a hint */
    char	    type;	    /* type of variable */
/* See important comment in snmp_vars.c relating to acl */
    u_short	    acl;	    /* access control list for variable */
    u_char	    *(*findVar)__P((struct variable *, oid *, int *, int, int *, int (**write_proc) __P((int, u_char *, u_char, int, u_char *, oid *,int)) ));  /* function that finds variable */
    u_char	    namelen;	    /* length of above */
    oid		    name[32];	    /* object identifier of variable */
};

int subtree_old_size __P((void));
void sort_tree __P((void));
struct subtree *find_subtree __P((oid *, int, struct subtree *));
struct subtree *find_subtree_next __P((oid *, int, struct subtree *));
void register_mib __P((char *, struct variable *, int , int , oid *, int));
void unregister_mib __P((oid *, int));
struct subtree *unregister_mib_tree __P((oid *, int, struct subtree *));
struct subtree *free_subtree __P((struct subtree *));
int in_a_view(oid *, int *, struct snmp_pdu *, int);
