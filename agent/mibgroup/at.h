/*
 *  Template MIB group interface - at.h
 *
 */

#ifndef _MIBGROUP_AT_H
#define _MIBGROUP_AT_H

extern void	init_at __P((void));
extern u_char	*var_atEntry __P((struct variable *, oid *, int *, int, int *, int (**write) __P((int, u_char *, u_char, int, u_char *, oid *, int)) ));


#define ATIFINDEX	0
#define ATPHYSADDRESS	1
#define ATNETADDRESS	2

#include "ip.h"		/* for ipNetToMedia table */

#ifdef IN_SNMP_VARS_C

  /* variable4 because var_atEntry is also used by ipNetToMediaTable */
struct variable4 at_variables[] = {
    {ATIFINDEX, INTEGER, RONLY, var_atEntry, 1, {1}},
    {ATPHYSADDRESS, STRING, RONLY, var_atEntry, 1, {2}},
    {ATNETADDRESS, IPADDRESS, RONLY, var_atEntry, 1, {3}}
};
#define  AT_SUBTREE  { \
    {MIB, 3, 1, 1}, 9, (struct variable *)at_variables, \
	 sizeof(at_variables)/sizeof(*at_variables), \
	 sizeof(*at_variables) }
#endif

#endif /* _MIBGROUP_AT_H */
