/* $Id$
 * 
 * Implementation of main() which directly calls first hilti_init() and then
 * hilti_multithreaded_run().
 * 
 */

#include <getopt.h>

#include "hilti.h"

// Top-level function generated by HILTI compiler. The user must define this
// function when linking with libhiltimain.
extern void main_run();

static void usage(const char* prog)
{
    printf("%s [options]\n"
           "\n"
           "  -h| --help           Show usage information.\n"
           "  -t| --threads <num>  Number of worker threads. [default: 2]\n"
           "\n", prog);
    
    exit(1);
}

static struct option long_options[] = {
    {"threads", required_argument, 0, 't'},
    {0, 0, 0, 0}
};

int main(int argc, char **argv)
{
    int threads = 2;
    
    while ( 1 ) {
        char c = getopt_long (argc, argv, "ht:", long_options, 0);
        
        if ( c == -1 )
            break;
        
        switch ( c ) {
          case 't':
            threads = atoi(optarg);
            break;
            
          default:
            usage(argv[0]);
        }
    }

    if ( optind != argc )
        usage(argv[0]);

    hlt_init();
    
    hlt_config cfg = *hlt_config_get();
    cfg.num_workers = threads;
    hlt_config_set(&cfg);
    
    hlt_exception* excpt = 0;

    hlt_threading_start();
    
    main_run(&excpt);
    
    hlt_threading_stop(&excpt);

    if ( excpt )
        hlt_exception_print_uncaught(excpt, __hlt_global_execution_context);

    return 0;
}
