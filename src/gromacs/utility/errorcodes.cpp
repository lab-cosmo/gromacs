/*
 *
 *                This source code is part of
 *
 *                 G   R   O   M   A   C   S
 *
 *          GROningen MAchine for Chemical Simulations
 *
 * Written by David van der Spoel, Erik Lindahl, Berk Hess, and others.
 * Copyright (c) 1991-2000, University of Groningen, The Netherlands.
 * Copyright (c) 2001-2009, The GROMACS development team,
 * check out http://www.gromacs.org for more information.
 *
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
 */
/*! \internal \file
 * \brief
 * Implements functions in errorcodes.h.
 *
 * \author Teemu Murtola <teemu.murtola@cbr.su.se>
 * \ingroup module_utility
 */
#include "gromacs/utility/errorcodes.h"

#include <cstdlib>

#include "gromacs/legacyheaders/thread_mpi/mutex.h"

#include "errorformat.h"

// This has to match the enum in errorcodes.h
static const char *const error_names[] =
{
    "No error",
    "Out of memory",
    "File not found",
    "System I/O error",
    "Error in user input",
    "Inconsistency in user input",
    "Simulation instability detected",

    "Feature not implemented",
    "Invalid value (bug)",
    "Invalid call (bug)",
    "Internal error (bug)",
    "API error (bug)",
    "Range checking error (possible bug)",
    "Communication error (possible bug)",

    "Unknown error",
};

namespace gmx
{

const char *getErrorCodeString(int errorcode)
{
    if (errorcode < 0 || errorcode >= eeUnknownError)
    {
        errorcode = eeUnknownError;
    }
    return error_names[errorcode];
}

static void standardErrorHandler(int retcode, const char *msg,
                                 const char *file, int line)
{
    const char *title = getErrorCodeString(retcode);
    internal::printFatalError(stderr, title, msg, NULL, file, line);
    std::exit(1);
}

static ErrorHandlerFunc error_handler = standardErrorHandler;
static tMPI::mutex handler_mutex;

ErrorHandlerFunc setFatalErrorHandler(ErrorHandlerFunc handler)
{
    tMPI::lock_guard<tMPI::mutex> lock(handler_mutex);
    ErrorHandlerFunc old_handler = error_handler;
    error_handler = handler;
    return old_handler;
}

/*! \cond internal */
namespace internal
{

void fatalError(int retcode, const char *msg, const char *file, int line)
{
    ErrorHandlerFunc handler = NULL;
    {
        tMPI::lock_guard<tMPI::mutex> lock(handler_mutex);
        handler = error_handler;
    }
    if (handler != NULL)
    {
        handler(retcode, msg, file, line);
    }
}

} // namespace internal
//! \endcond

} // namespace gmx
