/*
 *  System MIB group interface - system.h
 *
 */
#ifndef _MIBGROUP_SYSTEM_H
#define _MIBGROUP_SYSTEM_H

struct variable;
extern void	init_system __P((void));
extern u_char	*var_system __P((struct variable *, oid *, int *, int, int *, int (**write) __P((int, u_char *, u_char, int, u_char *, oid *, int)) ));

#define	VERSION_DESCR		1
#define	VERSIONID		2
#define	UPTIME			3
#define SYSCONTACT		4
#define SYSTEMNAME		5
#define SYSLOCATION		6
#define SYSSERVICES		7


#include "../var_struct.h"


#ifdef IN_SNMP_VARS_C

struct variable2 system_variables[] = {
    {VERSION_DESCR, STRING, RWRITE, var_system, 1, {1}},
    {VERSIONID, OBJID, RONLY, var_system, 1, {2}},
    {UPTIME, TIMETICKS, RONLY, var_system, 1, {3}},
    {SYSCONTACT, STRING, RWRITE, var_system, 1, {4}},
    {SYSTEMNAME, STRING, RWRITE, var_system, 1, {5}},
    {SYSLOCATION, STRING, RWRITE, var_system, 1, {6}},
    {SYSSERVICES, INTEGER, RONLY, var_system, 1, {7}}
};
#define  SYSTEM_SUBTREE  { \
    { MIB, 1}, 7, (struct variable *)system_variables, \
	sizeof(system_variables)/sizeof(*system_variables), \
	sizeof(*system_variables) }
#endif

#endif /* _MIBGROUP_SYSTEM_H */
