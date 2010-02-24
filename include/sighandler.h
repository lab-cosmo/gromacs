/*
 * 
 *                This source code is part of
 * 
 *                 G   R   O   M   A   C   S
 * 
 *          GROningen MAchine for Chemical Simulations
 * 
 *                        VERSION 3.2.0
 * Written by David van der Spoel, Erik Lindahl, Berk Hess, and others.
 * Copyright (c) 1991-2000, University of Groningen, The Netherlands.
 * Copyright (c) 2001-2004, The GROMACS development team,
 * check out http://www.gromacs.org for more information.

 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * If you want to redistribute modifications, please consider that
 * scientific software is very special. Version control is crucial -
 * bugs must be traceable. We will be happy to consider code for
 * inclusion in the official distribution, but derived work must not
 * be called official GROMACS. Details are found in the README & COPYING
 * files - if they are missing, get the official version at www.gromacs.org.
 * 
 * To help us fund GROMACS development, we humbly ask that you cite
 * the papers on the package - you can find them in the top README file.
 * 
 * For more info, check our website at http://www.gromacs.org
 * 
 * And Hey:
 * Gromacs Runs On Most of All Computer Systems
 */

#ifndef _sighandler_h
#define _sighandler_h

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The following two variables and the signal_handler function
 * are used from md.c and pme.c as well 
 *
 * Do not fear these global variables: they represent inherently process-global
 * information that needs to be shared across threads 
 */


/* we got a signal to stop in the next step: */
extern volatile sig_atomic_t bGotStopNextStepSignal;
/* we got a signal to stop in the next neighbour search step: */
extern volatile sig_atomic_t bGotStopNextNSStepSignal;

/* our names for the handled signals. These must match the number given
   in signal_handler.*/
extern const char *signal_name[];
/* the last signal received, according to the numbering
   we use in signal_name. Is set to -1 if no signal has (yet) 
   been  received */
extern volatile sig_atomic_t last_signal_number_recvd;


/* prototype for the signal handler */
extern RETSIGTYPE signal_handler(int n);


#ifdef __cplusplus
}
#endif


#endif	/* _sighandler_h */
