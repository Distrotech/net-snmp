/*
 *  Interfaces MIB group implementation - interfaces.c
 *
 */

#include "../common_header.h"
#include "interfaces.h"


	/*********************
	 *
	 *  Kernel & interface information,
	 *   and internal forward declarations
	 *
	 *********************/


static struct nlist interfaces_nl[] = {
#define N_IFNET		0
#define N_IN_IFADDR    	1
#if !defined(hpux) && !defined(solaris2)
        { "_ifnet"},
        { "_in_ifaddr"},
#else
        { "ifnet"},
        { "in_ifaddr"},
#endif
        { 0 },
};

int header_interfaces __P((struct variable *, oid *, int *, int, int *, int (**write) __P((int, u_char *, u_char,int, u_char *, oid *, int)) ));
int header_ifEntry __P((struct variable *, oid *, int *, int, int *, int (**write) __P((int, u_char *, u_char,int, u_char *, oid *, int)) ));
extern u_char	*var_ifEntry __P((struct variable *, oid *, int *, int, int *, int (**write) __P((int, u_char *, u_char,int, u_char *, oid *, int)) ));

#if defined(sunV3) || defined(linux)
static int Interface_Scan_By_Index __P((int, char *, struct ifnet *));
#else
static int Interface_Scan_By_Index __P((int, char *, struct ifnet *, struct in_ifaddr *));
#endif
static int Interface_Get_Ether_By_Index __P((int, u_char *));

	/*********************
	 *
	 *  Initialisation & common implementation functions
	 *
	 *********************/


void	init_interfaces( )
{
    init_nlist( interfaces_nl );
}

#define MATCH_FAILED	-1
#define MATCH_SUCCEEDED	0


int
header_interfaces(vp, name, length, exact, var_len, write_method)
    register struct variable *vp;    /* IN - pointer to variable entry that points here */
    oid     *name;	    /* IN/OUT - input name requested, output name found */
    int     *length;	    /* IN/OUT - length of input and output oid's */
    int     exact;	    /* IN - TRUE if an exact match was requested. */
    int     *var_len;	    /* OUT - length of variable or 0 if function returned. */
    int     (**write_method) __P((int, u_char *, u_char, int, u_char *, oid *, int));
{
#define INTERFACES_NAME_LENGTH	8
    oid newname[MAX_NAME_LEN];
    int result;
#ifdef DODEBUG
    char c_oid[1024];

    sprint_objid (c_oid, name, *length);
    printf ("var_interfaces: %s %d\n", c_oid, exact);
#endif

    bcopy((char *)vp->name, (char *)newname, (int)vp->namelen * sizeof(oid));
    newname[INTERFACES_NAME_LENGTH] = 0;
    result = compare(name, *length, newname, (int)vp->namelen + 1);
    if ((exact && (result != 0)) || (!exact && (result >= 0)))
        return MATCH_FAILED;
    bcopy((char *)newname, (char *)name, ((int)vp->namelen + 1) * sizeof(oid));
    *length = vp->namelen + 1;

    *write_method = 0;
    *var_len = sizeof(long);	/* default to 'long' results */
    return MATCH_SUCCEEDED;
};



int
header_ifEntry(vp, name, length, exact, var_len, write_method)
    register struct variable *vp;    /* IN - pointer to variable entry that points here */
    oid     *name;	    /* IN/OUT - input name requested, output name found */
    int     *length;	    /* IN/OUT - length of input and output oid's */
    int     exact;	    /* IN - TRUE if an exact match was requested. */
    int     *var_len;	    /* OUT - length of variable or 0 if function returned. */
    int     (**write_method) __P((int, u_char *, u_char, int, u_char *, oid *, int));
{
#define IFENTRY_NAME_LENGTH	10
    oid newname[MAX_NAME_LEN];
    register int	interface;
    int result, count;
#ifdef DODEBUG
    char c_oid[1024];

    sprint_objid (c_oid, name, *length);
    printf ("var_ifEntry: %s %d\n", c_oid, exact);
#endif

    bcopy((char *)vp->name, (char *)newname, (int)vp->namelen * sizeof(oid));
    /* find "next" interface */
    count = Interface_Scan_Get_Count();
    for(interface = 1; interface <= count; interface++){
	newname[IFENTRY_NAME_LENGTH] = (oid)interface;
	result = compare(name, *length, newname, (int)vp->namelen + 1);
	if ((exact && (result == 0)) || (!exact && (result < 0)))
	    break;
    }
    if (interface > count) {
#ifdef DODEBUG
	printf ("... index out of range\n");
#endif
        return MATCH_FAILED;
    }


    bcopy((char *)newname, (char *)name, ((int)vp->namelen + 1) * sizeof(oid));
    *length = vp->namelen + 1;
    *write_method = 0;
    *var_len = sizeof(long);	/* default to 'long' results */

#ifdef DODEBUG
    sprint_objid (c_oid, name, *length);
    printf ("... get I/F stats %s\n", c_oid);
#endif

    return interface;
};



	/*********************
	 *
	 *  System specific implementation functions
	 *
	 *********************/

u_char	*
var_interfaces(vp, name, length, exact, var_len, write_method)
    register struct variable *vp;
    oid     *name;
    int     *length;
    int     exact;
    int     *var_len;
    int     (**write_method) __P((int, u_char *, u_char, int, u_char *, oid *, int));
{
    if (header_interfaces(vp, name, length, exact, var_len, write_method) == MATCH_FAILED )
	return NULL;

    switch (vp->magic){
	case IFNUMBER:
	    long_return = Interface_Scan_Get_Count();
	    return (u_char *)&long_return;
	default:
	    ERROR("");
    }
    return NULL;
}



#ifndef solaris2
#ifndef hpux

u_char *
var_ifEntry(vp, name, length, exact, var_len, write_method)
    register struct variable *vp;
    register oid	*name;
    register int	*length;
    int			exact;
    int			*var_len;
    int			(**write_method) __P((int, u_char *, u_char, int, u_char *, oid *, int));
{
    static struct ifnet ifnet;
    register int interface;
#ifndef sunV3
    static struct in_ifaddr in_ifaddr;
#endif sunV3
    static char Name[16];
    register char *cp;
#if STRUCT_IFNET_HAS_IF_LASTCHANGE_TV_SEC
          struct timeval now;
#endif

    interface = header_ifEntry(vp, name, length, exact, var_len, write_method);
    if ( interface == MATCH_FAILED )
	return NULL;

#ifdef sunV3
    Interface_Scan_By_Index(interface, Name, &ifnet);   
#else 
    Interface_Scan_By_Index(interface, Name, &ifnet, &in_ifaddr);
#endif


    switch (vp->magic){
	case IFINDEX:
	    long_return = interface;
	    return (u_char *) &long_return;
	case IFDESCR:
#define USE_NAME_AS_DESCRIPTION
#ifdef USE_NAME_AS_DESCRIPTION
	    cp = Name;
#else  USE_NAME_AS_DESCRIPTION
	    cp = Lookup_Device_Annotation(Name, "snmp-descr");
	    if (!cp)
		cp = Lookup_Device_Annotation(Name, 0);
	    if (!cp) cp = Name;
#endif USE_NAME_AS_DESCRIPTION
	    *var_len = strlen(cp);
	    return (u_char *)cp;
	case IFTYPE:
#if 0
	    cp = Lookup_Device_Annotation(Name, "snmp-type");
	    if (cp) long_return = atoi(cp);
	    else
#endif
#if STRUCT_IFNET_HAS_IF_TYPE
		long_return = ifnet.if_type;
#else
		long_return = 1;	/* OTHER */
#endif
	    return (u_char *) &long_return;
	case IFMTU: {
	    long_return = (long) ifnet.if_mtu;
	    return (u_char *) &long_return;
	}
	case IFSPEED:
#if 0
	    cp = Lookup_Device_Annotation(Name, "snmp-speed");
	    if (cp) long_return = atoi(cp);
	    else
#endif
#if STRUCT_IFNET_HAS_IF_BAUDRATE
	    long_return = ifnet.if_baudrate;
#else
	    long_return = (u_long)  1;	/* OTHER */
#endif
#if STRUCT_IFNET_HAS_IF_TYPE
	    if((long_return == 0) || (long_return == 1)) {
		if(ifnet.if_type == IFT_ETHER) long_return=10000000;
		if(ifnet.if_type == IFT_P10) long_return=10000000;
		if(ifnet.if_type == IFT_P80) long_return=80000000;
		if(ifnet.if_type == IFT_ISDNBASIC) long_return=64000; /* EDSS1 only */
		if(ifnet.if_type == IFT_ISDNPRIMARY) long_return=64000*30;
	    }
#endif
	    return (u_char *) &long_return;
	case IFPHYSADDRESS:
#if 0
	    if (Lookup_Device_Annotation(Name, "ethernet-device")) {
		Interface_Get_Ether_By_Index(interface, return_buf);
		*var_len = 6;
		return(u_char *) return_buf;
	    } else {
		long_return = 0;
		return (u_char *) long_return;
	    }
#endif
		Interface_Get_Ether_By_Index(interface, return_buf);
		*var_len = 6;
	        if ((return_buf[0] == 0) && (return_buf[1] == 0) &&
		    (return_buf[2] == 0) && (return_buf[3] == 0) &&
		    (return_buf[4] == 0) && (return_buf[5] == 0))
		    *var_len = 0;
		return(u_char *) return_buf;
	case IFADMINSTATUS:
	    long_return = ifnet.if_flags & IFF_RUNNING ? 1 : 2;
	    return (u_char *) &long_return;
	case IFOPERSTATUS:
	    long_return = ifnet.if_flags & IFF_UP ? 1 : 2;
	    return (u_char *) &long_return;
	case IFLASTCHANGE:
#if defined(STRUCT_IFNET_HAS_IF_LASTCHANGE_TV_SEC) && !defined(freebsd2)
/* XXX - SNMP's ifLastchange is time when op. status changed
 * FreeBSD's if_lastchange is time when packet was input or output
 * (at least in 2.1.0-RELEASE. Changed in later versions of the kernel?)
 */
          if ((ifnet.if_lastchange.tv_sec == 0 ) &&
              (ifnet.if_lastchange.tv_usec == 0))
            long_return = 0;
          else {
            gettimeofday(&now, (struct timezone *)0);
            long_return = (u_long)
              ((now.tv_sec - ifnet.if_lastchange.tv_sec) * 100
               + (now.tv_usec - ifnet.if_lastchange.tv_usec) / 10000);
          }
#else
          long_return = 0; /* XXX */
#endif
          return (u_char *) &long_return;
	case IFINOCTETS:
#ifdef STRUCT_IFNET_HAS_IF_IBYTES
          long_return = (u_long)  ifnet.if_ibytes;
#else
	    long_return = (u_long)  ifnet.if_ipackets * 308; /* XXX */
#endif
	    return (u_char *) &long_return;
	case IFINUCASTPKTS:
	    {
	    long_return = (u_long)  ifnet.if_ipackets;
#if STRUCT_IFNET_HAS_IF_IMCASTS
	    long_return -= (u_long) ifnet.if_imcasts;
#endif
	    }
	    return (u_char *) &long_return;
	case IFINNUCASTPKTS:
#if STRUCT_IFNET_HAS_IF_IMCASTS
	    long_return = (u_long)  ifnet.if_imcasts;
#else
	    long_return = (u_long)  0; /* XXX */
#endif
	    return (u_char *) &long_return;
	case IFINDISCARDS:
#if STRUCT_IFNET_HAS_IF_IQDROPS
	    long_return = (u_long)  ifnet.if_iqdrops;
#else
	    long_return = (u_long)  0; /* XXX */
#endif
	    return (u_char *) &long_return;
	case IFINERRORS:
	    return (u_char *) &ifnet.if_ierrors;
	case IFINUNKNOWNPROTOS:
#if STRUCT_IFNET_HAS_IF_NOPROTO
	    long_return = (u_long)  ifnet.if_noproto;
#else
	    long_return = (u_long)  0; /* XXX */
#endif
	    return (u_char *) &long_return;
	case IFOUTOCTETS:
#ifdef STRUCT_IFNET_HAS_IF_OBYTES
          long_return = (u_long)  ifnet.if_obytes;
#else
	    long_return = (u_long)  ifnet.if_opackets * 308; /* XXX */
#endif
	    return (u_char *) &long_return;
	case IFOUTUCASTPKTS:
	    {
	    long_return = (u_long)  ifnet.if_opackets;
#if STRUCT_IFNET_HAS_IF_OMCASTS
	    long_return -= (u_long) ifnet.if_omcasts;
#endif
	    }
	    return (u_char *) &long_return;
	case IFOUTNUCASTPKTS:
#if STRUCT_IFNET_HAS_IF_OMCASTS
	    long_return = (u_long)  ifnet.if_omcasts;
#else
	    long_return = (u_long)  0; /* XXX */
#endif
	    return (u_char *) &long_return;
	case IFOUTDISCARDS:
	    return (u_char *) &ifnet.if_snd.ifq_drops;
	case IFOUTERRORS:
	    return (u_char *) &ifnet.if_oerrors;
	case IFOUTQLEN:
	    return (u_char *) &ifnet.if_snd.ifq_len;
	case IFSPECIFIC:
	    *var_len = nullOidLen;
	    return (u_char *) &nullOid;
	default:
	    ERROR("");
    }
    return NULL;
}

#else /* hpux */

u_char *
var_ifEntry(vp, name, length, exact, var_len, write_method)
    register struct variable *vp;
    register oid	*name;
    register int	*length;
    int			exact;
    int			*var_len;
    int			(**write_method) __P((int, u_char *, u_char, int, u_char *, oid *, int));
{
    static struct ifnet ifnet;
    register int interface;
    static struct in_ifaddr in_ifaddr;
    static char Name[16];
    register char *cp;
#if STRUCT_IFNET_HAS_IF_LASTCHANGE_TV_SEC
          struct timeval now;
#endif
    struct nmparms hp_nmparms;
    mib_ifEntry hp_ifEntry;
    int  hp_fd;
    int  hp_len=sizeof(hp_ifEntry);


    interface = header_ifEntry(vp, name, length, exact, var_len, write_method);
    if ( interface == MATCH_FAILED )
	return NULL;

    Interface_Scan_By_Index(interface, Name, &ifnet, &in_ifaddr);

	/*
	 * Additional information about the interfaces is available under
	 * HP-UX through the network management interface '/dev/netman'
	 */
#undef		OBJID
	/* Re-instate the HP-UX definition of 'OBJID' */
#define		OBJID(x,y)	((x)<<16) + (y)
    hp_ifEntry.ifIndex = interface;
    hp_nmparms.objid  = ID_ifEntry;
    hp_nmparms.buffer = &hp_ifEntry;
    hp_nmparms.len    = &hp_len;
    if ((hp_fd=open("/dev/netman", O_RDONLY)) != -1 ) {
      if (ioctl(hp_fd, NMIOGET, &hp_nmparms) != -1 ) {
          close(hp_fd);
      }
      else {
          close(hp_fd);
          hp_fd = -1;         /* failed */
      }
    };
#undef		OBJID
#define		OBJID		ASN_OBJECT_ID

    switch (vp->magic){
	case IFINDEX:
	    long_return = interface;
	    return (u_char *) &long_return;
	case IFDESCR:
	  if ( hp_fd != -1 )
	    cp = hp_ifEntry.ifDescr;
	  else
	    cp = Name;
	    *var_len = strlen(cp);
	    return (u_char *)cp;
	case IFTYPE:
	      if ( hp_fd != -1 )
		long_return = hp_ifEntry.ifType;
	      else
		long_return = 1;	/* OTHER */
	    return (u_char *) &long_return;
	case IFMTU: {
	    long_return = (long) ifnet.if_mtu;
	    return (u_char *) &long_return;
	}
	case IFSPEED:
	    if ( hp_fd != -1 )
		long_return = hp_ifEntry.ifSpeed;
	    else
	        long_return = (u_long)  1;	/* OTHER */
	    return (u_char *) &long_return;
	case IFPHYSADDRESS:
		Interface_Get_Ether_By_Index(interface, return_buf);
		*var_len = 6;
	        if ((return_buf[0] == 0) && (return_buf[1] == 0) &&
		    (return_buf[2] == 0) && (return_buf[3] == 0) &&
		    (return_buf[4] == 0) && (return_buf[5] == 0))
		    *var_len = 0;
		return(u_char *) return_buf;
	case IFADMINSTATUS:
	    long_return = ifnet.if_flags & IFF_RUNNING ? 1 : 2;
	    return (u_char *) &long_return;
	case IFOPERSTATUS:
	    long_return = ifnet.if_flags & IFF_UP ? 1 : 2;
	    return (u_char *) &long_return;
	case IFLASTCHANGE:
	  if ( hp_fd != -1 )
	    long_return = hp_ifEntry.ifLastChange;
	  else
          long_return = 0; /* XXX */
          return (u_char *) &long_return;
	case IFINOCTETS:
	  if ( hp_fd != -1 )
	    long_return = hp_ifEntry.ifInOctets;
	  else
	    long_return = (u_long)  ifnet.if_ipackets * 308; /* XXX */
	  return (u_char *) &long_return;
	case IFINUCASTPKTS:
	  if ( hp_fd != -1 )
	    long_return = hp_ifEntry.ifInUcastPkts;
	  else
	    long_return = (u_long)  ifnet.if_ipackets;
	  return (u_char *) &long_return;
	case IFINNUCASTPKTS:
	  if ( hp_fd != -1 )
	    long_return = hp_ifEntry.ifInNUcastPkts;
	  else
	    long_return = (u_long)  0; /* XXX */
	    return (u_char *) &long_return;
	case IFINDISCARDS:
	  if ( hp_fd != -1 )
	    long_return = hp_ifEntry.ifInDiscards;
	  else
	    long_return = (u_long)  0; /* XXX */
	    return (u_char *) &long_return;
	case IFINERRORS:
	    return (u_char *) &ifnet.if_ierrors;
	case IFINUNKNOWNPROTOS:
	  if ( hp_fd != -1 )
	    long_return = hp_ifEntry.ifInUnknownProtos;
	  else
	    long_return = (u_long)  0; /* XXX */
	    return (u_char *) &long_return;
	case IFOUTOCTETS:
	  if ( hp_fd != -1 )
	    long_return = hp_ifEntry.ifOutOctets;
	  else
	    long_return = (u_long)  ifnet.if_opackets * 308; /* XXX */
	    return (u_char *) &long_return;
	case IFOUTUCASTPKTS:
	  if ( hp_fd != -1 )
	    long_return = hp_ifEntry.ifOutUcastPkts;
	  else
	    long_return = (u_long)  ifnet.if_opackets;
	    return (u_char *) &long_return;
	case IFOUTNUCASTPKTS:
	  if ( hp_fd != -1 )
	    long_return = hp_ifEntry.ifOutNUcastPkts;
	  else
	    long_return = (u_long)  0; /* XXX */
	    return (u_char *) &long_return;
	case IFOUTDISCARDS:
	    return (u_char *) &ifnet.if_snd.ifq_drops;
	case IFOUTERRORS:
	    return (u_char *) &ifnet.if_oerrors;
	case IFOUTQLEN:
	    return (u_char *) &ifnet.if_snd.ifq_len;
	case IFSPECIFIC:
	    *var_len = nullOidLen;
	    return (u_char *) &nullOid;
	default:
	    ERROR("");
    }
    return NULL;
}

#endif /* hpux */
#else /* solaris2 */

static int
IF_cmp(void *addr, void *ep)
{
#ifdef DODEBUG
    printf ("... IF_cmp %d %d\n", 
    ((mib2_ifEntry_t *)ep)->ifIndex, ((mib2_ifEntry_t *)addr)->ifIndex);
#endif
    if (((mib2_ifEntry_t *)ep)->ifIndex == ((mib2_ifEntry_t *)addr)->ifIndex)
	return (0);
    else
	return (1);
}

u_char *
var_ifEntry(vp, name, length, exact, var_len, write_method)
    register struct variable *vp;
    register oid        *name;
    register int        *length;
    int                 exact;
    int                 *var_len;
    int                 (**write_method) __P((int, u_char *, u_char, int, u_char *, oid *, int));
{
    int        interface;
    mib2_ifEntry_t      ifstat;


    interface = header_ifEntry(vp, name, length, exact, var_len, write_method);
    if ( interface == MATCH_FAILED )
	return NULL;

    if (getMibstat(MIB_INTERFACES, &ifstat, sizeof(mib2_ifEntry_t),
                   GET_EXACT, &IF_cmp, &interface) != 0) {
#ifdef DODEBUG
      printf ("... no mib stats\n");
#endif
      return NULL;
    }
    switch (vp->magic){
    case IFINDEX:
      long_return = ifstat.ifIndex;
      return (u_char *) &long_return;
    case IFDESCR:
      *var_len = ifstat.ifDescr.o_length;
      (void)memcpy(return_buf, ifstat.ifDescr.o_bytes, *var_len);
      return(u_char *)return_buf;
    case IFTYPE:
      long_return = (u_long)ifstat.ifType;
      return (u_char *) &long_return;
    case IFMTU:
      long_return = (u_long)ifstat.ifMtu;
      return (u_char *) &long_return;
    case IFSPEED:
      long_return = (u_long)ifstat.ifSpeed;
      return (u_char *) &long_return;
    case IFPHYSADDRESS:
      *var_len = ifstat.ifPhysAddress.o_length;
      (void)memcpy(return_buf, ifstat.ifPhysAddress.o_bytes, *var_len);
      return(u_char *)return_buf;
    case IFADMINSTATUS:
      long_return = (u_long)ifstat.ifAdminStatus;
      return (u_char *) &long_return;
    case IFOPERSTATUS:
      long_return = (u_long)ifstat.ifOperStatus;
      return (u_char *) &long_return;
    case IFLASTCHANGE:
      long_return = (u_long)ifstat.ifLastChange;
      return (u_char *) &long_return;
    case IFINOCTETS:
      long_return = (u_long)ifstat.ifInOctets;
      return (u_char *) &long_return;
    case IFINUCASTPKTS:
      long_return = (u_long)ifstat.ifInUcastPkts;
      return (u_char *) &long_return;
    case IFINNUCASTPKTS:
      long_return = (u_long)ifstat.ifInNUcastPkts;
      return (u_char *) &long_return;
    case IFINDISCARDS:
      long_return = (u_long)ifstat.ifInDiscards;
      return (u_char *) &long_return;
    case IFINERRORS:
      long_return = (u_long)ifstat.ifInErrors;
    case IFINUNKNOWNPROTOS:
      long_return = (u_long)ifstat.ifInUnknownProtos;
      return (u_char *) &long_return;
    case IFOUTOCTETS:
      long_return = (u_long)ifstat.ifOutOctets;
      return (u_char *) &long_return;
    case IFOUTUCASTPKTS:
      long_return = (u_long)ifstat.ifOutUcastPkts;
      return (u_char *) &long_return;
    case IFOUTNUCASTPKTS:
      long_return = (u_long)ifstat.ifOutNUcastPkts;
      return (u_char *) &long_return;
    case IFOUTDISCARDS:
      long_return = (u_long)ifstat.ifOutDiscards;
      return (u_char *) &long_return;
    case IFOUTERRORS:
      long_return = (u_long)ifstat.ifOutErrors;
      return (u_char *) &long_return;
     case IFOUTQLEN:
      long_return = (u_long)ifstat.ifOutQLen;
      return (u_char *) &long_return;
    default:
      ERROR("");
    }
    return NULL;
}

#endif /* solaris2 */



	/*********************
	 *
	 *  Internal implementation functions
	 *
	 *********************/


#ifndef solaris2

#ifndef sunV3
static struct in_ifaddr savein_ifaddr;
#endif
static struct ifnet *ifnetaddr, saveifnet, *saveifnetaddr;
static int saveIndex=0;
static char saveName[16];

void
Interface_Scan_Init()
{
    KNLookup (interfaces_nl, N_IFNET, (char *)&ifnetaddr, sizeof(ifnetaddr));
    saveIndex=0;
}



#if defined(sunV3) || defined(linux)
/*
**  4.2 BSD doesn't have ifaddr
**  
*/
int Interface_Scan_Next(Index, Name, Retifnet)
short *Index;
char *Name;
struct ifnet *Retifnet;
{
	struct ifnet ifnet;
	register char *cp;

	while (ifnetaddr) {
	    /*
	     *	    Get the "ifnet" structure and extract the device name
	     */
	    klookup((unsigned long)ifnetaddr, (char *)&ifnet, sizeof ifnet);
	    klookup((unsigned long)ifnet.if_name, (char *)saveName, 16);
	    if (strcmp(saveName, "ip") == 0) {
		ifnetaddr = ifnet.if_next;
		continue;
	    }



 	    saveName[15] = '\0';
	    cp = index(saveName, '\0');
	    string_append_int (cp, ifnet.if_unit);
	    if (1 || strcmp(saveName,"lo0") != 0) {  /* XXX */

		if (Index)
		    *Index = ++saveIndex;
		if (Retifnet)
		    *Retifnet = ifnet;
		if (Name)
		    strcpy(Name, saveName);
		saveifnet = ifnet;
		saveifnetaddr = ifnetaddr;
		ifnetaddr = ifnet.if_next;

		return(1);	/* DONE */
	    } 
	    ifnetaddr = ifnet.if_next;
	}
	return(0);	    /* EOF */
}


#else

#ifdef netbsd1
#define ia_next ia_list.tqe_next
#define if_next if_list.tqe_next
#endif

int Interface_Scan_Next(Index, Name, Retifnet, Retin_ifaddr)
short *Index;
char *Name;
struct ifnet *Retifnet;
struct in_ifaddr *Retin_ifaddr;
{
	struct ifnet ifnet;
	struct in_ifaddr *ia, in_ifaddr;
#if !STRUCT_IFNET_HAS_IF_XNAME
	register char *cp;
#endif

	while (ifnetaddr) {
	    /*
	     *	    Get the "ifnet" structure and extract the device name
	     */
	    klookup((unsigned long)ifnetaddr, (char *)&ifnet, sizeof ifnet);
#if STRUCT_IFNET_HAS_IF_XNAME
	    klookup((unsigned long)ifnet.if_xname, (char *)saveName, 16);
	    saveName[15] = '\0';
#else
	    klookup((unsigned long)ifnet.if_name, (char *)saveName, 16);

	    saveName[15] = '\0';
	    cp = index(saveName, '\0');
	    string_append_int (cp, ifnet.if_unit);
#endif
	    if (1 || strcmp(saveName,"lo0") != 0) {  /* XXX */
		/*
		 *  Try to find an address for this interface
		 */

		KNLookup(interfaces_nl, N_IN_IFADDR, (char *)&ia, sizeof(ia));
		while (ia) {
		    klookup((unsigned long)ia ,  (char *)&in_ifaddr, sizeof(in_ifaddr));
		    if (in_ifaddr.ia_ifp == ifnetaddr) break;
		    ia = in_ifaddr.ia_next;
		}

#if !defined(netbsd1) && !defined(freebsd2)
		ifnet.if_addrlist = (struct ifaddr *)ia;     /* WRONG DATA TYPE; ONLY A FLAG */
#endif
/*		ifnet.if_addrlist = (struct ifaddr *)&ia->ia_ifa;   */  /* WRONG DATA TYPE; ONLY A FLAG */

		if (Index)
		    *Index = ++saveIndex;
		if (Retifnet)
		    *Retifnet = ifnet;
		if (Retin_ifaddr)
		    *Retin_ifaddr = in_ifaddr;
		if (Name)
		    strcpy(Name, saveName);
		saveifnet = ifnet;
		saveifnetaddr = ifnetaddr;
		savein_ifaddr = in_ifaddr;
		ifnetaddr = ifnet.if_next;

		return(1);	/* DONE */
	    }
	    ifnetaddr = ifnet.if_next;
	}
	return(0);	    /* EOF */
}


#endif sunV3




#ifdef sunV3

static int Interface_Scan_By_Index(Index, Name, Retifnet)
int Index;
char *Name;
struct ifnet *Retifnet;
{
        short i;

        Interface_Scan_Init();
        while (Interface_Scan_Next(&i, Name, Retifnet)) {
          if (i == Index) break;
        }
        if (i != Index) return(-1);     /* Error, doesn't exist */
	return(0);	/* DONE */
}

#else

static int Interface_Scan_By_Index(Index, Name, Retifnet, Retin_ifaddr)
int Index;
char *Name;
struct ifnet *Retifnet;
struct in_ifaddr *Retin_ifaddr;
{
	short i;

        Interface_Scan_Init();
        while (Interface_Scan_Next(&i, Name, Retifnet, Retin_ifaddr)) {
          if (i == Index) break;
        }
        if (i != Index) return(-1);     /* Error, doesn't exist */
	return(0);	/* DONE */
}

#endif


static int Interface_Count=0;

int Interface_Scan_Get_Count __P((void))
{

	if (!Interface_Count) {
	    Interface_Scan_Init();
#ifdef sunV3
	    while (Interface_Scan_Next((short *)0, (char *)0, (struct ifnet *)0) != 0) {
#else
	    while (Interface_Scan_Next((short *)0, (char *)0, (struct ifnet *)0, (struct in_ifaddr *)0) != 0) {
#endif
		Interface_Count++;
	    }
	}
	return(Interface_Count);
}


static int Interface_Get_Ether_By_Index(Index, EtherAddr)
int Index;
u_char *EtherAddr;
{
	short i;
	struct arpcom arpcom;

	if (saveIndex != Index) {	/* Optimization! */

	    Interface_Scan_Init();

#ifdef sunV3
	    while (Interface_Scan_Next((short *)&i, (char *)0, (struct ifnet *)0) != 0) {
#else
	    while (Interface_Scan_Next((short *)&i, (char *)0, (struct ifnet *)0, (struct in_ifaddr *)0) != 0) {
#endif
		if (i == Index) break;
	    }
	    if (i != Index) return(-1);     /* Error, doesn't exist */
	}

#ifdef freebsd2
	    if (saveifnet.if_type != IFT_ETHER)
	    {
		bzero(EtherAddr, sizeof(arpcom.ac_enaddr));
		return(0);	/* Not an ethernet if */
	    }
#endif
	/*
	 *  the arpcom structure is an extended ifnet structure which
	 *  contains the ethernet address.
	 */
	klookup((unsigned long)saveifnetaddr, (char *)&arpcom, sizeof (struct arpcom));
	if (strncmp("lo", saveName, 2) == 0) {
	    /*
	     *  Loopback doesn't have a HW addr, so return 00:00:00:00:00:00
	     */
	    bzero(EtherAddr, sizeof(arpcom.ac_enaddr));

	} else {

#if defined(mips) || defined(hpux)
          bcopy((char *) arpcom.ac_enaddr, EtherAddr, sizeof (arpcom.ac_enaddr));
#else
          bcopy((char *) &arpcom.ac_enaddr, EtherAddr, sizeof (arpcom.ac_enaddr));
#endif


	}
	return(0);	/* DONE */
}

#ifdef freebsd2
static struct in_ifaddr *in_ifaddraddr;

Address_Scan_Init()
{
    KNLookup (interface_nl, N_IN_IFADDR, (char *)&in_ifaddraddr, sizeof(in_ifaddraddr));
}

/* NB: Index is the number of the corresponding interface, not of the address */
int Address_Scan_Next(Index, Retin_ifaddr)
short *Index;
struct in_ifaddr *Retin_ifaddr;
{
      struct in_ifaddr in_ifaddr;
      struct ifnet ifnet,*ifnetaddr;  /* NOTA: same name as another one */
      short index=1;

      while (in_ifaddraddr) {
          /*
           *      Get the "in_ifaddr" structure
           */
          klookup(in_ifaddraddr, (char *)&in_ifaddr, sizeof in_ifaddr);
          in_ifaddraddr = in_ifaddr.ia_next;

          if (Retin_ifaddr)
              *Retin_ifaddr = in_ifaddr;

              /*
               * Now, more difficult, find the index of the interface to which
               * this address belongs
               */

              KNLookup (interface_nl, N_IFNET, (char *)&ifnetaddr, sizeof(ifnetaddr));
              while (ifnetaddr && ifnetaddr != in_ifaddr.ia_ifp) {
                      klookup(ifnetaddr, (char *)&ifnet, sizeof ifnet);
                      ifnetaddr = ifnet.if_next;
                      index++;
              }

              /* XXX - might not find it? */

              if (Index)
                  *Index = index;

          return(1);  /* DONE */
      }
      return(0);          /* EOF */
}

#endif

#else /* solaris2 */

int Interface_Scan_Get_Count()
{
	int i, sd;

	if ((sd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
	  return (0);
	if (ioctl(sd, SIOCGIFNUM, &i) == -1) {
	  close(sd);
	  return (0);
	} else {
	  close(sd);
	  return (i);
	}
}

int
Interface_Index_By_Name(Name, Len)
char *Name;
int Len;
{
	int i, sd, ret;
	char buf[1024];
	struct ifconf ifconf;
	struct ifreq *ifrp;

	if (Name == 0)
	  return (0);
	if ((sd = socket(AF_INET, SOCK_DGRAM, 0)) < 0)
	  return (0);
	ifconf.ifc_buf = buf;
	ifconf.ifc_len = 1024;
	if (ioctl(sd, SIOCGIFCONF, &ifconf) == -1) {
	  ret = 0;
	  goto Return;
	}
	for (i = 1, ifrp = ifconf.ifc_req, ret = 0;
	     (char *)ifrp < (char *)ifconf.ifc_buf + ifconf.ifc_len; i++, ifrp++)
	  if (strncmp(Name, ifrp->ifr_name, Len) == 0) {
	    ret = i;
	    break;
	  } else
	    ret = 0;
      Return:
	close(sd);
	return (ret);	/* DONE */
}

#endif /* solaris2 */

