#include <config.h>

#if HAVE_STDLIB_H
#include <stdlib.h>
#endif
#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#if HAVE_STRING_H
#include <string.h>
#else
#include <strings.h>
#endif
#include <sys/socket.h>
#if HAVE_SYS_SOCKIO_H
#include <sys/sockio.h>
#endif
#if HAVE_NETINET_IN_H
#include <netinet/in.h>
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

#include "asn1.h"
#include "snmp_api.h"
#include "snmp_impl.h"
#include "snmp_client.h"
#include "mib.h"
#include "snmp.h"
#include "party.h"
#include "context.h"
#include "acl.h"
#include "system.h"
#include "read_config.h"
#include "snmp_debug.h"

struct traphandle {
   char *exec;
   oid trap[MAX_OID_LEN];
   int traplen;
   struct traphandle *next;
};

struct traphandle *traphandlers=0;
struct traphandle *defaulthandler=0;

/* handles parsing .conf lines of: */
/*   traphandle OID EXEC           */

char *
snmptrapd_get_traphandler(oid *name, int namelen)
{
  struct traphandle *ttmp;
  DEBUGMSGTL(("snmptrapd:traphandler", "looking for trap handler for "));
  DEBUGMSGOID(("snmptrapd:traphandler", name, namelen));
  DEBUGMSG(("snmptrapd:traphandler", "...\n"));
  for(ttmp = traphandlers;
      ttmp != NULL && snmp_oid_compare(ttmp->trap, ttmp->traplen, name, namelen);
      ttmp = ttmp->next);
  if (ttmp == NULL) {
    if (defaulthandler) {
      DEBUGMSGTL(("snmptrapd:traphandler", "  None found, Using the default handler.\n"));
      return defaulthandler->exec;
    }
    DEBUGMSGTL(("snmptrapd:traphandler", "  Didn't find one.\n"));
    return NULL;
  }
  DEBUGMSGTL(("snmptrapd:traphandler", "  Found it!\n"));
  return ttmp->exec;
}

void
snmptrapd_traphandle(char *token, char *line)
{
  struct traphandle **ttmp;
  char buf[STRINGMAX];
  char *cptr;
  int doingdefault=0;

  /* find the current one, if it exists */
  if (strncmp(line,"default",7) == 0) {
    ttmp = &defaulthandler;
    doingdefault = 1;
  } else {
    for(ttmp = &traphandlers; *ttmp != NULL; ttmp = &((*ttmp)->next));
  }

  if (*ttmp == NULL) {
    /* it doesn't, so allocate a new one. */
    *ttmp = (struct traphandle *) malloc(sizeof(struct traphandle));
    memset(*ttmp, 0, sizeof(struct traphandle));
  } else {
    if ((*ttmp)->exec)
      free((*ttmp)->exec);
  }
  cptr = copy_word(line, buf);
  if (!doingdefault) {
    (*ttmp)->traplen = MAX_OID_LEN;
    if (!read_objid(buf,(*ttmp)->trap, &((*ttmp)->traplen))) {
      sprintf(buf,"Invalid object identifier: %s/%s",buf,line);
      config_perror(buf);
      return;
    }
  }

  (*ttmp)->exec = strdup(cptr);
  DEBUGMSGTL(("read_config:traphandler", "registered handler for: "));
  if (doingdefault) {
    DEBUGMSG(("read_config:traphandler", "default"));
  } else {
    DEBUGMSGOID(("read_config:traphandler", (*ttmp)->trap, (*ttmp)->traplen));
  }
  DEBUGMSG(("read_config:traphandler", "\n"));
}

