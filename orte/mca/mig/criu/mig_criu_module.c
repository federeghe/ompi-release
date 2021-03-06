/*
 * Copyright (c) 2004-2005 The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2004-2005 The University of Tennessee and The University
 *                         of Tennessee Research Foundation.  All rights
 *                         reserved.
 * Copyright (c) 2004-2005 High Performance Computing Center Stuttgart,
 *                         University of Stuttgart.  All rights reserved.
 * Copyright (c) 2004-2005 The Regents of the University of California.
 *                         All rights reserved.
 * Copyright (c) 2014      Intel, Inc.  All rights reserved.
 * Copyright (c) 2015	   Politecnico di Milano.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

#include "orte_config.h"
#include "orte/constants.h"
#include "orte/types.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif
#ifdef HAVE_ARPA_INET_H
#include <arpa/inet.h>
#endif
#include <errno.h>

#include "orte/mca/state/state.h"
#include "orte/mca/errmgr/errmgr.h"
#include "orte/runtime/orte_globals.h"
#include "orte/util/name_fns.h"
#include "orte/mca/rmaps/base/base.h"
#include "orte/runtime/orte_globals.h"
#include "orte/mca/plm/plm.h"
#include "orte/mca/plm/plm_types.h"

#include "mig_criu.h"
#include "orte/mca/mig/base/base.h"
#include "orte/mca/mig/mig_types.h"

#include "orte/mca/ras/bbq/bbq_ompi_types.h"
#include "orte/mca/ras/ras.h"
#include "orte/mca/ras/base/base.h"

#include "criu/criu.h"

#if ORTE_MIG_OVERHEAD_TEST
    struct timespec dump_s_t;
    struct timespec dump_e_t;
    struct timespec restore_s_t;
    struct timespec restore_e_t;
#endif

#define RESTORE_PATH_PREFIX "/tmp/ckpt_"

static int init(void);
static int orte_mig_criu_dump(pid_t fpid);
static int orte_mig_criu_finalize(void);
static char *orte_mig_criu_get_name(void);
static orte_mig_migration_state_t orte_mig_criu_get_state(void);
static int orte_mig_criu_restore(void);
static int orte_mig_criu_migrate(char* host, char* path, int pid_to_restore);
/*
 * Global variables
 */

orte_mig_migration_state_t mig_state;

char dump_path[sizeof(RESTORE_PATH_PREFIX)+10];

orte_mig_base_module_t orte_mig_criu_module = {
    init,
    orte_mig_base_prepare_migration,
    orte_mig_criu_dump,
    orte_mig_criu_migrate,
    orte_mig_criu_restore,
    orte_mig_base_fwd_info,
    orte_mig_criu_finalize,
    orte_mig_criu_get_state,
    orte_mig_criu_get_name  
};

static int init(void){
    /*TODO: checks to flag us as available*/


    mig_state = MIG_AVAILABLE;
    opal_output_verbose(0, orte_mig_base_framework.framework_output,
                "%s mig:criu: Criu module initialized.",
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
    return ORTE_SUCCESS;
}

static int orte_mig_criu_dump(pid_t fpid){
    
#if ORTE_MIG_OVERHEAD_TEST
    clock_gettime(CLOCK_MONOTONIC, &dump_s_t);
#endif
    int dir;
    
    sprintf(dump_path, "/tmp/ckpt_%d", fpid);
    
    opal_output_verbose(0,orte_mig_base_framework.framework_output,
                "%s orted:mig:criu Setting parameters", 
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
    
    if(0 != mkdir(dump_path, S_IRWXU | S_IRWXG | S_IROTH | S_IXOTH)){
        opal_output_verbose(0,orte_mig_base_framework.framework_output,
                "%s orted:mig: Error while creating dump folder", 
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
        return ORTE_ERROR;
    }
    
    criu_init_opts();
    criu_set_log_file("criu_dump.log");
    criu_set_log_level(4);
    criu_set_pid(fpid);
    criu_set_leave_running(false);
    criu_set_tcp_established(true);
    criu_set_ghost_limit(1024 * 1024 * 1024); // 1 GB
    
    if(0 > (dir = open(dump_path, O_DIRECTORY))){
        opal_output_verbose(0,orte_mig_base_framework.framework_output,
                "%s orted:mig:criu Error while opening folder", 
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
        return ORTE_ERROR;
    }
        
    criu_set_images_dir_fd(dir);

    opal_output_verbose(0,orte_mig_base_framework.framework_output,
                "%s orted:mig:criu Dumping father process", 
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
    
    if(0 > criu_dump()){
        opal_output_verbose(0,orte_mig_base_framework.framework_output,
                "%s orted:mig:criu Unable to dump parent process, error %d",
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), errno);

        return ORTE_ERROR;
    }

    opal_output_verbose(0,orte_mig_base_framework.framework_output,
                "%s orted:mig:criu Dumped",
                ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));


    mig_state = MIG_MOVING;
    
#if ORTE_MIG_OVERHEAD_TEST
    clock_gettime(CLOCK_MONOTONIC, &dump_e_t);
    fprintf(stdout,
        "%s #TS B %.5f\n", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
        ((double)dump_e_t.tv_sec + 1.0e-9*dump_e_t.tv_nsec) - 
        ((double)dump_s_t.tv_sec + 1.0e-9*dump_s_t.tv_nsec));
    fflush(stdout);
#endif

    return ORTE_SUCCESS;
}


static int orte_mig_criu_finalize(void){
    return ORTE_SUCCESS;
}

static char *orte_mig_criu_get_name(void){
    return "criu";
}

static orte_mig_migration_state_t orte_mig_criu_get_state(void){
    return mig_state;
}

static int orte_mig_criu_migrate(char* host, char* path, int pid_to_restore) {
    (void) path;    // Ignored
    return orte_mig_base_migrate(host,dump_path, pid_to_restore);
}

static void gen_random(char *s, const int len) {
    static const char alphanum[] =
        "0123456789"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "abcdefghijklmnopqrstuvwxyz";

    for (int i = 0; i < len; ++i) {
        s[i] = alphanum[rand() % (sizeof(alphanum) - 1)];
    }

    s[len] = '\0';
}

/*static void sig_child_handler(int s) {
    static int x=0;
    if (s != SIGCLD)
        return; // what's happened here??

    if (x++ <= 0)
        return; // It is normal that first child dies.

    opal_output_verbose(5,orte_mig_base_framework.framework_output,"mig:criu sig cld received.");
    exit(0);    // All ok, bye bye
}*/

static int orte_mig_criu_restore(void) {

    //signal(SIGCLD, sig_child_handler);

    // Generate a random string for the directory
    srand(time(NULL));
    int prefix_len = strlen(RESTORE_PATH_PREFIX);
    char* path = malloc(sizeof(char)*(prefix_len + 10 ));
    strcpy(path, RESTORE_PATH_PREFIX);
    gen_random(path + prefix_len, 5);


    int pid_to_restore = orte_mig_base_restore(path);
    
#if ORTE_MIG_OVERHEAD_TEST
    clock_gettime(CLOCK_MONOTONIC, &restore_s_t);
#endif

    if ( pid_to_restore < 0 ) {
        return ORTE_ERROR;
    }

    opal_output_verbose(2,orte_mig_base_framework.framework_output,"mig:criu old PID before unshare is %i", getpid());
    unshare(CLONE_NEWPID | CLONE_NEWNS );
    int pid = fork();
    if (pid != 0) {
        int status;
        waitpid(-1, &status, 0);
        opal_output_verbose(20,orte_mig_base_framework.framework_output,"mig:criu sig cld received. %i",status);
        waitpid(-1, &status, 0);
        opal_output_verbose(20,orte_mig_base_framework.framework_output,"mig:criu sig cld received. %i",status);
        return status;
    }

    /* NO RETURNs BEYOND THIS POINT */

    opal_output_verbose(2,orte_mig_base_framework.framework_output,"mig:criu new PID after unshare is %i", getpid());


    if (mount("none", "/proc", NULL, MS_PRIVATE|MS_REC, NULL)) {
        opal_output(0,"mig:criu Can't umount proc! errno=%i", errno);
        exit(ORTE_ERROR);
    }

    if (mount("proc", "/proc", "proc", MS_NOSUID|MS_NOEXEC|MS_NODEV, NULL)) {
        opal_output(0,"mig:criu Can't mount proc! errno=%i", errno);
        exit(ORTE_ERROR);
    }


    if (mount("devpts", "/dev/pts", "devpts", MS_MGC_VAL | MS_NOSUID | MS_NOEXEC, "newinstance") ) {
        opal_output(0,"mig:criu Can't mount pts! errno=%i", errno);
        exit(ORTE_ERROR);
    }

    if (mount("/dev/pts/ptmx", "/dev/ptmx", NULL, MS_MGC_VAL | MS_NOSUID | MS_NOEXEC | MS_BIND, NULL) ) {
        opal_output(0,"mig:criu Can't mount ptmx! errno=%i", errno);
        exit(ORTE_ERROR);
    }

    int dir;

    criu_init_opts();
    if(0 > (dir = open(path, O_DIRECTORY))){
        opal_output(0,"%s mig:criu Error while opening folder", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
        exit(ORTE_ERROR);
    }

    criu_set_images_dir_fd(dir);
    criu_set_log_file("criu_restore.log");
    criu_set_log_level(4);
    criu_set_tcp_established(true);


    int status = criu_restore();

    if (status < 0 ) {
        opal_output(0,"%s mig:criu Error during restore, please check criu_restore.log",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME));
        exit(ORTE_ERROR);
    }

    if (0 > kill(pid_to_restore, SIGUSR2) ) {
        opal_output(0,"%s mig:criu Can't signal process %i",
                        ORTE_NAME_PRINT(ORTE_PROC_MY_NAME), pid_to_restore);
        exit(ORTE_ERROR);
    }
    
#if ORTE_MIG_OVERHEAD_TEST
    clock_gettime(CLOCK_MONOTONIC, &restore_e_t);
    fprintf(stdout,
        "%s #TS F %.5f\n", ORTE_NAME_PRINT(ORTE_PROC_MY_NAME),
        ((double)restore_e_t.tv_sec + 1.0e-9*restore_e_t.tv_nsec) - 
        ((double)restore_s_t.tv_sec + 1.0e-9*restore_s_t.tv_nsec));
    fflush(stdout);
#endif

    // We have to wait the termination of the restored process to
    // guarantee the output will be forwarded to ssh and then to
    // hnp.
    wait(NULL);


    exit(ORTE_SUCCESS);


}
