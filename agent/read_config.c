#include <config.h>

#if STDC_HEADERS
#include <string.h>
#include <stdlib.h>
#else
#if HAVE_STDLIB_H
#include <stdlib.h>
#endif
#endif
#include <signal.h>
#include <ctype.h>

#include <sys/time.h>
#if HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#include "m2m.h"
#include "mibincl.h"

/* #include "common_header.h" */

#include "read_config.h"
#include "mib_module_includes.h"
#include "mib_module_config.h"
#include "../snmp_agent.h"
#include "../snmpd.h"

extern struct extensible *passthrus;    /* In pass.c */
extern int numpassthrus;                 /* ditto */

int minimumswap;
char dontReadConfigFiles;
char *optconfigfile;

struct config_line config_handlers[] = {
#include "mibgroup/mib_module_dot_conf.h"
  {"community", snmp_agent_parse_config, NULL},
  {"authtrapenable", snmpd_parse_config_authtrap, NULL},
  {"trapsink", snmpd_parse_config_trapsink, NULL},
  {"trapcommunity", snmpd_parse_config_trapcommunity, NULL}
};

void init_read_config __P((void))
{
  
  update_config(0);
  signal(SIGHUP,update_config);
}

int linecount;
char *curfilename;

int read_config(filename)
     char *filename;
{

  FILE *ifile;
  char line[STRMAX], word[STRMAX], tmpbuf[1024];
  char *cptr;
  int i;

  linecount = 0;
  curfilename = filename;
  
  if ((ifile = fopen(filename,"r")) == NULL) {
    return(1);
  }

  while (fgets(line,STRMAX,ifile) != NULL) 
    {
      linecount++;
      cptr = line;
      /* check blank line or # comment */
      if ((cptr = skip_white(cptr)))
	{
          copy_word(cptr,word);
          cptr = skip_not_white(cptr);
          cptr = skip_white(cptr);
          if (cptr == NULL) {
            sprintf(tmpbuf,"Blank line following %s token.", word);
            config_perror(tmpbuf);
          } else {
            for(i=0; i < sizeof(config_handlers)/sizeof(struct config_line);
                i++) {
              if (!strcasecmp(word,config_handlers[i].config_token)) {
                (*(config_handlers[i].parse_line))(word,cptr);
                i += sizeof(config_handlers);
              }
            }
            if (i < sizeof(config_handlers)) {
              sprintf(tmpbuf,"Unknown token:  %s.", word);
              config_perror(tmpbuf);
            }
          }
	}
    }
  fclose(ifile);
  return(0);
}

void
free_config __P((void))
{
  int i;

  for(i=0; i < sizeof(config_handlers)/sizeof(struct config_line);i++) {
    if (config_handlers[i].free_func != NULL)
      (*(config_handlers[i].free_func))();
  }
}

RETSIGTYPE update_config(a)
int a;
{
  int i;
  char configfile[300];
  struct extensible **etmp, *ptmp;
  char *envconfpath;
  char *cptr1, *cptr2;

  free_config();

  if (!dontReadConfigFiles) {  /* don't read if -C present on command line */
    /* read the config files */
    sprintf(configfile,"%s/snmpd.conf",SNMPLIBPATH);
    read_config (configfile);
    sprintf(configfile,"%s/snmpd.local.conf",SNMPLIBPATH);
    read_config (configfile);

    if ((envconfpath = getenv("SNMPCONFPATH"))) {
      envconfpath = strdup(envconfpath);  /* prevent actually writting in env */
      cptr1 = cptr2 = envconfpath;
      i = 1;
      while (i && *cptr2 != 0) {
        while(*cptr1 != 0 && *cptr1 != ':')
          cptr1++;
        if (*cptr1 == 0)
          i = 0;
        else
          *cptr1 = 0;
        sprintf(configfile,"%s/snmpd.conf",cptr2);
        read_config (configfile);
        sprintf(configfile,"%s/snmpd.local.conf",cptr2);
        read_config (configfile);
        cptr2 = ++cptr1;
      }
      free(envconfpath);
    }
  }
  
  /* read all optional config files */
  /* last is -c from command line */
  /* always read this one even if -C is present (ie both -c and -C) */
  if (optconfigfile != NULL) {
    read_config (optconfigfile);
  }

  /* The order of passthrus is now handled by the 'subtrees' construction */

  setup_tree();
  
  signal(SIGHUP,update_config);
}

void config_perror(string)
  char *string;
{
  fprintf(stderr, "snmpd: %s: line %d: %s\n", curfilename, linecount, string);
}

/* skip all white spaces and return 1 if found something either end of
   line or a comment character */
char *skip_white(ptr)
  char *ptr;
{

  if (ptr == NULL) return (NULL);
  while (*ptr != 0 && isspace(*ptr)) ptr++;
  if (*ptr == 0 || *ptr == '#') return (NULL);
  return (ptr);
}

char *skip_not_white(ptr)
  char *ptr;
{
  
  if (ptr == NULL) return (NULL);
  while (*ptr != 0 && !isspace(*ptr)) ptr++;
  if (*ptr == 0 || *ptr == '#') return (NULL);
  return (ptr);
}

void copy_word(from, to)
     char *from, *to;
{
  while (*from != 0 && !isspace(*from)) *(to++) = *(from++);
  *to = 0;
}


int tree_compare(a, b)
  const void *a, *b;
{
  struct subtree *ap, *bp;
  ap = (struct subtree *) a;
  bp = (struct subtree *) b;

  return compare(ap->name,ap->namelen,bp->name,bp->namelen);
}

int is_parent(name1, len1, name2)
    register oid	    *name1, *name2;
    register int	    len1;
{
    register int len = len1;

    if ( name2 == NULL )
	return 0;	/* Null child - doesn't count */
	
		/*
		 * Is name1 a strict prefix of name2 ?
		 */
    while(len-- > 0){
	if (*name2++ != *name1++)
	    return 0;	/* No */
    }
    return 1;		/* Yes */
}


struct subtree*
insert_in_children_list( eldest, new_tree )
    struct subtree *eldest;	/* The eldest child in the list of potential
					siblings (or ancestors) */
    struct subtree *new_tree;	/* The new subtree to install */
{
    struct subtree *sibling = eldest;
    struct subtree *previous = NULL;

		/* Search for earlier subtrees (including ancestors) */

    while ( (sibling!=NULL) &&
	    tree_compare(sibling, new_tree) <0 ) {
	if ( is_parent(sibling->name, sibling->namelen, new_tree->name) ) {

		/* Found an ancestor of the new subtree, so recurse */
	    struct subtree *result;

	    result = insert_in_children_list( sibling->children, new_tree );
	    if ( result != NULL )
		sibling->children = result;	/* New eldest child */
	    return NULL;
	}
		/* Skip over earlier sibling (or 'uncle') subtrees */
	previous = sibling;
	sibling = sibling->next;
    }


		/* Run out of earlier subtrees */

    if (( sibling == NULL ) ||
	    tree_compare(sibling, new_tree) >0 ) {
	if (( sibling != NULL ) &&
	      is_parent(new_tree->name, new_tree->namelen, sibling->name) ) {
		/* The new subtree is the parent of existing subtrees,
			which need to be cut out of the current sibling list,
			and inserted as children of the new subtree */

	    struct subtree *prev_kid = sibling;
	    struct subtree *next_kid = sibling->next;

	    new_tree->children = sibling;
	
	    while (( next_kid != NULL ) &&
		     is_parent(new_tree->name, new_tree->namelen, next_kid->name) ) {
		prev_kid = next_kid;
		next_kid = next_kid->next;
	    }
	    prev_kid = NULL;
	    sibling = next_kid;		/* I.e. the next sibling of the new subtree */
	}
		
		/* Found the correct place to insert the new tree */
	new_tree->next = sibling;
	if ( previous != NULL ) {
	    previous->next = new_tree;
	    return NULL;
	}
	else {
	    return new_tree;	/* Tell the parent this is a new eldest child */
	}
    }

		/*
		   This point is only reached if the new subtree
			corresponds exactly to an existing subtree.
		   There are three possibilities:
			- Two module implementations are covering
			    exactly the same data  (Not A Good Idea)
			- The same module implemntation has been
			    loaded twice  (so it's safe to ignore it)
			- Two module implementations are cooperating
			    to cover different portions of the subtree.

		  This third is legitmate, so ought to be handled.
		  The probable implementation should move the 'mount_point'
		    sufficiently to differentiate the two, possibly splitting
		    one or both into a collection of smaller subtrees.

		  This idea will be quietly shelved for now, at least until
		    the simple case has been implemented, weighed in the
		    balance and found wanting.....
		*/
   return NULL;
}


void
load_subtree ( new_subtree )
    struct subtree *new_subtree;
{
    extern struct subtree *subtrees;
    insert_in_children_list( subtrees, new_subtree );
}

static struct subtree root_subtrees[] = {
   { { 0 }, 1 },	/* ccitt */
   { { 1 }, 1 },	/*  iso  */
   { { 2 }, 1 }		/* joint-ccitt-iso */
};


void setup_tree __P((void))
{
  extern struct subtree *subtrees,subtrees_old[];
  struct subtree *sb;
  int old_treesz;
  int i;
#if defined(USING_EXTENSIBLE_MODULE) || defined(USING_PASS_MODULE)
  static struct subtree *mysubtree;
  struct extensible *exten;
#ifdef USING_EXTENSIBLE_MODULE
  extern int numrelocs;
  extern struct extensible *relocs;
  extern struct variable2 extensible_relocatable_variables[];
#endif
#ifdef USING_PASS_MODULE
  extern int numpassthrus;
  extern struct extensible *passthrus;
  extern struct variable2 extensible_passthru_variables[];
#endif
#endif
    
  if ( subtrees == NULL ) {
	subtrees =             &(root_subtrees[0]);
	subtrees->next =       &(root_subtrees[1]);
	subtrees->next->next = &(root_subtrees[2]);
  }

  /* Go through the 'static' subtrees (subtrees_old),
	and link them into the global subtree structure */

  for ( i=0 ; i < subtree_old_size(); i++ )
	load_subtree( &(subtrees_old[i]) );


#if USING_EXTENSIBLE_MODULE
  /* add in relocatable mibs */
  for(i=1;i<=numrelocs;i++, sb++) {
    exten = get_exten_instance(relocs,i);
    mysubtree = (struct subtree *)malloc( sizeof(struct subtree ));
    memcpy(mysubtree[0].name,exten->miboid,exten->miblen*sizeof(long));
    mysubtree[0].namelen = exten->miblen;
    mysubtree[0].variables = (struct variable *)extensible_relocatable_variables;
    mysubtree[0].variables_len = 6;
    mysubtree[0].variables_width = sizeof(*extensible_relocatable_variables);
    load_subtree( mysubtree );
  }
#endif
#ifdef USING_PASS_MODULE
  /* add in pass thrus */
  for(i=1;i<=numpassthrus;i++, sb++) {
    exten = get_exten_instance(passthrus,i);
    mysubtree = (struct subtree *)malloc( sizeof(struct subtree ));
    memcpy(mysubtree[0].name,exten->miboid,exten->miblen*sizeof(long));
    mysubtree[0].namelen = exten->miblen;
    mysubtree[0].variables = (struct variable *)extensible_passthru_variables;
    mysubtree[0].variables_len = 1;
    mysubtree[0].variables_width = sizeof(*extensible_passthru_variables);
    load_subtree( mysubtree );
  }
#endif
  
  /* No longer necessary to sort the mib tree - this is inherent in
     the construction of the subtree structure */
}

