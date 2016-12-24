/*
    Specific command handling.

    Copyright (C) 2006,2007,2009 Tavis Ormandy <taviso@sdf.lonestar.org>
    Copyright (C) 2009           Eli Dupree <elidupree@charter.net>
    Copyright (C) 2009,2010      WANG Lu <coolwanglu@gmail.com>
    Copyright (C) 2014-2016      Sebastian Parschauer <s.parschauer@gmx.de>

    This file is part of libscanmem.

    This library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published
    by the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this library.  If not, see <http://www.gnu.org/licenses/>.
*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "config.h"

#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <getopt.h>
#include <signal.h>
#include <assert.h>
#include <setjmp.h>
#include <alloca.h>
#include <strings.h>
#include <string.h>
#include <stdbool.h>
#include <time.h>
#include <sys/time.h>
#include <limits.h> /* to determine the word width */
#include <errno.h>
#include <inttypes.h>
#include <ctype.h>

#include "commands.h"
#include "endianness.h"
#include "handlers.h"
#include "interrupt.h"
#include "show_message.h"

/* macro to hide gcc unused warnings */
#define USEPARAMS() ((void) vars, (void) argv, (void) argc)

/*
 * This file defines all the command handlers used, each one is registered
 * using registercommand(), and when a matching command is entered, the
 * commandline is tokenized and parsed into an argv/argc.
 * 
 * argv[0] will contain the command entered, so one handler can handle multiple
 * commands by checking whats in there, but you still need seperate
 * documentation for each command when you register it.
 *
 * Most commands will also need some documentation, see handlers.h for
 * the format.
 *
 * Commands are allowed to read and modify settings in the vars structure.
 *
 */

#define calloca(x, y) (memset(alloca((x) * (y)), 0x00, (x) * (y)))

/* try to determine the size of a pointer */
#ifndef ULONG_MAX
#warning ULONG_MAX is not defined!
#endif
#if ULONG_MAX == 4294967295UL
#define POINTER_FMT "%8lx"
#elif ULONG_MAX == 18446744073709551615UL
#define POINTER_FMT "%12lx"
#else
#define POINTER_FMT "%12lx"
#endif

bool handler__set(globals_t * vars, char **argv, unsigned argc)
{
    unsigned blk, seconds = 1;
    char    *delay = NULL;
    char    *end;
    bool     cont = false;

    struct setting {
        char *matchids;
        char *value;
        unsigned seconds;
    } *settings = NULL;

    assert(argc != 0);
    assert(argv != NULL);
    assert(vars != NULL);


    if (argc < 2) {
        show_error("expected an argument, type `help set` for details.\n");
        return false;
    }

    /* supporting `set` for bytearray will cause annoying syntax problems... */
    if ((vars->options.scan_data_type == BYTEARRAY) ||
        (vars->options.scan_data_type == STRING   )) {
        show_error("`set` is not supported for bytearray or string, "
                   "use `write` instead.\n");
        return false;
    }

    /* check if there are any matches */
    if (vars->num_matches == 0) {
        show_error("no matches are known.\n");
        return false;
    }

    /* --- parse arguments into settings structs --- */

    settings = calloca(argc - 1, sizeof(struct setting));

    /* parse every block into a settings struct */
    for (blk = 0; blk < argc - 1; blk++) {
        /*
         * first seperate the block into matches and value,
         * which are separated by '='
         */
        if ((settings[blk].value = strchr(argv[blk + 1], '=')) == NULL)
            /* no '=' found, whole string must be the value */
            settings[blk].value = argv[blk + 1];
        else {
            /*
             * there is a '=', value+1 points to value string.
             * use strndupa() to copy the matchids into a new buffer.
             */
            settings[blk].matchids =
                strndupa(argv[blk + 1],
                         (size_t)(settings[blk].value++ - argv[blk + 1]));
        }

        /*
         * value points to the value string, possibly with a delay suffix
         * matchids points to the match-ids (possibly multiple) or NULL
         *
         * Now check for a delay suffix (meaning continuous mode), eg 0xff/10
         */
        if ((delay = strchr(settings[blk].value, '/')) != NULL) {
            char *end = NULL;

            /* parse delay count */
            settings[blk].seconds = strtoul(delay + 1, &end, 10);

            if (*(delay + 1) == '\0') {
                /* empty delay count, eg: 12=32/ */
                show_error("you specified an empty delay count, `%s`, "
                           "see `help set`.\n", settings[blk].value);
                return false;
            } else if (*end != '\0') {
                /*
                 * parse failed before end, probably trailing garbage,
                 * e.g. 34=9/16foo
                 */
                show_error("trailing garbage after delay count, `%s`.\n",
                           settings[blk].value);
                return false;
            } else if (settings[blk].seconds == 0) {
                /* 10=24/0 disables continous mode */
                show_info("you specified a zero delay, disabling "
                          "continuous mode.\n");
            } else {
                /* valid delay count seen and understood */
                show_info("setting %s every %u seconds until interrupted...\n",
                          settings[blk].matchids ? settings[blk].matchids
                                                   : "all",
                          settings[blk].seconds);

                /* continuous mode on */
                cont = true;
            }

            /* remove any delay suffix from the value */
            settings[blk].value =
                strndupa(settings[blk].value,
                         (size_t)(delay - settings[blk].value));
        }                       /* if (strchr('/')) */
    }                           /* for(blk...) */

    /* setup a longjmp to handle interrupt */
    if (INTERRUPTABLE()) {
        /* control returns here when interrupted */
        sm_detach(vars->target);
        ENDINTERRUPTABLE();
        return true;
    }

    /* execute the parsed setting structs */
    while (true) {
        uservalue_t userval;

        /* for every settings struct */
        for (blk = 0; blk < argc - 1; blk++) {
            /* check if this block has anything to do this iteration */
            if (seconds != 1)
                /*
                 * not the first iteration
                 * (all blocks get executed first iteration)
                 *
                 *
                 * if settings.seconds is zero, then this block is only
                 * executed once
                 * if seconds % settings.seconds is zero, then this block
                 * must be executed
                 */
                if ( settings[blk].seconds            == 0 ||
                    (seconds % settings[blk].seconds) != 0)
                    continue;

            /* convert value */
            if (!parse_uservalue_number(settings[blk].value, &userval)) {
                show_error("bad number `%s` provided\n", settings[blk].value);
                goto fail;
            }

            /* check if specific match(s) were specified */
            if (settings[blk].matchids != NULL) {
                char    *id, *lmatches = NULL;
                unsigned num = 0;

                /* create local copy of the matchids for strtok() to modify */
                lmatches = strdupa(settings[blk].matchids);

                /* now seperate each match, spearated by commas */
                while ((id = strtok(lmatches, ",")) != NULL) {
                    match_location loc;

                    /* set to NULL for strtok() */
                    lmatches = NULL;

                    /* parse this id */
                    num = strtoul(id, &end, 0x00);

                    /* check that succeeded */
                    if (*id == '\0' || *end != '\0') {
                        show_error("could not parse match id `%s`\n", id);
                        goto fail;
                    }

                    /* check this is a valid match-id */
                    loc = nth_match(vars->matches, num);
                    if (loc.swath) {
                        value_t v;
                        value_t old;
                        void *addr =
                            remote_address_of_nth_element(loc.swath, loc.index
                                                     /* ,MATCHES_AND_VALUES */);

                        /*
                         * copy val onto v
                         * XXX: valcmp? make sure the sizes match
                         */
                        old = data_to_val(loc.swath, loc.index
                                          /* ,MATCHES_AND_VALUES */);
                        zero_value(&v);
                        v.flags = old.flags = loc.swath->data[loc.index]
                                                 .match_info;
                        uservalue2value(&v, &userval);
                        
                        show_info("setting *%p to %#"PRIx64"...\n", addr,
                                  v.int64_value);

                        /* set the value specified */
                        fix_endianness(vars, &v);
                        if (sm_setaddr(vars->target, addr, &v) == false) {
                            show_error("failed to set a value.\n");
                            goto fail;
                        }

                    } else {
                        /* match-id > than number of matches */
                        show_error("found an invalid match-id `%s`\n", id);
                        goto fail;
                    }
                }
            } else {
                matches_and_old_values_swath *reading_swath_idx = 
                    (matches_and_old_values_swath *)vars->matches->swaths;
                int reading_iter = 0;

                /* user wants to set all matches */
                while (reading_swath_idx->first_byte_in_child) {
                    /* Only actual matches are considered */
                    if (flags_to_max_width_in_bytes(reading_swath_idx
                                                     ->data[reading_iter]
                                                       .match_info) > 0) {
                        void *addr = 
                            remote_address_of_nth_element(reading_swath_idx,
                                                          reading_iter
                                                     /* ,MATCHES_AND_VALUES */);

                        /* XXX: as above : make sure the sizes match */
                        value_t old = data_to_val(reading_swath_idx,
                                                  reading_iter
                                              /* ,MATCHES_AND_VALUES */);
                        value_t v;
                        zero_value(&v);
                        v.flags = old.flags = reading_swath_idx
                                               ->data[reading_iter]
                                                 .match_info;
                        uservalue2value(&v, &userval);

                        show_info("setting *%p to %#"PRIx64"...\n", addr,
                                  v.int64_value);

                        fix_endianness(vars, &v);
                        if (sm_setaddr(vars->target, addr, &v) == false) {
                            show_error("failed to set a value.\n");
                            goto fail;
                        }
                    }
                     
                     /* Go on to the next one... */
                    ++reading_iter;
                    if (reading_iter >= reading_swath_idx
                                              ->number_of_bytes) {
                        reading_swath_idx =
                            local_address_beyond_last_element(
                                reading_swath_idx /* ,MATCHES_AND_VALUES */);
                        reading_iter = 0;
                    }
                }
            }                   /* if (matchid != NULL) else ... */
        }                       /* for(blk) */

        if (cont)
            sleep(1);
        else
            break;

        seconds++;
    } /* while(true) */

    ENDINTERRUPTABLE();
    return true;

fail:
    ENDINTERRUPTABLE();
    return false;
    
}

/*
 * XXX: add yesno command to check if matches > 099999
 *
 * FORMAT (don't change, front-end depends on this): 
 * [#no] addr, value, [possible types (separated by space)]
 */
bool handler__list(globals_t *vars, char **argv, unsigned argc)
{
    unsigned   i = 0;
    int        buf_len = 128; /* will be realloc later if necessary */
    element_t *np = NULL;
    char      *v;
    char      *bytearray_suffix = ", [bytearray]";
    char      *string_suffix    = ", [string]";

    if ((v = malloc(buf_len)) == NULL) {
        show_error("memory allocation failed.\n");
        return false;
    }


    USEPARAMS();

    if (!vars->matches)
        goto out_free;

    if (vars->regions)
        np = vars->regions->head;

    matches_and_old_values_swath *reading_swath_idx =
        (matches_and_old_values_swath *)vars->matches->swaths;

    int reading_iter = 0;

    /* list all known matches */
    while (reading_swath_idx->first_byte_in_child) {
        match_flags flags = reading_swath_idx->data[reading_iter]
                                                .match_info;

        /* Only actual matches are considered */
        if (flags_to_max_width_in_bytes(flags) > 0) {
            switch(vars->options.scan_data_type) {
            case BYTEARRAY:
                ; /* cheat gcc */ 
                buf_len = flags.bytearray_length * 3 + 32;
                /* for each byte and the suffix', this should be enough */
                v = realloc(v, buf_len);

                if (v == NULL) {
                    show_error("memory allocation failed.\n");
                    return false;
                }
                data_to_bytearray_text(v, buf_len, reading_swath_idx,
                                       reading_iter,
                                       flags.bytearray_length);
                /* XXX: or maybe realloc is better? */
                assert(strlen(v) + strlen(bytearray_suffix) + 1 <= buf_len);
                strcat(v, bytearray_suffix);

                break;
            case STRING:
                ; /* cheat gcc */
                 /* for the string and suffix, this should be enough */
                buf_len = flags.string_length + strlen(string_suffix) + 32;
                v = realloc(v, buf_len);
                if (v == NULL) {
                    show_error("memory allocation failed.\n");
                    return false;
                }
                data_to_printable_string(v, buf_len, reading_swath_idx,
                                         reading_iter, flags.string_length);
                /* XXX: or maybe realloc is better? */
                assert(strlen(v) + strlen(string_suffix) + 1 <= buf_len);
                strcat(v, string_suffix);

                break;
            default: /* numbers */
                ; /* cheat gcc */
                value_t val = data_to_val(reading_swath_idx, reading_iter
                                      /* ,MATCHES_AND_VALUES */);
                truncval_to_flags(&val, flags);

                valtostr(&val, v, buf_len);

                break;
            }

            void *addr = remote_address_of_nth_element(reading_swath_idx,
                reading_iter /* ,MATCHES_AND_VALUES */);

            unsigned long addr_ul     = (unsigned long)addr;
            int           region_id   = 99;
            unsigned long match_off   = 0;
            const char   *region_type = "??";
            /*
             * get region info belonging to the match -
             * note: we assume the regions list and matches to be sorted
             */
            while (np) {
                region_t     *region       = np->data;
                unsigned long region_start = (unsigned long)region->start;

                if (addr_ul <  region_start + region->size &&
                    addr_ul >= region_start) {
                    region_id = region->id;
                    match_off = addr_ul - region->load_addr;
                    region_type = region_type_names[region->type];
                    break;
                }
                np = np->next;
            }
            fprintf(stdout, "[%2u] "POINTER_FMT", %2u + "POINTER_FMT", %5s, "
                            " %s\n", 
                    i++, addr_ul, region_id, match_off, region_type, v);
        }

        /* Go on to the next one... */
        ++reading_iter;
        if (reading_iter >= reading_swath_idx->number_of_bytes) {
            reading_swath_idx = local_address_beyond_last_element(
                reading_swath_idx /* ,MATCHES_AND_VALUES */);
            reading_iter = 0;
        }
    }

out_free:
    free(v);
    return true;
}

/* XXX: handle multiple deletes, eg delete !1 2 3 4 5 6 */
bool handler__delete(globals_t * vars, char **argv, unsigned argc)
{
    unsigned       id;
    char          *end = NULL;
    match_location loc;

    if (argc != 2) {
        show_error("was expecting one argument, see `help delete`.\n");
        return false;
    }

    /* parse argument */
    id = strtoul(argv[1], &end, 0x00);

    /* check that strtoul() worked */
    if (argv[1][0] == '\0' || *end != '\0') {
        show_error("sorry, couldnt parse `%s`, try `help delete`\n", argv[1]);
        return false;
    }
    
    loc = nth_match(vars->matches, id);
    
    if (loc.swath) {
        /*
         * It is not convenient to check whether anything else relies on this,
         * so just mark it as not a REAL match
         */
        zero_match_flags(&loc.swath->data[loc.index].match_info);
        vars->num_matches--;
        return true;
    } else {
        /* I guess this is not a valid match-id */
        show_warn("you specified a non-existant match `%u`.\n", id);
        show_info("use \"list\" to list matches, or \"help\" for other "
                  "commands.\n");
        return false;
    }
}

bool handler__reset(globals_t * vars, char **argv, unsigned argc)
{
    USEPARAMS();

    if (vars->matches) { 
        free(vars->matches);
        vars->matches = NULL;
        vars->num_matches = 0;
    }

    /* refresh list of regions */
    l_destroy(vars->regions);

    /* create a new linked list of regions */
    if ((vars->regions = l_init()) == NULL) {
        show_error("sorry, there was a problem allocating memory.\n");
        return false;
    }

    /* read in maps if a pid is known */
    if (vars->target && sm_readmaps(vars->target, vars->regions) != true) {
        show_error("sorry, there was a problem getting a list of regions to "
                   "search.\n");
        show_warn("the pid may be invalid, or you don't have permission.\n");
        vars->target = 0;
        return false;
    }

    return true;
}

bool handler__pid(globals_t * vars, char **argv, unsigned argc)
{
    char *resetargv[] = { "reset", NULL };
    char *end         = NULL;

    if (argc == 2) {
        vars->target = (pid_t) strtoul(argv[1], &end, 0x00);

        if (vars->target == 0) {
            show_error("`%s` does not look like a valid pid.\n", argv[1]);
            return false;
        }
    } else if (vars->target) {
        /* print the pid of the target program */
        show_info("target pid is %u.\n", vars->target);
        return true;
    } else {
        show_info("no target is currently set.\n");
        return false;
    }

    return handler__reset(vars, resetargv, 1);
}

bool handler__snapshot(globals_t *vars, char **argv, unsigned argc)
{
    USEPARAMS();
    
    /* check that a pid has been specified */
    if (vars->target == 0) {
        show_error("no target set, type `help pid`.\n");
        return false;
    }

    /* remove any existing matches */
    if (vars->matches) {
        free(vars->matches);
        vars->matches = NULL;
        vars->num_matches = 0;
    }

    if (sm_searchregions(vars, MATCHANY, NULL) != true) {
        show_error("failed to save target address space.\n");
        return false;
    }

    return true;
}

/* dregion [!][x][,x,...] */
bool handler__dregion(globals_t *vars, char **argv, unsigned argc)
{
    unsigned   id;
    bool       invert = false;
    char      *end    = NULL, *idstr = NULL, *block = NULL;
    element_t *np, *pp;
    list_t    *keep   = NULL;
    region_t  *save;

    /* need an argument */
    if (argc < 2) {
        show_error("expected an argument, see `help dregion`.\n");
        return false;
    }

     /* check that there is a process known */
    if (vars->target == 0) {
        show_error("no target specified, see `help pid`\n");
        return false;
    }
    
    /* check for an inverted match */
    if (*argv[1] == '!') {
        invert = true;
        /* create a copy of the argument for strtok(), +1 to skip '!' */
        block = strdupa(argv[1] + 1);
        
        /* check for lone '!' */
        if (*block == '\0') {
            show_error("inverting an empty set, maybe try `reset` instead?\n");
            return false;
        }

        /* create a list to keep the specified regions */
        if ((keep = l_init()) == NULL) {
            show_error("memory allocation error.\n");
            return false;
        }
    } else {
        invert = false;
        block = strdupa(argv[1]);
    }

    /* loop for every number specified, eg "1,2,3,4,5" */
    while ((idstr = strtok(block, ",")) != NULL) {
        region_t *r = NULL;
        
        /* set block to NULL for strtok() */
        block = NULL;
        
        /* attempt to parse as a regionid */
        id = strtoul(idstr, &end, 0x00);

        /* check that worked, "1,abc,4,,5,6foo" */
        if (*end != '\0' || *idstr == '\0') {
            show_error("could not parse argument %s.\n", idstr);
            if (invert)
                if (l_concat(vars->regions, &keep) == -1) {
                    show_error("there was a problem restoring saved "
                               "regions.\n");
                    l_destroy(vars->regions);
                    l_destroy(keep);
                    return false;
                }
            assert(keep == NULL);
            return false;
        }
        
        /* initialise list pointers */
        np = vars->regions->head;
        pp = NULL;
        
        /* find the correct region node */
        while (np) {
            r = np->data;
            
            /* compare the node id to the id the user specified */
            if (r->id == id)
                break;
            
            pp = np; /* keep track of prev for l_remove() */
            np = np->next;
        }

        /* check if a match was found */
        if (np == NULL) {
            show_error("no region matching %u, or already moved.\n", id);

            if (invert)
                if (l_concat(vars->regions, &keep) == -1) {
                    show_error("there was a problem restoring saved "
                               "regions.\n");
                    l_destroy(vars->regions);
                    l_destroy(keep);
                    return false;
                }
            if (keep)
                l_destroy(keep);

            return false;
        }
        
        np = pp;
        
        /* save this region if the match is inverted */
        if (invert) {
            assert(keep != NULL);
            
            l_remove(vars->regions, np, (void *) &save);
            if (l_append(keep, keep->tail, save) == -1) {
                show_error("sorry, there was an internal memory error.\n");
                free(save);
                l_destroy(keep);
                return false;
            }

            continue;
        }
        
        /* check for any affected matches before removing it */
        if(vars->num_matches > 0) {
            region_t *s;

            /* determine the correct pointer we're supposed to be checking */
            if (np) {
                assert(np->next);
                s = np->next->data;
            } else {
                /* head of list */
                s = vars->regions->head->data;
            }
            
            if (!(vars->matches = delete_by_region(vars->matches,
                                                   &vars->num_matches, s,
                                                   false)))
                show_error("memory allocation error while deleting matches\n");
        }

        l_remove(vars->regions, np, NULL);
    }

    if (invert) {
        region_t *s = keep->head->data;

        if (vars->num_matches > 0)
            if (!(vars->matches = delete_by_region(vars->matches,
                                                   &vars->num_matches, s,
                                                   true)))
                show_error("memory allocation error while deleting matches\n");
            
        /* okay, done with the regions list */
        l_destroy(vars->regions);
        
        /* and switch to the keep list */
        vars->regions = keep;
    }

    return true;
}

bool handler__lregions(globals_t * vars, char **argv, unsigned argc)
{
    element_t *np = vars->regions->head;

    USEPARAMS();

    if (vars->target == 0) {
        show_error("no target has been specified, see `help pid`.\n");
        return false;
    }

    if (vars->regions->size == 0)
        show_info("no regions are known.\n");
    
    /* print a list of regions that are searched */
    while (np) {
        region_t *region = np->data;

        fprintf(stderr, "[%2u] "POINTER_FMT", %7lu bytes, %5s, "POINTER_FMT
                        ", %c%c%c, %s\n", 
                region->id,
                (unsigned long)region->start, region->size,
                region_type_names[region->type], region->load_addr,
                region->flags.read  ? 'r' : '-',
                region->flags.write ? 'w' : '-',
                region->flags.exec  ? 'x' : '-',
                region->filename[0] ? region->filename : "unassociated");
        np = np->next;
    }

    return true;
}

/*
 * the name of this function is for historical reasons.
 * now GREATERTHAN & LESSTHAN are also handled by this function
 */
bool handler__decinc(globals_t * vars, char **argv, unsigned argc)
{
    uservalue_t       val;
    scan_match_type_t m;

    USEPARAMS();

    if (argc == 1) {
        zero_uservalue(&val);
    } else if (argc > 2) {
        show_error("too many values specified, see `help %s`", argv[0]);
        return false;
    } else {
        if (!parse_uservalue_number(argv[1], &val)) {
            show_error("bad value specified, see `help %s`", argv[0]);
            return false;
        }
    }

    if (strcmp(argv[0], "=") == 0) {
        m = (argc == 1) ? MATCHNOTCHANGED : MATCHEQUALTO;
    } else if (strcmp(argv[0], "!=") == 0) {
        m = (argc == 1) ? MATCHCHANGED : MATCHNOTEQUALTO;
    } else if (strcmp(argv[0], "<") == 0) {
        m = (argc == 1) ? MATCHDECREASED : MATCHLESSTHAN;
    } else if (strcmp(argv[0], ">") == 0) {
        m = (argc == 1) ? MATCHINCREASED : MATCHGREATERTHAN;
    } else if (strcmp(argv[0], "+") == 0) {
        m = (argc == 1) ? MATCHINCREASED : MATCHINCREASEDBY;
    } else if (strcmp(argv[0], "-") == 0) {
        m = (argc == 1) ? MATCHDECREASED : MATCHDECREASEDBY;
    } else {
        show_error("unrecognized match type seen at decinc handler.\n");
        return false;
    }

    if (vars->matches) {
        if (sm_checkmatches(vars, m, &val) == false) {
            show_error("failed to search target address space.\n");
            return false;
        }
    } else {
        /*
         * Cannot be used on first scan:
         *   =, !=, <, >, +, + N, -, - N
         * Can be used on first scan:
         *   = N, != N, < N, > N
         */
        if (m == MATCHNOTCHANGED  ||
            m == MATCHCHANGED     ||
            m == MATCHDECREASED   ||
            m == MATCHINCREASED   ||
            m == MATCHDECREASEDBY ||
            m == MATCHINCREASEDBY) {
            show_error("cannot use that search without matches\n");
            return false;
        } else {
            if (sm_searchregions(vars, m, &val) != true) {
                show_error("failed to search target address space.\n");
                return false;
            }
        }
    }

    if (vars->num_matches == 1) {
        show_info("match identified, use \"set\" to modify value.\n");
        show_info("enter \"help\" for other commands.\n");
    }

    return true;
}

bool handler__version(globals_t *vars, char **argv, unsigned argc)
{
    USEPARAMS();

    vars->printversion(stderr);

    return true;
}

bool handler__string(globals_t * vars, char **argv, unsigned argc)
{
    int         i;
    uservalue_t val;

    /* test scan_data_type */
    if (vars->options.scan_data_type != STRING) {
        show_error("scan_data_type is not string, see `help option`.\n");
        return false;
    }

    /* test the length */
    for (i = 0; (i < 3) && vars->current_cmdline[i]; ++i)
        ;

    if (i != 3) { /* cmdline too short */
        show_error("please specify a string\n");
        return false;
    }
 
    /* the string being scanned */
    val.string_value        = vars->current_cmdline+2;
    val.flags.string_length = strlen(val.string_value);
 
    /* need a pid for the rest of this to work */
    if (vars->target == 0)
        return false;

    /* user has specified an exact value of the variable to find */
    if (vars->matches) {
        /* already know some matches */
        if (sm_checkmatches(vars, MATCHEQUALTO, &val) != true) {
            show_error("failed to search target address space.\n");
            return false;
        }
    } else {
        /* initial search */
        if (sm_searchregions(vars, MATCHEQUALTO, &val) != true) {
            show_error("failed to search target address space.\n");
            return false;
        }
    }

    /* check if we now know the only possible candidate */
    if (vars->num_matches == 1) {
        show_info("match identified, use \"set\" to modify value.\n");
        show_info("enter \"help\" for other commands.\n");
    }

    return true;
}

static inline bool parse_uservalue_default(char *str, uservalue_t *val)
{
    bool ret = true;

    if (!parse_uservalue_number(str, val)) {
        show_error("unable to parse number `%s`\n", str);
        ret = false;
    }

    return ret;
}

bool handler__default(globals_t * vars, char **argv, unsigned argc)
{
    uservalue_t          vals[2];
    uservalue_t         *val   = &vals[0];
    bytearray_element_t *arr   = NULL;
    scan_match_type_t    m     = MATCHEQUALTO;
    char                *ustr  = argv[0];
    char                *pos;
    bool                 ret = false;

    USEPARAMS();

    switch(vars->options.scan_data_type) {
    case ANYNUMBER:
    case ANYINTEGER:
    case ANYFLOAT:
    case INTEGER8:
    case INTEGER16:
    case INTEGER32:
    case INTEGER64:
    case FLOAT32:
    case FLOAT64:
        /* attempt to parse command as a number */
        if (argc != 1) {
            show_error("unknown command\n");
            goto retl;
        }
	    /* detect a range */
	    if ((pos = strstr(ustr, ".."))) {
            *pos = '\0';
            if (!parse_uservalue_default(ustr, &vals[0]))
                goto retl;
            ustr = pos + 2;
            if (!parse_uservalue_default(ustr, &vals[1]))
                goto retl;
            m = MATCHRANGE;
            } else {
                if (!parse_uservalue_default(ustr, val))
                    goto retl;
            }

            break;
    case BYTEARRAY:
        /* attempt to parse command as a bytearray */
        arr = calloc(argc, sizeof(bytearray_element_t));
    
        if (arr == NULL) {
            show_error("there's a memory allocation error.\n");
            goto retl;
        }
        if (!parse_uservalue_bytearray(argv, argc, arr, val)) {
            show_error("unable to parse command `%s`\n", ustr);
            goto retl;
        }

        break;
    case STRING:
        show_error("unable to parse command `%s`\nIf you want to scan "
                   "for a string, use command `\"`.\n", ustr);
        goto retl;

        break;
    default:
        assert(false);
        /* NORETURN */
    }

    /* need a pid for the rest of this to work */
    if (vars->target == 0)
        goto retl;

    /* user has specified an exact value of the variable to find */
    if (vars->matches) {
        /* already know some matches */
        if (sm_checkmatches(vars, m, val) != true) {
            show_error("failed to search target address space.\n");
            goto retl;
        }
    } else {
        /* initial search */
        if (sm_searchregions(vars, m, val) != true) {
            show_error("failed to search target address space.\n");
            goto retl;
        }
    }

    /* check if we now know the only possible candidate */
    if (vars->num_matches == 1) {
        show_info("match identified, use \"set\" to modify value.\n");
        show_info("enter \"help\" for other commands.\n");
    }

    ret = true;

retl:
    if (arr)
        free(arr);

    return ret;
}

bool handler__update(globals_t *vars, char **argv, unsigned argc)
{
    USEPARAMS();

    if (vars->matches) {
        if (sm_checkmatches(vars, MATCHANY, NULL) == false) {
            show_error("failed to scan target address space.\n");
            return false;
        }
    } else {
        show_error("cannot use that command without matches\n");
        return false;
    }

    return true;
}

bool handler__exit(globals_t *vars, char **argv, unsigned argc)
{
    USEPARAMS();

    vars->exit = 1;

    return true;
}

/* which column descriptions start on with help command */
#define DOC_COLUMN 11

bool handler__help(globals_t *vars, char **argv, unsigned argc)
{
    bool       ret  = false;
    list_t    *cmds = vars->commands;
    element_t *np   = NULL;
    command_t *def  = NULL;

    assert(cmds != NULL);
    assert(argc >= 1);

    np = cmds->head;

    /* pager support, dirty ugly hardcoded */
    FILE *outfd = popen("more", "w"); 
    if (outfd == NULL) {
        show_warn("Cannot execute pager, fall back to normal output\n");
        outfd = stderr;
    }

    /* print version information for generic help */
    if (argv[1] == NULL) {
        vars->printversion(outfd);
        fprintf(outfd, "\n");
    }

    /* traverse the commands list, printing out the relevant documentation */
    while (np) {
        command_t *cmd = np->data;

        /* remember the default command */
        if (cmd->command == NULL)
            def = cmd;

        /* just `help` with no argument */
        if (argv[1] == NULL) {
            /* NULL shortdoc means dont print in help listing */
            if (cmd->shortdoc == NULL) {
                np = np->next;
                continue;
            }

            /* print out command name */
            fprintf(outfd, "%-*s%s\n", DOC_COLUMN, cmd->command ? cmd->command
                                                                : "default",
                    cmd->shortdoc);
        } else if (cmd->command &&
                   !strcasecmp(argv[1], cmd->command)) {
            /* detailed information requested about specific command */
            fprintf(outfd, "%s\n", cmd->longdoc ? cmd->longdoc
                                                : "missing documentation");
            ret = true;
            goto retl;
        }

        np = np->next;
    }

    if (argc > 1) {
        show_error("unknown command `%s`\n", argv[1]);
        ret = false;
    } else if (def) {
        fprintf(outfd, "\n%s\n", def->longdoc ? def->longdoc : "");
        ret = true;
    }
    
    ret = true;

retl:
    if (outfd != stderr)
        pclose(outfd);

    return ret;
}

bool handler__eof(globals_t * vars, char **argv, unsigned argc)
{
    show_user("exit\n");

    return handler__exit(vars, argv, argc);
}

/* XXX: handle !ls style escapes */
bool handler__shell(globals_t * vars, char **argv, unsigned argc)
{
    size_t   len = argc;
    unsigned i;
    char    *cmd;

    USEPARAMS();

    if (argc < 2) {
        show_error("shell command requires an argument, see `help shell`.\n");
        return false;
    }

    /* convert arg vector into single string, first calculate length */
    for (i = 1; i < argc; i++)
        len += strlen(argv[i]);

    /* allocate space */
    cmd = calloca(len, 1);

    /* concatenate strings */
    for (i = 1; i < argc; i++) {
        strcat(cmd, argv[i]);
        strcat(cmd, " ");
    }

    /* finally execute command */
    if (system(cmd) == -1) {
        show_error("system() failed, command was not executed.\n");
        return false;
    }

    return true;
}

bool handler__watch(globals_t * vars, char **argv, unsigned argc)
{
    value_t          o, n;
    unsigned         id;
    char            *end = NULL, buf[128], timestamp[64];
    time_t           t;
    match_location   loc;
    value_t          old_val;
    void            *addr;
    scan_data_type_t data_type = vars->options.scan_data_type;

    if (argc != 2) {
        show_error("was expecting one argument, see `help watch`.\n");
        return false;
    }

    if ((data_type == BYTEARRAY) || (data_type == STRING)) {
        show_error("`watch` is not supported for bytearray or string.\n");
        return false;
    }

    /* parse argument */
    id = strtoul(argv[1], &end, 0x00);

    /* check that strtoul() worked */
    if (argv[1][0] == '\0' || *end != '\0') {
        show_error("sorry, couldn't parse `%s`, try `help watch`\n", argv[1]);
        return false;
    }
    
    loc = nth_match(vars->matches, id);

    /* check this is a valid match-id */
    if (!loc.swath) {
        show_error("you specified a non-existent match `%u`.\n", id);
        show_info("use \"list\" to list matches, or \"help\" for "
                  "other commands.\n");
        return false;
    }
    
    addr = remote_address_of_nth_element(loc.swath, loc.index
                                         /* ,MATCHES_AND_VALUES */);
    old_val = data_to_val(loc.swath, loc.index /* ,MATCHES_AND_VALUES */);
    old_val.flags = loc.swath->data[loc.index].match_info;
    valcpy(&o, &old_val);
    valcpy(&n, &o);

    valtostr(&o, buf, sizeof(buf));

    if (INTERRUPTABLE()) {
        (void) sm_detach(vars->target);
        ENDINTERRUPTABLE();
        return true;
    }

    /* every entry is timestamped */
    t = time(NULL);
    strftime(timestamp, sizeof(timestamp), "[%T]", localtime(&t));

    show_info("%s monitoring %10p for changes until interrupted...\n",
              timestamp, addr);

    while (true) {
        if (sm_attach(vars->target) == false)
            return false;

        if (sm_peekdata(vars->target, addr, &n) == false)
            return false;

        truncval(&n, &old_val);

        /* check if the new value is different */
        match_flags tmpflags;
        zero_match_flags(&tmpflags);
        scan_routine_t valcmp_routine =
            (sm_get_scanroutine(ANYNUMBER, MATCHCHANGED));

        if (valcmp_routine(&o, &n, NULL, &tmpflags, addr)) {
            valcpy(&o, &n);
            truncval(&o, &old_val);

            valtostr(&o, buf, sizeof(buf));

            /* fetch new timestamp */
            t = time(NULL);
            strftime(timestamp, sizeof(timestamp), "[%T]", localtime(&t));

            show_info("%s %10p -> %s\n", timestamp, addr, buf);
        }

        /*
         * detach after valuecmp_routine, since it may read more data
         * (e.g. bytearray)
         */
        sm_detach(vars->target);

        (void) sleep(1);
    }
}

#include "licence.h"

bool handler__show(globals_t * vars, char **argv, unsigned argc)
{
    USEPARAMS();
    
    if (argv[1] == NULL) {
        show_error("expecting an argument.\n");
        return false;
    }
    
    if (!strcmp(argv[1], "copying"))
        show_user(SM_COPYING);
    else if (!strcmp(argv[1], "warranty"))
        show_user(SM_WARRANTY);
    else if (!strcmp(argv[1], "version"))
        vars->printversion(stderr);
    else {
        show_error("unrecognized show command `%s`\n", argv[1]);
        return false;
    }
    
    return true;
}

bool handler__dump(globals_t * vars, char **argv, unsigned argc)
{
    void *addr;
    char *endptr;
    char *buf = NULL;
    int   len;
    bool  dump_to_file = false;
    FILE *dump_f = NULL;

    if (argc < 3 || argc > 4) {
        show_error("bad argument, see `help dump`.\n");
        return false;
    }
    
    /* check address */
    errno = 0;
    addr = (void *)(strtoll(argv[1], &endptr, 16));
    if ((errno != 0) || (*endptr != '\0')) {
        show_error("bad address, see `help dump`.\n");
        return false;
    }

    /* check length */
    errno = 0;
    len = strtoll(argv[2], &endptr, 0);
    if ((errno != 0) || (*endptr != '\0')) {
        show_error("bad length, see `help dump`.\n");
        return false;
    }

    /* check filename */
    if (argc == 4) {
        if ((dump_f = fopen(argv[3], "wb")) == NULL) {
            show_error("failed to open file\n");
            return false;
        }

        dump_to_file = true;
    }

    if ((buf = malloc(len + sizeof(long))) == NULL) {
        if (dump_f)
            fclose(dump_f);
        show_error("memory allocation failed.\n");
        return false;
    }

    if (!sm_read_array(vars->target, addr, buf, len)) {
        if (dump_f)
            fclose(dump_f);
        show_error("read memory failed.\n");
        free(buf);
        return false;
    }

    if (dump_to_file) {
        size_t s = fwrite(buf,1,len,dump_f);

        fclose(dump_f);
        if (s != len) {
            show_error("write to file failed.\n");
            free(buf);
            return false;
        }  
    } else {
        /* print it out */
        int i, j;
        int buf_idx = 0;

        for (i = 0; i + 16 < len; i += 16) {
            if (vars->options.backend == 0)
                printf("%p: ", addr+i);

            for (j = 0; j < 16; ++j)
                printf("%02X ", (unsigned char)(buf[buf_idx++]));

            if(vars->options.dump_with_ascii)
                for (j = 0; j < 16; ++j) {
                    char c = buf[i+j];
                    printf("%c", isprint(c) ? c : '.');
                }
            printf("\n");
        }

        if (i < len) {
            if (vars->options.backend == 0)
                printf("%p: ", addr+i);

            for (j = i; j < len; ++j)
                printf("%02X ", (unsigned char)(buf[buf_idx++]));

            if (vars->options.dump_with_ascii) {
                while (j%16 != 0) { /* skip "empty" numbers */
                    printf("   ");
                    ++j;
                }
                for (j = 0; i+j < len; ++j) {
                    char c = buf[i+j];
                    printf("%c", isprint(c) ? c : '.');
                }
            }
            printf("\n");
        }
    }

    free(buf);
    return true;
}

bool handler__write(globals_t * vars, char **argv, unsigned argc)
{
    int         data_width = 0;
    const char *fmt = NULL;
    void       *addr;
    char       *buf = NULL;
    char       *endptr;
    int         datatype; /* 0 for numbers, 1 for bytearray, 2 for string */
    bool        ret;
    const char *string_param = NULL; /* used by string type */

    if (argc < 4) {
        show_error("bad arguments, see `help write`.\n");
        ret = false;
        goto retl;
    }

    /* try int first */
    if (!strcasecmp(argv[1], "i8"  ) ||
        !strcasecmp(argv[1], "int8")) {
        data_width = 1;
        datatype = 0;
        fmt = "%"PRId8;
    } else if (!strcasecmp(argv[1], "i16"  ) ||
               !strcasecmp(argv[1], "int16")) {
        data_width = 2;
        datatype = 0;
        fmt = "%"PRId16;
    } else if (!strcasecmp(argv[1], "i32"  ) ||
               !strcasecmp(argv[1], "int32")) {
        data_width = 4;
        datatype = 0;
        fmt = "%"PRId32;
    } else if (!strcasecmp(argv[1], "i64"  ) ||
               !strcasecmp(argv[1], "int64")) {
        data_width = 8;
        datatype = 0;
        fmt = "%"PRId64;
    } else if (!strcasecmp(argv[1], "f32"    ) ||
               !strcasecmp(argv[1], "float32")) {
        data_width = 4;
        datatype = 0;
        fmt = "%f";
    } else if (!strcasecmp(argv[1], "f64"    ) ||
               !strcasecmp(argv[1], "float64")) {
        data_width = 8;
        datatype = 0;
        fmt = "%lf";
    } else if (!strcasecmp(argv[1], "bytearray")) {
        data_width = argc - 3;
        datatype = 1;
    }
    else if (!strcasecmp(argv[1], "string")) {
        /*
         * locate the string parameter,
         * say locate the beginning of the 4th parameter
         * (2 characters after the end of the 3rd paramter)
         */
        int i;
        string_param = vars->current_cmdline;

        for (i = 0; i < 3; i++) {
            while ((string_param[0] == ' ' ) &&
                   (string_param[0] == '\t'))
                ++string_param;
            while ((string_param[0] != ' ') &&
                   (string_param[0] != '\t'))
                ++string_param;
        }
        ++string_param;
        data_width = strlen(string_param);
        datatype = 2;
    } else { /* may support more types here later */
        show_error("bad data_type, see `help write`.\n");
        ret = false;
        goto retl;
    }

    /* check argc again */
    if ((datatype == 0) && (argc != 4)) {
        show_error("bad arguments, see `help write`.\n");
        ret = false;
        goto retl;
    }

    /* check address */
    errno = 0;
    addr = (void *)strtoll(argv[2], &endptr, 16);
    if ((errno != 0) || (*endptr != '\0')) {
        show_error("bad address, see `help write`.\n");
        ret = false;
        goto retl;
    }

    /* allocate a little bit more, just in case */
    if ((buf = malloc(data_width + 8)) == NULL) {
        show_error("memory allocation failed.\n");
        ret = false;
        goto retl;
    }

    /* load value into buffer */
    switch(datatype) {
    case 0: /* numbers */
         /* should be OK even for max uint64 */
        if (sscanf(argv[3], fmt, buf) < 1) {
            show_error("bad value, see `help write`.\n");
            ret = false;
            goto retl;
        }
        if (1 < data_width && vars->options.reverse_endianness)
            swap_bytes_var(buf, data_width);

        break;
    case 1: // bytearray
        ; /* cheat gcc */
        /* call parse_uservalue_bytearray */
        bytearray_element_t *arr = calloc(data_width,
                                            sizeof(bytearray_element_t));
        uservalue_t val_buf;

        if (arr == NULL) {
            show_error("memory allocation failed.\n");
            ret = false;
            goto retl;
        }
        if (!parse_uservalue_bytearray(argv+3, argc-3, arr, &val_buf)) {
            show_error("bad byte array speicified.\n");
            free(arr);
            ret = false;
            goto retl;
        }

        /*
         * if wildcard is provided in the bytearray, we need the
         * original data.
         */
        bool wildcard_used = false;
        int i;

        for (i = 0; i < data_width; ++i) {
            if (arr[i].is_wildcard) {
                wildcard_used = true;
                break;
            }
        }

        if (wildcard_used)
            if (!sm_read_array(vars->target, addr, buf, data_width)) {
                show_error("read memory failed.\n");
                free(arr);
                ret = false;
                goto retl;
            }

        for(i = 0; i < data_width; ++i) {
            bytearray_element_t *cur_elem = arr + i;

            if (cur_elem->is_wildcard == 0)
                buf[i] = cur_elem->byte;
        }
        free(array);

        break;
    case 2: /* string */
        strncpy(buf, string_param, data_width);

        break;
    default:
        assert(false);
    }

    /* write into memory */
    ret = sm_write_array(vars->target, addr, buf, data_width);

retl:
    if(buf)
        free(buf);
    return ret;
}

bool handler__option(globals_t * vars, char **argv, unsigned argc)
{
    /* this might need to change */
    if (argc != 3) {
        show_error("bad arguments, see `help option`.\n");
        return false;
    }

    if (!strcasecmp(argv[1], "scan_data_type")) {
        if (!strcasecmp(argv[2], "number"))
            vars->options.scan_data_type = ANYNUMBER;
        else if (!strcasecmp(argv[2], "int"))
            vars->options.scan_data_type = ANYINTEGER;
        else if (!strcasecmp(argv[2], "int8"))
            vars->options.scan_data_type = INTEGER8;
        else if (!strcasecmp(argv[2], "int16"))
            vars->options.scan_data_type = INTEGER16;
        else if (!strcasecmp(argv[2], "int32"))
            vars->options.scan_data_type = INTEGER32;
        else if (!strcasecmp(argv[2], "int64"))
            vars->options.scan_data_type = INTEGER64;
        else if (!strcasecmp(argv[2], "float"))
            vars->options.scan_data_type = ANYFLOAT;
        else if (!strcasecmp(argv[2], "float32"))
            vars->options.scan_data_type = FLOAT32;
        else if (!strcasecmp(argv[2], "float64"))
            vars->options.scan_data_type = FLOAT64;
        else if (!strcasecmp(argv[2], "bytearray"))
            vars->options.scan_data_type = BYTEARRAY;
        else if (!strcasecmp(argv[2], "string"))
            vars->options.scan_data_type = STRING;
        else {
            show_error("bad value for scan_data_type, see `help option`.\n");
            return false;
        }
    } else if (!strcasecmp(argv[1], "region_scan_level")) {
        if (!strcmp(argv[2], "1"))
            vars->options.region_scan_level = REGION_HEAP_STACK_EXECUTABLE;
        else if (!strcmp(argv[2], "2"))
            vars->options.region_scan_level = REGION_HEAP_STACK_EXECUTABLE_BSS;
        else if (!strcmp(argv[2], "3"))
            vars->options.region_scan_level = REGION_ALL;
        else {
            show_error("bad value for region_scan_level, see `help option`.\n");
            return false;
        }
    } else if (!strcasecmp(argv[1], "detect_reverse_change")) {
        if (!strcmp(argv[2], "0"))
            vars->options.detect_reverse_change = 0;
        else if (!strcmp(argv[2], "1"))
            vars->options.detect_reverse_change = 1;
        else {
            show_error("bad value for detect_reverse_change, "
                       "see `help option`.\n");
            return false;
        }
    } else if (!strcasecmp(argv[1], "dump_with_ascii")) {
        if (!strcmp(argv[2], "0"))
            vars->options.dump_with_ascii = 0;
        else if (!strcmp(argv[2], "1"))
            vars->options.dump_with_ascii = 1;
        else {
            show_error("bad value for dump_with_ascii, see `help option`.\n");
            return false;
        }
    } else if (!strcasecmp(argv[1], "endianness")) {
        /* data is host endian: don't swap */
        if (!strcmp(argv[2], "0"))
            vars->options.reverse_endianness = 0;
        /* data is little endian: swap if host is big endian */
        else if (!strcmp(argv[2], "1"))
            vars->options.reverse_endianness = big_endian;
        /* data is big endian: swap if host is little endian */
        else if (!strcmp(argv[2], "2"))
            vars->options.reverse_endianness = !big_endian;
        else {
            show_error("bad value for endianness, see `help option`.\n");
            return false;
        }
    } else {
        show_error("unknown option specified, see `help option`.\n");
        return false;
    }

    return true;
}
