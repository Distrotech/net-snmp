#include <config.h>

#if HAVE_UNISTD_H
#include <unistd.h>
#endif
#if HAVE_FCNTL_H
#include <fcntl.h>
#endif
#include <signal.h>
#include <nlist.h>
#if HAVE_MACHINE_PARAM_H
#include <machine/param.h>
#endif
#if HAVE_SYS_VMMETER_H
#include <sys/vmmeter.h>
#endif
#include <sys/conf.h>
#include <sys/param.h>
#if HAVE_SYS_SWAP_H
#include <sys/swap.h>
#endif
#if HAVE_SYS_FS_H
#include <sys/fs.h>
#elif HAVE_UFS_FS_H
#include <ufs/fs.h>
#elif HAVE_UFS_FFS_FS_H
#include <ufs/ffs/fs.h>
#endif
#if HAVE_MTAB_H
#include <mtab.h>
#endif
#include <sys/stat.h>
#include <errno.h>
#if HAVE_FSTAB_H
#include <fstab.h>
#endif
#if HAVE_SYS_STATVFS_H
#include <sys/statvfs.h>
#endif
#if HAVE_SYS_FIXPOINT_H
#include <sys/fixpoint.h>
#endif
#if STDC_HEADERS
#include <string.h>
#endif

#include "mibincl.h"
#include "mibdefs.h"

RETSIGTYPE update_config();
struct extensible *get_exten_instance();
unsigned char *var_extensible_relocatable();

extern struct myproc *procwatch;         /* moved to proc.c */
extern int numprocs;                     /* ditto */
extern struct exstensible *extens;       /* In exec.c */
extern struct exstensible *relocs;       /* In exec.c */
extern int numextens;                    /* ditto */
extern int numrelocs;                    /* ditto */
extern struct exstensible *passthrus;    /* In pass.c */
extern int numpassthrus;                 /* ditto */

int minimumswap;
double maxload[3];
static int pageshift;           /* log base 2 of the pagesize */
#ifdef SECURITYEXCEPTIONS
  static int exceptions[] = SECURITYEXCEPTIONS;
#endif

int checkmib(vp,name,length,exact,var_len,write_method,newname,max)
    register struct variable *vp;
    register oid	*name;
    register int	*length;
    int			exact;
    int			*var_len;
    int			(**write_method)();
    oid                 *newname;
    int                 max;
{
  int i, rtest;

  for(i=0,rtest=0; i < (int) vp->namelen && i < (int)(*length) && !rtest; i++) {
    if (name[i] != vp->name[i]) {
      if (name[i] < vp->name[i]) 
        rtest = -1;
      else
        rtest = 1;
    }
  }
  if (rtest > 0 || (rtest == 0 && (int) vp->namelen+1 < (int) *length) ||
    (exact == -1 && rtest)) {
    if (var_len)
	*var_len = NULL;
    return NULL;
  }
/*  printf("%d/ck:  vp=%d  ln=%d lst=%d\n",exact,
         vp->namelen,*length,name[*length-1]); */
  if (((int) *length) <= (int) vp->namelen || rtest == -1) {
    bcopy((char *) vp->name, (char *)newname, (int)vp->namelen * sizeof (oid));
    newname[vp->namelen] = 1;
    *length = vp->namelen+1;
  }
  else {
    *length = vp->namelen+1;
    bcopy((char *)name, (char *)newname, (*length) * sizeof(oid));
    if (!exact)
      newname[*length-1] = name[*length-1] + 1;
    else
      newname[*length-1] = name[*length-1];
  }  
  if (max >= 0 && newname[*length-1] > max) {
    if(var_len)
      *var_len = NULL;
    return NULL;
  }
  bcopy((char *)newname, (char *)name, (*length) * sizeof(oid)); 
  if (write_method)
    *write_method = 0;
  if (var_len)
    *var_len = sizeof(long);   /* default */
#ifdef GLOBALSECURITY
  vp->acl =
    ((int) vp->acl & 0x03) |           /* save right most two bits */
    ((int)vp->acl & (GLOBALSECURITY >> 1))      /* and with WRITE bits */
/*    ((((int)(vp->acl & 0x5555) >> 1) & GLOBALSECURITY) << 1) /* ick */
    | GLOBALSECURITY;                           /* include READ bits */
#ifdef SECURITYEXCEPTIONS
  for(i=0; exceptions[i] != -1; i += 2)
    if (vp->magic == exceptions[i])
      vp->acl =
        ((int) vp->acl & 0x03) |                /* save right most two bits */
        ((int)vp->acl & (exceptions[i+1] >> 1)) /* and with WRITE bits */
        | exceptions[i+1];                      /* include READ bits */
#endif
#endif
  return(1);
}

#define pagetok(size) ((size) << pageshift)
#define NL_AVENRUN 0
#define NL_TOTAL 1
#define NL_SWDEVT 2
#define NL_FSWDEVT 3
#define NL_NSWAPFS 4
#define NL_NSWAPDEV 5
#define NL_PHYSMEM 6
#define  KNLookup(nl_which, buf, s)   (klookup((int) nl[nl_which].n_value, buf, s))

static struct nlist nl[] = {
#if !defined(hpux) && !defined(solaris2)
  { "_avenrun"},
  { "_total"},
  { "_swdevt"},
  { "_fswdevt"},
  { "_nswapfs"},
  { "_nswapdev"},
  { "_physmem"},
#else
  { "avenrun"},
  { "total"},
  { "swdevt"},
  { "fswdevt"},
  { "nswapfs"},
  { "nswapdev"},
  { "physmem"},
#endif
  { 0 }
};

#define SWAPGETLEFT 0
#define SWAPGETTOTAL 1

int nswapdev=10;            /* taken from <machine/space.h> */
int nswapfs=10;            /* taken from <machine/space.h> */

#ifdef USEMEMMIB

int
getswap(rettype)
  int rettype;
{

  int spaceleft=0, spacetotal=0, i, fd;

  struct swdevt swdevt[100];
  struct fswdevt fswdevt[100];
  FILE *file;
  struct extensible ex;
  
  if (KNLookup(NL_SWDEVT,(int *) swdevt, sizeof(struct swdevt)*nswapdev)
      == NULL)
    return(0);
  for (i=0; i < nswapdev; i++) {
    if (swdevt[i].sw_enable) {
      spacetotal += swdevt[i].sw_nblks;
      spaceleft += (swdevt[i].sw_nfpgs * 4);
    }
  }
  if (KNLookup(NL_FSWDEVT,(int *) fswdevt, sizeof(struct fswdevt)*nswapfs)
      == NULL)
    return(0);
  for (i=0; i < nswapfs; i++) {
    if (fswdevt[i].fsw_enable) {
      spacetotal += (fswdevt[i].fsw_limit * 2048);  /* 2048=bytes per page? */
      spaceleft += (fswdevt[i].fsw_limit * 2048 -
                    ((fswdevt[i].fsw_allocated - fswdevt[i].fsw_min) * 37));
      /* 37 = calculated value I know it makes no sense, nor is it accurate */
    }
  }
  /* this is a real hack.  I need to get the hold info from swapinfo, but
     I can't figure out how to read it out of the kernel directly
     -- Wes */
  strcpy(ex.command,"/etc/swapinfo -h");
  if (fd = get_exec_output(&ex)) {
    file = fdopen(fd,"r");
    for (i=1;i <= 2 && fgets(ex.output,STRMAX,file) != NULL; i++);
    if (fgets(ex.output,STRMAX,file) != NULL) {
      spaceleft -= atoi(&ex.output[14]);
    }
    fclose(file);
    close(fd);
  } else {
    return(NULL);
  }
  switch
    (rettype) {
    case SWAPGETLEFT:
      return(spaceleft);
    case SWAPGETTOTAL:
      return(spacetotal);
  }
}

unsigned char *
var_extensible_mem(vp, name, length, exact, var_len, write_method)
    register struct variable *vp;	/* IN - pointer to variable entry that points here */
    register oid	*name;		/* IN/OUT - input name requested, output name found */
    register int	*length;	/* IN/OUT - length of input and output oid's */
    int			exact;		/* IN - TRUE if an exact match was requested. */
    int			*var_len;	/* OUT - length of variable or 0 if function returned. */
    int			(**write_method)(); /* OUT - pointer to function to set variable, otherwise 0 */
{

  oid newname[30];
  int count, result,i, rtest=0;
  register int interface;
  struct myproc *proc;
  static long long_ret;
  static char errmsg[300];

  struct vmtotal total;

  if (!checkmib(vp,name,length,exact,var_len,write_method,newname,1))
    return(NULL);
  if (KNLookup(NL_TOTAL, (int *)&total, sizeof(total)) == NULL) {
    return(0);
  }
  switch (vp->magic) {
    case MIBINDEX:
      long_ret = 1;
      return((u_char *) (&long_ret));
    case ERRORNAME:    /* dummy name */
      sprintf(errmsg,"swap");
      *var_len = strlen(errmsg);
      return((u_char *) (errmsg));
    case MEMTOTALSWAP:
      long_ret = getswap(SWAPGETTOTAL);
      return((u_char *) (&long_ret));
    case MEMUSEDSWAP:
      long_ret = getswap(SWAPGETLEFT);
      return((u_char *) (&long_ret));
    case MEMSWAPMINIMUM:
      long_ret = minimumswap;
      return((u_char *) (&long_ret));
    case MEMTOTALREAL:
      /* long_ret = pagetok((int) total.t_rm); */
      if(KNLookup(NL_PHYSMEM,(int *) &result,sizeof(result)) == NULL)
        return(0);
      long_ret = result*1000;
      return((u_char *) (&long_ret));
    case MEMUSEDREAL:
      long_ret = pagetok((int) total.t_arm);
      return((u_char *) (&long_ret));
    case MEMTOTALSWAPTXT:
      long_ret = pagetok(total.t_vmtxt);
      return((u_char *) (&long_ret));
    case MEMUSEDSWAPTXT:
      long_ret = pagetok(total.t_avmtxt);
      return((u_char *) (&long_ret));
    case MEMTOTALREALTXT:
      long_ret = pagetok(total.t_rmtxt);
      return((u_char *) (&long_ret));
    case MEMUSEDREALTXT:
      long_ret = pagetok(total.t_armtxt);
      return((u_char *) (&long_ret));
    case MEMTOTALFREE:
      long_ret = pagetok(total.t_free);
      return((u_char *) (&long_ret));
    case ERRORFLAG:
      long_ret = getswap(SWAPGETLEFT);
      long_ret = (long_ret > minimumswap)?0:1;
      return((u_char *) (&long_ret));
    case ERRORMSG:
      long_ret = getswap(SWAPGETLEFT);
      if ((long_ret > minimumswap)?0:1)
        sprintf(errmsg,"Running out of swap space (%d)",getswap(SWAPGETLEFT));
      else
        errmsg[0] = NULL;
      *var_len = strlen(errmsg);
      return((u_char *) (errmsg));
  }
}

#endif

static int numdisks;
struct diskpart disks[MAXDISKS];

#if defined(USEDISKMIB)

unsigned char *
var_extensible_disk(vp, name, length, exact, var_len, write_method)
    register struct variable *vp;
/* IN - pointer to variable entry that points here */
    register oid	*name;
/* IN/OUT - input name requested, output name found */
    register int	*length;
/* IN/OUT - length of input and output oid's */
    int			exact;
/* IN - TRUE if an exact match was requested. */
    int			*var_len;
/* OUT - length of variable or 0 if function returned. */
    int			(**write_method)();
/* OUT - pointer to function to set variable, otherwise 0 */
{

  oid newname[30];
  int count, result,i, rtest=0, disknum=0;
  int totalblks, free, used, avail, availblks;
  register int interface;
  struct myproc *proc;
  static long long_ret;
  static char errmsg[300];

#if HAVE_SYS_STATVFS_H
  struct statvfs vfs;
#else
#if HAVE_FSTAB_H
  int file;
  union {
     struct fs iu_fs;
     char dummy[SBSIZE];
  } sb;
#define filesys sb.iu_fs
#endif
#endif
  
  if (!checkmib(vp,name,length,exact,var_len,write_method,newname,numdisks))
    return(NULL);
  disknum = newname[*length - 1] - 1;
  switch (vp->magic) {
    case MIBINDEX:
      long_ret = disknum;
      return((u_char *) (&long_ret));
    case ERRORNAME:       /* DISKPATH */
      *var_len = strlen(disks[disknum].path);
      return((u_char *) disks[disknum].path);
    case DISKDEVICE:
      *var_len = strlen(disks[disknum].device);
      return((u_char *) disks[disknum].device);
    case DISKMINIMUM:
      long_ret = disks[disknum].minimumspace;
      return((u_char *) (&long_ret));
  }
#if HAVE_SYS_STATVFS_H
  if (statvfs (disks[disknum].path, &vfs) == -1) {
    fprintf(stderr,"Couldn't open device %s\n",disks[disknum].device);
    setPerrorstatus("statvfs dev/disk");
    return NULL;
  }
  switch (vp->magic) {
    case DISKTOTAL:
      long_ret = vfs.f_blocks;
      return((u_char *) (&long_ret));
    case DISKAVAIL:
      long_ret = vfs.f_bavail;
      return((u_char *) (&long_ret));
    case DISKUSED:
      long_ret = vfs.f_blocks - vfs.f_bfree;
      return((u_char *) (&long_ret));
    case DISKPERCENT:
      long_ret = (int) (vfs.f_bavail <= 0 ? 100 :
                        ((double) (vfs.f_blocks - vfs.f_bavail) / (double) vfs.f_blocks) * 100);
      return ((u_char *) (&long_ret));
    case ERRORFLAG:
      long_ret = (vfs.f_bavail < disks[disknum].minimumspace)
        ? 1 : 0;
      return((u_char *) (&long_ret));
    case ERRORMSG:
      if (vfs.f_bavail < disks[disknum].minimumspace) 
        sprintf(errmsg,"%s: under %d left (= %d)",disks[disknum].path,
                disks[disknum].minimumspace, vfs.f_bavail);
      else
        errmsg[0] = NULL;
      *var_len = strlen(errmsg);
      return((u_char *) (errmsg));
  }
#else
#if HAVE_FSTAB_H
  /* read the disk information */
  if ((file = open(disks[disknum].device,0)) < 0) {
    fprintf(stderr,"Couldn't open device %s\n",disks[disknum].device);
    setPerrorstatus("open dev/disk");
    return(NULL);
  }
  lseek(file, (long) (SBLOCK * DEV_BSIZE), 0);
  if (read(file,(char *) &filesys, SBSIZE) != SBSIZE) {
    setPerrorstatus("open dev/disk");
    fprintf(stderr,"Error reading device %s\n",disks[disknum].device);
    close(file);
    return(NULL);
  }
  close(file);
  totalblks = filesys.fs_dsize;
  free = filesys.fs_cstotal.cs_nbfree * filesys.fs_frag +
    filesys.fs_cstotal.cs_nffree;
  used = totalblks - free;
  availblks = totalblks * (100 - filesys.fs_minfree) / 100;
  avail = availblks > used ? availblks - used : 0;
  switch (vp->magic) {
    case DISKTOTAL:
      long_ret = (totalblks * filesys.fs_fsize / 1024);
      return((u_char *) (&long_ret));
    case DISKAVAIL:
      long_ret = avail * filesys.fs_fsize/1024;
      return((u_char *) (&long_ret));
    case DISKUSED:
      long_ret = used * filesys.fs_fsize/1024;
      return((u_char *) (&long_ret));
    case DISKPERCENT:
      long_ret = (int) (availblks == 0 ? 0 :
                        ((double) used / (double) availblks) * 100);
      return ((u_char *) (&long_ret));
    case ERRORFLAG:
      long_ret = (avail * filesys.fs_fsize/1024 < disks[disknum].minimumspace)
        ? 1 : 0;
      return((u_char *) (&long_ret));
    case ERRORMSG:
      if (avail * filesys.fs_fsize/1024 < disks[disknum].minimumspace) 
        sprintf(errmsg,"%s: under %d left (= %d)",disks[disknum].path,
                disks[disknum].minimumspace, avail * filesys.fs_fsize/1024);
      else
        errmsg[0] = NULL;
      *var_len = strlen(errmsg);
      return((u_char *) (errmsg));
  }
#endif
#endif
}

/*#endif*/
#endif

#define NOERR 0
#define LOCKDBROKE 1
#define OPENERR 2

/*
static int
locktimeouttest;

int lockd_timeout()
{
  locktimeouttest=1;
}
*/

#ifdef USELOCKDMIB
long
lockd_test(msg)
  char *msg;
{

  int file, ret;
  
/*  signal(SIGALRM,update_config); */
  
  file = open(LOCKDNFSFILE,O_RDONLY);
  if (file < 0) {
    /* needs to be created */
    file = open(LOCKDNFSFILE,O_WRONLY|O_CREAT,0644);
    if (file < 0) {
      sprintf(msg,"open() - %s\terrno:%d",LOCKDNFSFILE,errno);
      return (OPENERR);
    }
  }
  ret = lockf(file,F_TLOCK,0);
  if (ret) {
    sprintf(msg,"Lockd not functioning:  couldn't lock %s",LOCKDNFSFILE);
    close(file);
/*    unlink(LOCKDREALFILE); */
    return(LOCKDBROKE);
  }
  ret = lockf(file,F_ULOCK,0);
  if (ret) {
    sprintf(msg,"Lockd not functioning:  couldn't unlock %s",LOCKDNFSFILE);
    close(file);
/*    unlink(LOCKDREALFILE);*/
    return(LOCKDBROKE);
  }
  close(file);
  /* remove the real file, not the nfs file */
  /* unlink(LOCKDREALFILE); */
  return (NOERR);
}


unsigned char *
var_extensible_lockd_test(vp, name, length, exact, var_len, write_method)
    register struct variable *vp;
/* IN - pointer to variable entry that points here */
    register oid	*name;
/* IN/OUT - input name requested, output name found */
    register int	*length;
/* IN/OUT - length of input and output oid's */
    int			exact;
/* IN - TRUE if an exact match was requested. */
    int			*var_len;
/* OUT - length of variable or 0 if function returned. */
    int			(**write_method)();
/* OUT - pointer to function to set variable, otherwise 0 */
{

  oid newname[30];
  int count, result,i, rtest=0;
  register int interface;
  struct myproc *proc;
  static long long_ret;
  static char errmsg[300];


  if (!checkmib(vp,name,length,exact,var_len,write_method,newname,1))
    return(NULL);

  errmsg[0] = NULL;
  
  switch (vp->magic) {
    case MIBINDEX:
      long_ret = newname[*length - 1];
      return((u_char *) (&long_ret));
    case ERRORFLAG:
      long_ret = lockd_test(errmsg);
      return((u_char *) (&long_ret));
    case ERRORMSG:
      long_ret = lockd_test(errmsg);
      *var_len = strlen(errmsg);
      return((u_char *) errmsg);
  }
  return NULL;
}

#endif
#ifdef USELOADAVEMIB

unsigned char *
var_extensible_loadave(vp, name, length, exact, var_len, write_method)
    register struct variable *vp;	/* IN - pointer to variable entry that points here */
    register oid	*name;		/* IN/OUT - input name requested, output name found */
    register int	*length;	/* IN/OUT - length of input and output oid's */
    int			exact;		/* IN - TRUE if an exact match was requested. */
    int			*var_len;	/* OUT - length of variable or 0 if function returned. */
    int			(**write_method)(); /* OUT - pointer to function to set variable, otherwise 0 */
{

  oid newname[30];
  int count, result,i, rtest=0;
  register int interface;
  struct extensible *exten;
  static long long_ret;
  static char errmsg[300];
#ifdef HAVE_SYS_FIXPOINT_H
  fix favenrun[3];
#endif
#if defined(sun) || defined(__alpha)
  long favenrun[3];
#define FIX_TO_DBL(_IN) (((double) _IN)/((double) FSCALE))
#endif
  double avenrun[3];
  oid loadave[3];
  
  if (!checkmib(vp,name,length,exact,var_len,write_method,newname,3))
    return(NULL);

  switch (vp->magic) {
    case MIBINDEX:
      long_ret = newname[*length-1];
      return((u_char *) (&long_ret));
    case ERRORNAME:
      sprintf(errmsg,"Load-%d",((newname[*length-1] == 1) ? 1 :
                                ((newname[*length-1] == 2) ? 5 : 15)));
      *var_len = strlen(errmsg);
      return((u_char *) (errmsg));
  }
#if defined(ultrix) || defined(sun) || defined(__alpha)
  if (KNLookup(NL_AVENRUN,(int *) favenrun, sizeof(favenrun)) == NULL)
    return(0);
  for(i=0;i<3;i++)
    avenrun[i] = FIX_TO_DBL(favenrun[i]);
#else
#ifdef freebsd2
  if (getloadavg(avenrun, sizeof(avenrun) / sizeof(avenrun[0])) == -1)
      return(0);
#else
  if (KNLookup(NL_AVENRUN,(int *) avenrun, sizeof(double)*3) == NULL)
    return(0);
#endif
#endif
  switch (vp->magic) {
    case LOADAVE:
      sprintf(errmsg,"%.2f",avenrun[newname[*length-1]-1]);
      *var_len = strlen(errmsg);
      return((u_char *) (errmsg));
    case LOADMAXVAL:
      sprintf(errmsg,"%.2f",maxload[newname[*length-1]-1]);
      *var_len = strlen(errmsg);
      return((u_char *) (errmsg));
    case ERRORFLAG:
      long_ret = (maxload[newname[*length-1]-1] != 0 &&
                  avenrun[newname[*length-1]-1] >= maxload[newname[*length-1]-1]) ? 1 : 0;
      return((u_char *) (&long_ret));
    case ERRORMSG:
      if (maxload[newname[*length-1]-1] != 0 &&
          avenrun[newname[*length-1]-1] >= maxload[newname[*length-1]-1]) {
        sprintf(errmsg,"%d min Load Average too high (= %.2f)",
                (newname[*length-1] == 1)?1:((newname[*length-1] == 2)?5:15),
                avenrun[newname[*length-1]-1]);
      } else {
        errmsg[0] = NULL;
      }
      *var_len = strlen(errmsg);
      return((u_char *) errmsg);
  }
}

#endif

static time_t errorstatustime=0;
static int errorstatusprior=0;
static char errorstring[STRMAX];

setPerrorstatus(to)
  char *to;
{
  char buf[STRMAX];
#if !defined(netbsd1) && !defined(freebsd2)
  extern char *sys_errlist[];
#endif
  extern int errno;
  
  sprintf(buf,"%s:  %s",to,sys_errlist[errno]);
  perror(to);
  seterrorstatus(buf,5);
}

seterrorstatus(to,prior)
  char *to;
  int prior;
{
  if (errorstatusprior <= prior ||
      (ERRORTIMELENGTH < (time(NULL) - errorstatustime))) {
    strcpy(errorstring,to);
    errorstatusprior = prior;
    errorstatustime = time(NULL);
  }
}
  
#ifdef USEERRORMIB

unsigned char *
var_extensible_errors(vp, name, length, exact, var_len, write_method)
    register struct variable *vp;
/* IN - pointer to variable entry that points here */
    register oid	*name;
/* IN/OUT - input name requested, output name found */
    register int	*length;
/* IN/OUT - length of input and output oid's */
    int			exact;
/* IN - TRUE if an exact match was requested. */
    int			*var_len;
/* OUT - length of variable or 0 if function returned. */
    int			(**write_method)();
/* OUT - pointer to function to set variable, otherwise 0 */
{

  oid newname[30];
  static long long_ret;
  static char errmsg[300];


  if (!checkmib(vp,name,length,exact,var_len,write_method,newname,1))
    return(NULL);

  errmsg[0] = NULL;
  
  switch (vp->magic) {
    case MIBINDEX:
      long_ret = newname[*length - 1];
      return((u_char *) (&long_ret));
    case ERRORNAME:
      strcpy(errmsg,"snmp");
      *var_len = strlen(errmsg);
      return((u_char *) errmsg);
    case ERRORFLAG:
      long_ret = (ERRORTIMELENGTH >= time(NULL)-errorstatustime) ? 1 : 0;
      return((u_char *) (&long_ret));
    case ERRORMSG:
      if ((ERRORTIMELENGTH >= time(NULL)-errorstatustime) ? 1 : 0) 
        strcpy(errmsg,errorstring);
      else
        errmsg[0] = NULL;
      *var_len = strlen(errmsg);
      return((u_char *) errmsg);
  }
  return NULL;
}

#endif


extern char version_descr[];
extern char sysName[];
extern struct subtree *subtrees,subtrees_old[];
extern struct variable2 extensible_relocatable_variables[];
extern struct variable2 extensible_passthru_variables[];

extern int compare();

int tree_compare(a, b)
  void *a, *b;
{
  struct subtree *ap, *bp;
  ap = (struct subtree *) a;
  bp = (struct subtree *) b;

  return compare(ap->name,ap->namelen,bp->name,bp->namelen);
}

void setup_tree()
{
  extern struct subtree *subtrees,subtrees_old[];
  extern struct variable2 extensible_relocatable_variables[];
  extern struct variable2 extensible_passthru_variables[];
  struct subtree *sb;
  int i, old_treesz;
  static struct subtree mysubtree[1];
  struct extensible *exten;
  
  /* Malloc new space at the end of the mib tree for the new
     extensible mibs and add them in. */

  old_treesz = subtree_old_size();

  subtrees = (struct subtree *) malloc ((numrelocs + old_treesz + numpassthrus)
                                        *sizeof(struct subtree));
  bcopy(subtrees_old,subtrees,old_treesz *sizeof(struct subtree));
  sb = subtrees;
  sb += old_treesz;

  /* add in relocatable mibs */
  for(i=1;i<=numrelocs;i++, sb++) {
    exten = get_exten_instance(relocs,i);
    memcpy(mysubtree[0].name,exten->miboid,exten->miblen*sizeof(long));
    mysubtree[0].namelen = exten->miblen;
    mysubtree[0].variables = (struct variable *)extensible_relocatable_variables;
    mysubtree[0].variables_len = 6;
    mysubtree[0].variables_width = sizeof(*extensible_relocatable_variables);
    memcpy(sb,mysubtree,sizeof(struct subtree));
  }

  /* add in pass thrus */
  for(i=1;i<=numpassthrus;i++, sb++) {
    exten = get_exten_instance(passthrus,i);
    memcpy(mysubtree[0].name,exten->miboid,exten->miblen*sizeof(long));
    mysubtree[0].namelen = exten->miblen;
    mysubtree[0].variables = (struct variable *)extensible_passthru_variables;
    mysubtree[0].variables_len = 1;
    mysubtree[0].variables_width = sizeof(*extensible_passthru_variables);
    memcpy(sb,mysubtree,sizeof(struct subtree));
  }

  /* Here we sort the mib tree so it can insert new extensible mibs
     and also double check that our mibs were in the proper order in
     the first place */

  qsort(subtrees,numrelocs + old_treesz + numpassthrus,
        sizeof(struct subtree),tree_compare);

}

RETSIGTYPE update_config(a)
int a;
{
  extern struct subtree *subtrees;
  int i;
  char configfile[300];
  
  free_config(&procwatch,&extens,&relocs,&passthrus);
  numprocs = numextens = numrelocs = numpassthrus = 0;
  /* restore defaults */
  minimumswap = DEFAULTMINIMUMSWAP;
  for (i=0; i<=2;i++)
    maxload[i] = DEFMAXLOADAVE;
  numdisks = 0;
  for(i=0;i<MAXDISKS;i++) {           /* init/erase disk db */
    disks[i].device[0] = NULL;
    disks[i].path[0] = NULL;
    disks[i].minimumspace = -1;
  }
  /* read the config files */
  sprintf(configfile,"%s/snmpd.conf",SNMPLIBPATH);
  read_config (configfile,&procwatch,&numprocs,&relocs,&numrelocs,&passthrus,&numpassthrus,&extens,&numextens,&minimumswap,disks,&numdisks,maxload);
  sprintf(configfile,"%s/snmpd.local.conf",SNMPLIBPATH);
  read_config (configfile,&procwatch,&numprocs,&relocs,&numrelocs,&passthrus,&numpassthrus,&extens,&numextens,&minimumswap,disks,&numdisks,maxload);

  if (subtrees)
    free(subtrees);
  setup_tree();
  
  signal(SIGHUP,update_config);
}


init_extensible() {
  
  struct extensible extmp;
  int ret,pagesize,i;
  char configfile[300];

  minimumswap = DEFAULTMINIMUMSWAP;
  for (i=0; i<=2;i++)
    maxload[i] = DEFMAXLOADAVE;
  numdisks = 0;
  for(i=0;i<MAXDISKS;i++) {           /* init/erase disk db */
    disks[i].device[0] = NULL;
    disks[i].path[0] = NULL;
    disks[i].minimumspace = -1;
  }

  procwatch = NULL;   /* initialize to NULL */
  extens = NULL;
  relocs = NULL;
  passthrus = NULL;

  /* read config file(s) */
  /* read the config files */
  sprintf(configfile,"%s/snmpd.conf",SNMPLIBPATH);
  read_config (configfile,&procwatch,&numprocs,&relocs,&numrelocs,&passthrus,&numpassthrus,&extens,&numextens,&minimumswap,disks,&numdisks,maxload);
  sprintf(configfile,"%s/snmpd.local.conf",SNMPLIBPATH);
  read_config (configfile,&procwatch,&numprocs,&relocs,&numrelocs,&passthrus,&numpassthrus,&extens,&numextens,&minimumswap,disks,&numdisks,maxload);
  
  /* set default values of system stuff */
  strcpy(extmp.command,"/bin/uname -a");
  /* setup defaults */
  extmp.type = EXECPROC;
  extmp.next = NULL;
  exec_command(&extmp);
  strcpy(version_descr,extmp.output);

  strcpy(extmp.command,"/bin/uname -n");
  /* setup defaults */
  extmp.type = EXECPROC;
  extmp.next = NULL;
  exec_command(&extmp);
  strcpy(sysName,extmp.output);

  signal(SIGHUP,update_config);

  /* nlist stuff */

  if ((ret = nlist(KERNEL_LOC,nl)) == -1) {
    ERROR("nlist");
    exit(1);
  }
  for(ret = 0; nl[ret].n_name != NULL; ret++) {
    if (nl[ret].n_type == 0) {
      DEBUGP1("nlist err:  %s not found\n", nl[ret].n_name);
    }
  }

#ifdef USEMEMMIB
  if (KNLookup(NL_NSWAPDEV,(int *) &nswapdev, sizeof(nswapdev))
      == NULL)
    return(0);
  if (KNLookup(NL_NSWAPFS,(int *) &nswapfs, sizeof(nswapfs))
      == NULL)
    return(0);

  pagesize = 1 << PGSHIFT;
  pageshift = 0;
  while (pagesize > 1) {
    pageshift++;
    pagesize >>= 1;
  }
  pageshift -= 10;
#endif

  setup_tree();
}

