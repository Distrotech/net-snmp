#include "mib_module_config.h"        /* list of which modules are supported */
#include <config.h>                   /* local SNMP configuration details*/
#if STDC_HEADERS
#include <stdlib.h>
#include <string.h>
#else 
#if HAVE_STDLIB_H
#include <stdlib.h>
#endif
#endif

#include <sys/types.h>


#include "mibincl.h"                  /* Standard set of SNMP includes*/
#include "util_funcs.h"               /* utility function declarations*/
#include "read_config.h"              /* if the module uses run-time*/
                                        /*      configuration controls*/
#include "auto_nlist.h"               /* if the module needs to read*/
                                       /*      kernel data structures*/
#include "../../../snmplib/system.h"

#include "memory_solaris2.h"                     /* the module-specific header*/

#include <kstat.h>
#include <sys/stat.h>
#include <sys/swap.h>
#include <unistd.h>

int minimumswap;
static char errmsg[300];
/****************************
 * Kstat specific variables *
 ****************************/
kstat_ctl_t *kc;
kstat_t *ksp1, *ksp2;
kstat_named_t *kn, *kn2;

void init_memory_solaris2(void)
{

  struct variable2 extensible_mem_variables[] = {
    {MIBINDEX, ASN_INTEGER, RONLY, var_extensible_mem,1,{MIBINDEX}},
    {ERRORNAME, ASN_OCTET_STR, RONLY, var_extensible_mem, 1, {ERRORNAME }},
    {MEMTOTALSWAP, ASN_INTEGER, RONLY, var_extensible_mem, 1, {MEMTOTALSWAP}},
    {MEMAVAILSWAP, ASN_INTEGER, RONLY, var_extensible_mem, 1, {MEMAVAILSWAP}},
    {MEMTOTALREAL, ASN_INTEGER, RONLY, var_extensible_mem, 1, {MEMTOTALREAL}},
    {MEMAVAILREAL, ASN_INTEGER, RONLY, var_extensible_mem, 1, {MEMAVAILREAL}},
    {MEMTOTALSWAPTXT, ASN_INTEGER, RONLY, var_extensible_mem, 1, {MEMTOTALSWAPTXT}},
    {MEMUSEDSWAPTXT, ASN_INTEGER, RONLY, var_extensible_mem, 1, {MEMUSEDSWAPTXT}},
    {MEMTOTALREALTXT, ASN_INTEGER, RONLY, var_extensible_mem, 1, {MEMTOTALREALTXT}},
    {MEMUSEDREALTXT, ASN_INTEGER, RONLY, var_extensible_mem, 1, {MEMUSEDREALTXT}},
    {MEMTOTALFREE, ASN_INTEGER, RONLY, var_extensible_mem, 1, {MEMTOTALFREE}},
    {MEMSWAPMINIMUM, ASN_INTEGER, RONLY, var_extensible_mem, 1, {MEMSWAPMINIMUM}},
    {MEMSHARED, ASN_INTEGER, RONLY, var_extensible_mem, 1, {MEMSHARED}},
    {MEMBUFFER, ASN_INTEGER, RONLY, var_extensible_mem, 1, {MEMBUFFER}},
    {MEMCACHED, ASN_INTEGER, RONLY, var_extensible_mem, 1, {MEMCACHED}},
    {ERRORFLAG, ASN_INTEGER, RONLY, var_extensible_mem, 1, {ERRORFLAG }},
    {ERRORMSG, ASN_OCTET_STR, RONLY, var_extensible_mem, 1, {ERRORMSG }}
  };

/* Define the OID pointer to the top of the mib tree that we're
   registering underneath */
  oid mem_variables_oid[] = { EXTENSIBLEMIB,MEMMIBNUM };

  /* register ourselves with the agent to handle our mib tree */
  REGISTER_MIB("ucd_snmp/memory", extensible_mem_variables, variable2, \
               mem_variables_oid);

  snmpd_register_config_handler("swap", memory_parse_config,
                                memory_free_config,"min-avail");

  kc = kstat_open();
  if (kc == 0) {
    printf("kstat_open(): failed\n");
  }
}

unsigned char *var_extensible_mem(
    struct variable *vp,
    oid        *name,
    int        *length,
    int        exact,
    int        *var_len,
    WriteMethod **write_method)
{
  static long long_ret;

  /* Initialize the return value to 0 */
  long_ret = 0;

  if (header_generic(vp,name,length,exact,var_len,write_method))
    return(NULL);

  switch (vp->magic) {
    case MIBINDEX:
      long_ret = 0;
      return((u_char *) (&long_ret));
    case ERRORNAME:    /* dummy name */
      sprintf(errmsg,"swap");
      *var_len = strlen(errmsg);
      return((u_char *) (errmsg));
    case MEMTOTALSWAP:
      long_ret = getTotalSwap() * (getpagesize() / 1024);
      return((u_char *) (&long_ret));
    case MEMAVAILSWAP:
      long_ret = getFreeSwap() * (getpagesize() / 1024);
      return((u_char *) (&long_ret));
    case MEMSWAPMINIMUM:
      long_ret = minimumswap;
      return((u_char *) (&long_ret));
    case MEMTOTALREAL:
      ksp1 = kstat_lookup(kc, "unix", 0, "system_pages");
      kstat_read(kc, ksp1, 0);
      kn = kstat_data_lookup(ksp1, "physmem");

      long_ret =  kn->value.ul * (getpagesize() / 1024);
      return((u_char *) (&long_ret));
    case MEMAVAILREAL:
      ksp1 = kstat_lookup(kc, "unix", 0, "system_pages");
      kstat_read(kc, ksp1, 0);
      kn = kstat_data_lookup(ksp1, "freemem");

      long_ret =  kn->value.ul * (getpagesize() / 1024);
      return((u_char *) (&long_ret));
    case MEMTOTALFREE:
      long_ret = getTotalFree() * (getpagesize() / 1024);
      return((u_char *) (&long_ret));

    case ERRORFLAG:
      long_ret = getTotalFree() * (getpagesize() / 1024);
      long_ret = (long_ret > minimumswap)?0:1;
      return((u_char *) (&long_ret));

    case ERRORMSG:
      long_ret = getTotalFree() * (getpagesize() / 1024);
      if ((long_ret > minimumswap)?0:1)
        sprintf(errmsg,"Running out of swap space (%ld)",long_ret);
      else
        errmsg[0] = 0;
      *var_len = strlen(errmsg);
      return((u_char *) (errmsg));
      
  }

  return(NULL);
}

#define DEFAULTMINIMUMSWAP 16000  /* kilobytes */

void memory_parse_config(char *token, char *cptr)
{
  minimumswap = atoi(cptr);
}

void memory_free_config(void) {
  minimumswap = DEFAULTMINIMUMSWAP;
}

long getTotalSwap(void) 
{
  long total_mem;

  int num, i, n;
  swaptbl_t      *s;
  char *strtab;

  total_mem = 0;

  num = swapctl(SC_GETNSWP, 0);
  s = malloc(num * sizeof(swapent_t) + sizeof(struct swaptable));
  strtab = (char *) malloc((num + 1) * MAXSTRSIZE);
  for (i = 0; i < (num + 1); i++) {
    s->swt_ent[i].ste_path = strtab + (i * MAXSTRSIZE);
  }
  s->swt_n = num + 1;
  n = swapctl(SC_LIST, s);

  for (i = 0; i < n; i++)
    total_mem += s->swt_ent[i].ste_pages;

  free (s);
  free (strtab);

  return (total_mem);
}

long getFreeSwap(void)
{
  long free_mem = 0;

  int num, i, n;
  swaptbl_t      *s;
  char *strtab;

  num = swapctl(SC_GETNSWP, 0);
  s = malloc(num * sizeof(swapent_t) + sizeof(struct swaptable));
  strtab = (char *) malloc((num + 1) * MAXSTRSIZE);
  for (i = 0; i < (num + 1); i++) {
    s->swt_ent[i].ste_path = strtab + (i * MAXSTRSIZE);
  }
  s->swt_n = num + 1;
  n = swapctl(SC_LIST, s);

  for (i = 0; i < n; i++)
    free_mem += s->swt_ent[i].ste_free;

  free (s);
  free (strtab);

  return (free_mem);
}

long getTotalFree(void)
{
  long free_mem = 0;

  int num, i, n;
  swaptbl_t      *s;
  char *strtab;

  num = swapctl(SC_GETNSWP, 0);
  s = malloc(num * sizeof(swapent_t) + sizeof(struct swaptable));
  strtab = (char *) malloc((num + 1) * MAXSTRSIZE);
  for (i = 0; i < (num + 1); i++) {
    s->swt_ent[i].ste_path = strtab + (i * MAXSTRSIZE);
  }
  s->swt_n = num + 1;
  n = swapctl(SC_LIST, s);

  for (i = 0; i < n; i++)
    free_mem += s->swt_ent[i].ste_free;

  free (s);
  free (strtab);

  ksp1 = kstat_lookup(kc, "unix", 0, "system_pages");
  kstat_read(kc, ksp1, 0);
  kn = kstat_data_lookup(ksp1, "freemem");

  free_mem += kn->value.ul;


  return (free_mem);
}
