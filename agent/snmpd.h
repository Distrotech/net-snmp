/*
 * snmpd.h
 */

extern int snmp_dump_packet;
extern int verbose;
extern int (*sd_handlers[])__P((int));

int snmp_read_packet(struct snmp_session *session, snmp_ipaddr from);
extern void init_snmp2p __P((u_short));
extern int open_port __P((u_short));
extern void open_ports_snmp2p __P((void));
extern char *reverse_bytes __P((char *, int));
extern void send_trap_pdu __P((struct snmp_pdu *));
extern void send_easy_trap __P((int, int));
extern u_char *getStatPtr(oid *, int *, u_char *, int *, u_short *, int, int (**write) (int, u_char *, u_char, int, u_char *, oid *, int), struct snmp_pdu *, int * );

/* config file parsing routines */
void snmpd_parse_config_authtrap __P((char *, char *));
void snmpd_parse_config_trapsink __P((char *, char *));
void snmpd_parse_config_trap2sink __P((char *, char *));
void snmpd_free_trapsinks __P((void));
void snmpd_parse_config_trapcommunity __P((char *, char *));
void snmpd_free_trapcommunity __P((void));
void agentBoots_conf __P((char *, char *));

void	init_agent __P((void));

