/*
 *  Prompt, command completion and version information.
 *
 *  Copyright (C) 2006,2007,2009 Tavis Ormandy <taviso@sdf.lonestar.org>
 *  Copyright (C) 2010,2011 Lu Wang <coolwanglu@gmail.com>
 *
 *  This file is part of libscanmem.
 *
 *  This library is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published
 *  by the Free Software Foundation; either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public License
 *  along with this library.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <string.h>
#include <stdbool.h>

#if HAVE_LIBREADLINE
#include <readline/readline.h>
#include <readline/history.h>
#else
#include "readline.h"
#endif

#include "getline.h"
#include "scanmem.h"
#include "commands.h"
#include "show_message.h"

/* command generator for readline completion */
static char *commandgenerator(const char *text, int state)
{
    static unsigned idx = 0;
    unsigned        i;
    size_t          len;
    element_t      *np;
    globals_t      *vars = &sm_globals;

    /* reset generator if state == 0, otherwise continue from last time */
    idx = state ? idx : 0;

    np = vars->commands ? vars->commands->head : NULL;

    len = strlen(text);

    /* skip to the last node checked */
    for (i = 0; np && i < idx; i++)
        np = np->next;

    /* traverse the commands list, checking for matches */
    while (np) {
        command_t *cmd = np->data;

        np = np->next;

        /* record progress */
        idx++;

        /* if shortdoc is NULL, this is not supposed to be user visible */
        if (cmd == NULL || cmd->command == NULL || cmd->shortdoc == NULL)
            continue;

        /* check if we have a match */
        if (!strncmp(text, command->command, len))
            return strdup(command->command);
    }

    return NULL;
}

/* custom completor program for readline */
static char **commandcompletion(const char *text, int start, int end)
{
    (void) end;

    /*
     * never use default completer (filenames), even if I dont
     * generate any matches
     */
    rl_attempted_completion_over = 1;

    /* only complete on the first word, the command */
    return start ? NULL : rl_completion_matches(text, commandgenerator);
}


/*
 * sm_getcommand() reads in a command using readline, and places a pointer to
 * the read string into *line, _which must be free'd by caller_.
 * returns true on success, or false on error.
 */
bool sm_getcommand(globals_t *vars, char **line)
{
    char prompt[64];
    bool success = true;

    assert(vars != NULL);

    snprintf(prompt, sizeof(prompt), "%ld> ", vars->num_matches);

    rl_readline_name                 = "scanmem";
    rl_attempted_completion_function = commandcompletion;

    while (true) {
        if (!vars->options.backend)
            /*
             * for normal users, read in the next command
             * using readline library
             */
            success = ((*line = readline(prompt)) != NULL);
        else {
            /*
             * disable readline for front-end, since readline may produce
             * ansi escape codes, which is terrible for the front-end
             */
            printf("%s\n", prompt); /* add a newline for front-end */
            fflush(stdout); /* otherwise front-end may not receive this */
            *line = NULL; /* let getline malloc it */
            size_t  n;
            ssize_t bytes_read = getline(line, &n, stdin);

            success = (bytes_read > 0);
            if (success)
                (*line)[bytes_read-1] = '\0'; /* remove the trialing newline */
        }

        if (!success)
            /* EOF */
            if ((*line = strdup("__eof")) == NULL) {
                fprintf(stderr,
                        "error: sorry, there was a memory allocation error.\n");
                return false;
            }

        if (strlen(*line))
            break;

        free(*line);
    }

    /* record this line to readline history */
    add_history(*line);

    return true;
}
