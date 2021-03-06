/* dboom is an HTTP load generator written in C using libdill coroutines */
#include <libdill.h>

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <time.h>

#include "dboom.h"
#include "req.h"

#define DEFAULT_REQUESTS    10
#define DEFAULT_CONCURR     5
#define DEFAULT_TIMEOUT     5000    // ms

static unsigned int getRequests(const char*);
static unsigned int getConcurrentReqs(const char*);
static int getTimeout(const char*);
static void usage();
static struct reqstats reqstats_new();

coroutine void boom(const char*, unsigned int, int, int, int);
coroutine void stats(int, int, int);

int main(int argc, char **argv) {

    char *requests = NULL;
    char *concurr = NULL;
    char *timeout = NULL;
    int verbose = 0;

    int c;
    while((c = getopt(argc, argv, "n:c:t:v")) != -1) {
        switch(c) {
        case 'n':
            requests = optarg;
            break;
        case 'c':
            concurr = optarg;
            break;
        case 't':
            timeout = optarg;
            break;
        case 'v':
            verbose = 1;
            break;
        default:
            usage();
            break;  // Unreachable
        }
    }

    /* Exit if no url provided */
    if(optind == argc) usage();
    /* TODO: Accept > 1 URL */
    const char* url = argv[optind];
    /* Validate program args */
    unsigned int nreqs = getRequests(requests);
    unsigned int nconcurr = getConcurrentReqs(concurr);
    int ntimeout = getTimeout(timeout);
    if(nreqs == 0 || nconcurr == 0) {
        fprintf(stderr,
            "The number of requests (%d) and the number of"
             "concurrent requests (%d) must be greater than 0.\n",
             nreqs, nconcurr);
        exit(EXIT_FAILURE);
    }
    /* The number of requests cannot be less than the number of concurrent
       requests. */
    if(nreqs < nconcurr) {
        fprintf(stderr,
            "The number of requests (%d) cannot be less than the number of "
             "concurrent requests (%d)\n", nreqs, nconcurr);
        exit(EXIT_FAILURE);
    }

    printf("Running dboom\n\
        Url: %s\n\
        Total Requests: %d\n\
        Concurrent Requests: %d\n\
        Timeout: %d ms\n", url, nreqs, nconcurr, ntimeout);
    
    /* Each boom() coroutine writes to this channel when done. This allows 
       main() to wait for all boom() coroutines to complete before exiting. */
    int done_ch = chmake(sizeof(int));
    /* Each boom() coroutine uses this channel to record statistics. */
    int stats_ch = chmake(sizeof(struct reqstats));
    /* stats() coroutine and main() uses this channel to control stats
       cleanup and shutdown. */
    int stop_ch = chmake(sizeof(int));

    if(done_ch < 0 || stats_ch < 0 || stop_ch < 0) {
        perror("Could not create channel");
        exit(EXIT_FAILURE);
    }
    /* Record start time */
    time_t start_t, end_t;
    time(&start_t);

    /* Launch coroutine for recording statistics */
    int stats_cor = 0;
    stats_cor = go(stats(stats_ch, stop_ch, verbose));
    if(stats_cor < 0) {
        perror("Could not start stats coroutine");
        exit(EXIT_FAILURE);
    }
    /* Launch nconcurr coroutines, each one sending nreqs/nconcurr requests. */
    int *pcoh = malloc(nconcurr * sizeof(int));
    if(pcoh == NULL) {
        perror("Could not allocate memory for coroutine handles");
        exit(EXIT_FAILURE);
    }
    for(int i = 0; i < nconcurr; ++i) {
        pcoh[i] = go(boom(url, nreqs/nconcurr, ntimeout, done_ch, stats_ch));
        if(pcoh[i] < 0) {
            perror("Could not launch coroutine");
            exit(EXIT_FAILURE);
        }
    }
    /* Wait for boom() coroutines to end */
    int rc = 0;
    int done = 0;
    for(int i = nconcurr; i > 0; --i) {
        rc = chrecv(done_ch, &done, sizeof(done), -1);
        if(rc != 0) {
            perror("Could not receive on done_ch");
            exit(EXIT_FAILURE);
        }
    }
    /* Tell stats coroutine to end */
    int stop = 1;
    rc = chsend(stop_ch, &stop, sizeof(stop), -1);
    if(rc != 0) perror("Failed to send on stop_ch");
    /* Wait for stats to end */
    rc = chrecv(stop_ch, &stop, sizeof(stop), -1);
    if(rc != 0) perror("Failed to receive on stop_ch");
    
    /* Clean up resources */

    /* Coroutines */
    for(int i = 0; i < nconcurr; ++i)
        if(hclose(pcoh[i])) perror("Could not close boom coroutine");
    if(hclose(stats_cor)) perror("Could not close stats coroutine");
    /* Channels */    
    if(hclose(done_ch)) perror("Could not close done_ch");
    if(hclose(stats_ch)) perror("Could not close stats_ch");
    if(hclose(stop_ch)) perror("Could not close stop_ch");
    /* Memory for coroutine array */
    free(pcoh);

    /* Print run time */
    time(&end_t);
    printf("Run time: %fs\n", difftime(end_t, start_t));
    
    exit(EXIT_SUCCESS);
}

coroutine void boom(const char* url, unsigned int nreqs, int timeout,
                    int done_ch, int stats_ch) {
    int rc = 0;
    /* Send requests until no more requests */
    for(int i = nreqs; i > 0; --i) {
        struct reqstats rs = reqstats_new();
        if(MakeRequest(url, timeout, &rs) == 0) {
            rc = chsend(stats_ch, &rs, sizeof(rs), -1);
            if(rc != 0) perror("Failed to send request stats");
        }
    }
    /* clean up and signal done */
    int done = 1;
    rc = chsend(done_ch, &done, sizeof(done), -1);
    if(rc != 0) perror("boom() - chsend() failed");
}

coroutine void stats(int stats_ch, int stop_ch, int verbose)
{
    int rc = 0;
    int nrequests = 0;
    int stop = 0;
    struct reqstats rs;
    unsigned int total = 0;

    struct chclause clauses[] = {
        {CHRECV, stop_ch, &stop, sizeof(stop)},
        {CHRECV, stats_ch, &rs, sizeof(rs)}
    };
    
    while(stop == 0) {
        rc = choose(clauses, 2, -1);
        if(rc < 0) {
            perror("Could not read from stop or stats channel");
            break;
        }
        if(rc == 1) {
            /* Request stats available */
            nrequests++;
            total += rs.tm;
            if(verbose)
                printf("%d,%ld\n", rs.http_code, rs.tm);
        }
    }
    if(nrequests > 0)
        /* Display stats and signal done */
        printf("Avg response time for %d requests: %d ms\n", nrequests, total/nrequests);

    /* signal done to main */
    rc = chsend(stop_ch, &stop, sizeof(stop), -1);
    if(rc != 0) perror("stats() - chsend() failed");
}

/* Create and initialize a new reqstat struct */
static struct reqstats
reqstats_new()
{
    struct reqstats rs;
    rs.tm = 0;
    rs.http_code = 0;
    return rs;
}

static
void usage()
{
    fprintf(stderr, "Usage: dboom [-n nreqs] [-c nconcurr] [-t timeoutms] URL.\n");
    exit(EXIT_FAILURE);
}

static
unsigned int getRequests(const char *requests)
{
    return requests ? atoi(requests) : DEFAULT_REQUESTS;
}

static
unsigned int getConcurrentReqs(const char *concurr)
{
    return concurr ? atoi(concurr) : DEFAULT_CONCURR;
}

static
int getTimeout(const char *timeout)
{
    return timeout ? atoi(timeout) : DEFAULT_TIMEOUT;
}
