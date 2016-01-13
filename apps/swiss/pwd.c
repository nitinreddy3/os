/*++

Copyright (c) 2013 Minoca Corp. All Rights Reserved

Module Name:

    pwd.c

Abstract:

    This module implements support for the pwd (print working directory)
    utility.

Author:

    Evan Green 26-Aug-2013

Environment:

    POSIX

--*/

//
// ------------------------------------------------------------------- Includes
//

#include <minoca/types.h>

#include "swlib.h"

//
// ---------------------------------------------------------------- Definitions
//

//
// ------------------------------------------------------ Data Type Definitions
//

//
// ----------------------------------------------- Internal Function Prototypes
//

//
// -------------------------------------------------------------------- Globals
//

//
// ------------------------------------------------------------------ Functions
//

INT
PwdMain (
    INT ArgumentCount,
    CHAR **Arguments
    )

/*++

Routine Description:

    This routine is the main entry point for the pwd (print working directory)
    utility.

Arguments:

    ArgumentCount - Supplies the number of command line arguments the program
        was invoked with.

    Arguments - Supplies a tokenized array of command line arguments.

Return Value:

    0 on success.

    Non-zero error on failure.

--*/

{

    return SwPwdCommand(ArgumentCount, Arguments);
}

//
// --------------------------------------------------------- Internal Functions
//
