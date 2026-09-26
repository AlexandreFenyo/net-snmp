
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>
#include <unistd.h>

extern int alex_main(int argc, char *argv[]);

extern void usage(void);

// Must be in sync with SNMPManager.swift
#define ALEX_AV_TAB_LEN 32
#define ALEX_AV_STR_LEN 1024

char alex_av_tab[ALEX_AV_TAB_LEN][ALEX_AV_STR_LEN];
char *alex_av[ALEX_AV_TAB_LEN];
int av_count = 0;

void alex_set_av(int idx, char *str) {
    strncpy(alex_av_tab[idx], str, ALEX_AV_STR_LEN - 1);
    alex_av_tab[idx][ALEX_AV_STR_LEN - 1] = 0;
}

void alex_set_av_count(int count) {
    av_count = count;
}

void alex_walk(void) {
    for (int i = 0; i < ALEX_AV_TAB_LEN; i++) {
        alex_av[i] = alex_av_tab[i];
        // printf("arg[%d]=%s\n", i, alex_av[i]);
    }

    alex_main(av_count, alex_av);
}

// Anneau de résultats entre le thread C du walk (producteur : alex_main -> push) et le
// thread Swift (consommateur : SNMPManager.walk -> poplength/pop, et isempty depuis
// getState). Un seul producteur, un seul consommateur : pas de verrou, mais les deux index
// sont atomiques, publiés en release et lus en acquire, pour qu'une entrée soit toujours
// entièrement écrite (malloc + copie) avant que l'autre thread voie l'index qui la rend
// visible (modèle mémoire faible d'ARM64).
// read == write <=> empty
// write + 1 == read <=> full
// note: this way, we loose 1 entry
// read: next read
// write: next write
#define ALEX_RBUF_LEN 1024
char *alex_rollingbuf[ALEX_RBUF_LEN];
static _Atomic int alex_rollingbuf_write_idx = 0;
static _Atomic int alex_rollingbuf_read_idx = 0;

// Arrêt d'un walk en cours demandé par l'app (alex_walk_stop) : vérifié par alex_main entre
// deux requêtes GETNEXT et pendant l'attente d'une place dans l'anneau. Remis à zéro par
// alex_rollingbuf_init, que l'app appelle juste avant de lancer chaque walk.
static _Atomic int alex_stop_requested = 0;

void alex_walk_stop(void) {
    atomic_store_explicit(&alex_stop_requested, 1, memory_order_release);
}

static int alex_walk_stopping(void) {
    return atomic_load_explicit(&alex_stop_requested, memory_order_acquire);
}

#define ALEX_ERRBUF_LEN 4096
char alex_errbuf[ALEX_ERRBUF_LEN];

void alex_errbuf_clear(void) {
    alex_errbuf[0] = 0;
}

void alex_errbuf_get(char *target) {
    stpcpy(target, alex_errbuf);
}

void alex_errbuf_set(char *src) {
    stpcpy(alex_errbuf, src);
}

// Appelé par l'app avant chaque walk, anneau vide (aucun walk en cours)
void alex_rollingbuf_init(void) {
    atomic_store_explicit(&alex_rollingbuf_write_idx, 0, memory_order_release);
    atomic_store_explicit(&alex_rollingbuf_read_idx, 0, memory_order_release);
    atomic_store_explicit(&alex_stop_requested, 0, memory_order_release);
}

static int alex_rollingbuf_next(int idx) {
    return idx + 1 == ALEX_RBUF_LEN ? 0 : idx + 1;
}

// Consommateur : libère les entrées non lues
void alex_rollingbuf_close(void) {
    int r = atomic_load_explicit(&alex_rollingbuf_read_idx, memory_order_relaxed);
    while (r != atomic_load_explicit(&alex_rollingbuf_write_idx, memory_order_acquire)) {
        free(alex_rollingbuf[r]);
        r = alex_rollingbuf_next(r);
        atomic_store_explicit(&alex_rollingbuf_read_idx, r, memory_order_release);
    }
}

int alex_rollingbuf_isfull(void) {
    int w = atomic_load_explicit(&alex_rollingbuf_write_idx, memory_order_acquire);
    int r = atomic_load_explicit(&alex_rollingbuf_read_idx, memory_order_acquire);
    return alex_rollingbuf_next(w) == r;
}

int alex_rollingbuf_isempty(void) {
    int w = atomic_load_explicit(&alex_rollingbuf_write_idx, memory_order_acquire);
    int r = atomic_load_explicit(&alex_rollingbuf_read_idx, memory_order_acquire);
    return w == r;
}

// Producteur. Retourne -1 si l'anneau est plein (ou en cas d'échec d'allocation)
int alex_rollingbuf_push(char *str) {
    int w = atomic_load_explicit(&alex_rollingbuf_write_idx, memory_order_relaxed);
    // acquire : la place libérée par le consommateur (free inclus) est bien disponible
    if (alex_rollingbuf_next(w) == atomic_load_explicit(&alex_rollingbuf_read_idx, memory_order_acquire)) return -1;
    char *copy = malloc(strlen(str) + 1);
    if (copy == NULL) return -1;
    stpcpy(copy, str);
    alex_rollingbuf[w] = copy;
    // release : l'entrée est entièrement écrite avant que l'index la rende visible
    atomic_store_explicit(&alex_rollingbuf_write_idx, alex_rollingbuf_next(w), memory_order_release);
    return 0;
}

// Consommateur : longueur de la prochaine entrée, -1 si l'anneau est vide
int alex_rollingbuf_poplength(void) {
    int r = atomic_load_explicit(&alex_rollingbuf_read_idx, memory_order_relaxed);
    if (r == atomic_load_explicit(&alex_rollingbuf_write_idx, memory_order_acquire)) return -1;
    return strlen(alex_rollingbuf[r]);
}

// Consommateur : copie la prochaine entrée dans target (taille poplength() + 1) et la libère
int alex_rollingbuf_pop(char *target) {
    int r = atomic_load_explicit(&alex_rollingbuf_read_idx, memory_order_relaxed);
    if (r == atomic_load_explicit(&alex_rollingbuf_write_idx, memory_order_acquire)) return -1;
    stpcpy(target, alex_rollingbuf[r]);
    free(alex_rollingbuf[r]);
    alex_rollingbuf[r] = NULL;
    atomic_store_explicit(&alex_rollingbuf_read_idx, alex_rollingbuf_next(r), memory_order_release);
    return 0;
}

#include <net-snmp/net-snmp-config.h>

#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#ifdef HAVE_STRING_H
#include <string.h>
#else
#include <strings.h>
#endif
#include <sys/types.h>
#ifdef HAVE_NETINET_IN_H
# include <netinet/in.h>
#endif
#ifdef TIME_WITH_SYS_TIME
# include <sys/time.h>
# include <time.h>
#else
# ifdef HAVE_SYS_TIME_H
#  include <sys/time.h>
# else
#  include <time.h>
# endif
#endif
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#include <stdio.h>
#ifdef HAVE_NETDB_H
#include <netdb.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif

#include <net-snmp/net-snmp-includes.h>

#define NETSNMP_DS_WALK_INCLUDE_REQUESTED	        1
#define NETSNMP_DS_WALK_PRINT_STATISTICS	        2
#define NETSNMP_DS_WALK_DONT_CHECK_LEXICOGRAPHIC	3
#define NETSNMP_DS_WALK_TIME_RESULTS     	        4
#define NETSNMP_DS_WALK_DONT_GET_REQUESTED	        5
#define NETSNMP_DS_WALK_TIME_RESULTS_SINGLE	        6

oid             objid_mib[] = { 1, 3, 6, 1, 2, 1 };
int             numprinted = 0;

char           *end_name = NULL;

void
usage(void)
{
    fprintf(stderr, "USAGE: snmpwalk ");
    snprintf(alex_errbuf, ALEX_ERRBUF_LEN, "USAGE: snmpwalk");

    snmp_parse_args_usage(stderr);
    fprintf(stderr, " [OID]\n\n");
    snmp_parse_args_descriptions(stderr);
    fprintf(stderr,
            "  -C APPOPTS\t\tSet various application specific behaviours:\n");
    fprintf(stderr, "\t\t\t  p:  print the number of variables found\n");
    fprintf(stderr, "\t\t\t  i:  include given OID in the search range\n");
    fprintf(stderr, "\t\t\t  I:  don't include the given OID, even if no results are returned\n");
    fprintf(stderr,
            "\t\t\t  c:  do not check returned OIDs are increasing\n");
    fprintf(stderr,
            "\t\t\t  t:  Display wall-clock time to complete the walk\n");
    fprintf(stderr,
            "\t\t\t  T:  Display wall-clock time to complete each request\n");
    fprintf(stderr, "\t\t\t  E {OID}:  End the walk at the specified OID\n");
}

void
snmp_get_and_print(netsnmp_session * ss, oid * theoid, size_t theoid_len)
{
    netsnmp_pdu    *pdu, *response;
    netsnmp_variable_list *vars;
    int             status;

    pdu = snmp_pdu_create(SNMP_MSG_GET);
    snmp_add_null_var(pdu, theoid, theoid_len);

    status = snmp_synch_response(ss, pdu, &response);
    if (status == STAT_SUCCESS && response->errstat == SNMP_ERR_NOERROR) {
        for (vars = response->variables; vars; vars = vars->next_variable) {
            numprinted++;
            // Alex : résultat envoyé à l'app par l'anneau, comme ceux du walk (au lieu de stdout)
            char buf[65536];
            snprint_variable(buf, sizeof buf, vars->name, vars->name_length, vars);
            while (alex_rollingbuf_push(buf) == -1 && !alex_walk_stopping())
                usleep(200000);
        }
    }
    if (response) {
        snmp_free_pdu(response);
    }
}

static void
optProc(int argc, char *const *argv, int opt)
{
    switch (opt) {
    case 'C':
        while (*optarg) {
            switch (*optarg++) {
            case 'i':
                netsnmp_ds_toggle_boolean(NETSNMP_DS_APPLICATION_ID,
					  NETSNMP_DS_WALK_INCLUDE_REQUESTED);
                break;

            case 'I':
                netsnmp_ds_toggle_boolean(NETSNMP_DS_APPLICATION_ID,
					  NETSNMP_DS_WALK_DONT_GET_REQUESTED);
                break;

            case 'p':
                netsnmp_ds_toggle_boolean(NETSNMP_DS_APPLICATION_ID,
					  NETSNMP_DS_WALK_PRINT_STATISTICS);
                break;

            case 'c':
                netsnmp_ds_toggle_boolean(NETSNMP_DS_APPLICATION_ID,
				    NETSNMP_DS_WALK_DONT_CHECK_LEXICOGRAPHIC);
                break;

            case 't':
                netsnmp_ds_toggle_boolean(NETSNMP_DS_APPLICATION_ID,
                                          NETSNMP_DS_WALK_TIME_RESULTS);
                break;

            case 'E':
                end_name = argv[optind++];
                break;

            case 'T':
                netsnmp_ds_toggle_boolean(NETSNMP_DS_APPLICATION_ID,
                                          NETSNMP_DS_WALK_TIME_RESULTS_SINGLE);
                break;
                
            default:
                snprintf(alex_errbuf, ALEX_ERRBUF_LEN, "Unknown flag passed to -C: %c\n",
                        optarg[-1]);
                fprintf(stderr, "Unknown flag passed to -C: %c\n",
                        optarg[-1]);
                exit(1);
            }
        }
        break;
    }
}

int
alex_main(int argc, char *argv[])
{
    netsnmp_session session, *ss;
    netsnmp_pdu    *pdu, *response;
    netsnmp_variable_list *vars;
    int             arg;
    oid             name[MAX_OID_LEN];
    size_t          name_length;
    oid             root[MAX_OID_LEN];
    size_t          rootlen;
    oid             end_oid[MAX_OID_LEN];
    size_t          end_len = 0;
    int             count;
    int             running;
    int             status = STAT_ERROR;
    int             check;
    int             exitval = 1;
    struct timeval  tv1, tv2, tv_a, tv_b;

    SOCK_STARTUP;

    // Alex : alex_main est rappelé pour chaque walk dans le même process
    numprinted = 0;

    netsnmp_ds_register_config(ASN_BOOLEAN, "snmpwalk", "includeRequested",
			       NETSNMP_DS_APPLICATION_ID, 
			       NETSNMP_DS_WALK_INCLUDE_REQUESTED);

    netsnmp_ds_register_config(ASN_BOOLEAN, "snmpwalk", "excludeRequested",
			       NETSNMP_DS_APPLICATION_ID, 
			       NETSNMP_DS_WALK_DONT_GET_REQUESTED);

    netsnmp_ds_register_config(ASN_BOOLEAN, "snmpwalk", "printStatistics",
			       NETSNMP_DS_APPLICATION_ID, 
			       NETSNMP_DS_WALK_PRINT_STATISTICS);

    netsnmp_ds_register_config(ASN_BOOLEAN, "snmpwalk", "dontCheckOrdering",
			       NETSNMP_DS_APPLICATION_ID,
			       NETSNMP_DS_WALK_DONT_CHECK_LEXICOGRAPHIC);

    netsnmp_ds_register_config(ASN_BOOLEAN, "snmpwalk", "timeResults",
                               NETSNMP_DS_APPLICATION_ID,
                               NETSNMP_DS_WALK_TIME_RESULTS);

    netsnmp_ds_register_config(ASN_BOOLEAN, "snmpwalk", "timeResultsSingle",
                               NETSNMP_DS_APPLICATION_ID,
                               NETSNMP_DS_WALK_TIME_RESULTS_SINGLE);

    /*
     * get the common command line arguments 
     */
    switch (arg = snmp_parse_args(argc, argv, &session, "C:", optProc)) {
    case NETSNMP_PARSE_ARGS_ERROR:
        goto out;
    case NETSNMP_PARSE_ARGS_SUCCESS_EXIT:
        exitval = 0;
        goto out;
    case NETSNMP_PARSE_ARGS_ERROR_USAGE:
        usage();
        goto out;
    default:
        break;
    }

    /*
     * get the initial object and subtree 
     */
    if (arg < argc) {
        /*
         * specified on the command line 
         */
        rootlen = MAX_OID_LEN;
        if (snmp_parse_oid(argv[arg], root, &rootlen) == NULL) {
            snmp_perror(argv[arg]);
            goto out;
        }
    } else {
        /*
         * use default value 
         */
        memmove(root, objid_mib, sizeof(objid_mib));
        rootlen = sizeof(objid_mib) / sizeof(oid);
    }

    /*
     * If we've been given an explicit end point,
     *  then convert this to an OID, otherwise
     *  move to the next sibling of the start.
     */
    if ( end_name ) {
        end_len = MAX_OID_LEN;
        if (snmp_parse_oid(end_name, end_oid, &end_len) == NULL) {
            snmp_perror(end_name);
            goto out;
        }
    } else {
        memmove(end_oid, root, rootlen*sizeof(oid));
        end_len = rootlen;
        end_oid[end_len-1]++;
    }

    /*
     * open an SNMP session 
     */
    ss = snmp_open(&session);
    if (ss == NULL) {
        /*
         * diagnose snmp_open errors with the input netsnmp_session pointer 
         */
        snmp_sess_perror("snmpwalk", &session);

        char *err;
        snmp_error(&session, NULL, NULL, &err);
        snprintf(alex_errbuf, ALEX_ERRBUF_LEN, "%s", err);

        goto out;
    }

    /*
     * get first object to start walk 
     */
    memmove(name, root, rootlen * sizeof(oid));
    name_length = rootlen;

    running = 1;

    check =
        !netsnmp_ds_get_boolean(NETSNMP_DS_APPLICATION_ID,
                        NETSNMP_DS_WALK_DONT_CHECK_LEXICOGRAPHIC);
    if (netsnmp_ds_get_boolean(NETSNMP_DS_APPLICATION_ID, NETSNMP_DS_WALK_INCLUDE_REQUESTED)) {
        snmp_get_and_print(ss, root, rootlen);
    }

    if (netsnmp_ds_get_boolean(NETSNMP_DS_APPLICATION_ID,
                               NETSNMP_DS_WALK_TIME_RESULTS))
        netsnmp_get_monotonic_clock(&tv1);
    exitval = 0;
    // Alex : arrêt possible depuis l'app, entre deux requêtes (sortie par le chemin normal :
    // session fermée et nettoyée comme après un timeout, le walk suivant repart proprement)
    while (running && !alex_walk_stopping()) {
        /*
         * create PDU for GETNEXT request and add object name to request 
         */
        pdu = snmp_pdu_create(SNMP_MSG_GETNEXT);
        snmp_add_null_var(pdu, name, name_length);

        /*
         * do the request 
         */
        if (netsnmp_ds_get_boolean(NETSNMP_DS_APPLICATION_ID, NETSNMP_DS_WALK_TIME_RESULTS_SINGLE))
            netsnmp_get_monotonic_clock(&tv_a);
        status = snmp_synch_response(ss, pdu, &response);
        if (status == STAT_SUCCESS) {
            if (netsnmp_ds_get_boolean(NETSNMP_DS_APPLICATION_ID, NETSNMP_DS_WALK_TIME_RESULTS_SINGLE))
                netsnmp_get_monotonic_clock(&tv_b);
            if (response->errstat == SNMP_ERR_NOERROR) {
                /*
                 * check resulting variables 
                 */
                for (vars = response->variables; vars;
                     vars = vars->next_variable) {
                    if (snmp_oid_compare(end_oid, end_len,
                                         vars->name, vars->name_length) <= 0) {
                        /*
                         * not part of this subtree
                         */
                        running = 0;
                        continue;
                    }
                    numprinted++;
                    if (netsnmp_ds_get_boolean(NETSNMP_DS_APPLICATION_ID, NETSNMP_DS_WALK_TIME_RESULTS_SINGLE))
                        fprintf(stdout, "%f s: ",
                                (double) (tv_b.tv_usec - tv_a.tv_usec)/1000000 +
                                (double) (tv_b.tv_sec - tv_a.tv_sec));
                    
                    // Alex
                    // printf("XXXXX: ICI11\n");
                    // print_variable(vars->name, vars->name_length, vars);
                    char foo[65536];
                    int retval;
                    snprint_variable(foo, sizeof foo, vars->name, vars->name_length, vars);
                    struct timespec req;

                    do {
                        // printf("var: %s\n", foo);
                        retval = alex_rollingbuf_push(foo);
                        if (retval == -1) {
                            // Arrêt demandé pendant l'attente d'une place : valeur abandonnée
                            if (alex_walk_stopping()) {
                                running = 0;
                                break;
                            }
                            // printf("XXXXX: attendre 0.2 sec\n");
                            usleep(200000);
                        }
                    } while (retval == -1);
                    // printf("XXXXX: ICI2\n");
                    
                    if ((vars->type != SNMP_ENDOFMIBVIEW) &&
                        (vars->type != SNMP_NOSUCHOBJECT) &&
                        (vars->type != SNMP_NOSUCHINSTANCE)) {
                        /*
                         * not an exception value 
                         */
                        if (check
                            && snmp_oid_compare(name, name_length,
                                                vars->name,
                                                vars->name_length) >= 0) {
                            fflush(stdout);
                            fprintf(stderr, "Error: OID not increasing: ");
                            snprintf(alex_errbuf, ALEX_ERRBUF_LEN, "Error: OID not increasing");
                            fprint_objid(stderr, name, name_length);
                            fprintf(stderr, " >= ");
                            fprint_objid(stderr, vars->name,
                                         vars->name_length);
                            fprintf(stderr, "\n");
                            running = 0;
                            exitval = 1;
                        }
                        memmove((char *) name, (char *) vars->name,
                                vars->name_length * sizeof(oid));
                        name_length = vars->name_length;
                    } else
                        /*
                         * an exception value, so stop 
                         */
                        running = 0;
                }
            } else {
                /*
                 * error in response, print it 
                 */
                running = 0;
                if (response->errstat == SNMP_ERR_NOSUCHNAME) {
                    printf("End of MIB\n");
                } else {
                    snprintf(alex_errbuf, ALEX_ERRBUF_LEN, "Error in packet.\nReason: %s\n",
                            snmp_errstring(response->errstat));
                    fprintf(stderr, "Error in packet.\nReason: %s\n",
                            snmp_errstring(response->errstat));
                    if (response->errindex != 0) {
                        fprintf(stderr, "Failed object: ");
                        for (count = 1, vars = response->variables;
                             vars && count != response->errindex;
                             vars = vars->next_variable, count++)
                            /*EMPTY*/;
                        if (vars)
                            fprint_objid(stderr, vars->name,
                                         vars->name_length);
                        fprintf(stderr, "\n");
                    }
                    exitval = 2;
                }
            }
        } else if (status == STAT_TIMEOUT) {
            snprintf(alex_errbuf, ALEX_ERRBUF_LEN, "Timeout: No Response from %s\n",
                     session.peername);
            fprintf(stderr, "Timeout: No Response from %s\n",
                    session.peername);
            running = 0;
            exitval = 1;
        } else {                /* status == STAT_ERROR */
            snmp_sess_perror("snmpwalk", ss);

            char *err;
            snmp_error(ss, NULL, NULL, &err);
            snprintf(alex_errbuf, ALEX_ERRBUF_LEN, "%s", err);

            running = 0;
            exitval = 1;
        }
        if (response)
            snmp_free_pdu(response);
    }
    if (netsnmp_ds_get_boolean(NETSNMP_DS_APPLICATION_ID,
                               NETSNMP_DS_WALK_TIME_RESULTS))
        netsnmp_get_monotonic_clock(&tv2);

    if (alex_walk_stopping()) {
        snprintf(alex_errbuf, ALEX_ERRBUF_LEN, "Walk interrupted");
    } else if (numprinted == 0 && status == STAT_SUCCESS) {
        /*
         * no printed successful results, which may mean we were
         * pointed at an only existing instance.  Attempt a GET, just
         * for get measure. 
         */
        if (!netsnmp_ds_get_boolean(NETSNMP_DS_APPLICATION_ID, NETSNMP_DS_WALK_DONT_GET_REQUESTED)) {
            snmp_get_and_print(ss, root, rootlen);
        }
    }
    snmp_close(ss);

    if (netsnmp_ds_get_boolean(NETSNMP_DS_APPLICATION_ID,
                               NETSNMP_DS_WALK_PRINT_STATISTICS)) {
        printf("Variables found: %d\n", numprinted);
    }
    if (netsnmp_ds_get_boolean(NETSNMP_DS_APPLICATION_ID,
                               NETSNMP_DS_WALK_TIME_RESULTS)) {
        fprintf (stderr, "Total traversal time = %f seconds\n",
                 (double) (tv2.tv_usec - tv1.tv_usec)/1000000 +
                 (double) (tv2.tv_sec - tv1.tv_sec));
    }

out:
    netsnmp_cleanup_session(&session);
    SOCK_CLEANUP;
    return exitval;
}
