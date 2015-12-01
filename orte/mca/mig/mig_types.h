/*
* Copyright (c) 2016 Politecnico di Milano, Inc.  All rights reserved.
* $COPYRIGHT$
* 
* Additional copyrights may follow
* 
* $HEADER$
*
*/

#ifndef MIG_TYPES_H
#define	MIG_TYPES_H

#include "orte_config.h"

// Costants for event
#define ORTE_MIG_PREPARE 0
#define ORTE_MIG_EXEC 1
#define ORTE_MIG_DONE 2


BEGIN_C_DECLS

typedef enum{
    MIG_AVAILABLE,
    MIG_MOVING,
    MIG_FINISHED,
    MIG_ERROR
}orte_mig_migration_state_t;

END_C_DECLS

#endif	/* MIG_TYPES_H */