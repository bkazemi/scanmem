/*
 *  The target memory information array (storage of matches).
 *
 *  Copyright (C) 2009 Eli Dupree  <elidupree(a)charter.net>
 *  Copyright (C) 2010 WANG Lu  <coolwanglu(a)gmail.com>
 *  Copyright (C) 2015 Sebastian Parschauer <s.parschauer@gmx.de>
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

#ifndef TARGETMEM_H
#define TARGETMEM_H

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <inttypes.h>

#include "value.h"
#include "maps.h"
#include "show_message.h"


/* public structs */
typedef struct {
    uint8_t old_value;
    match_flags match_info;
} old_value_and_match_info;

/*
 * These three structs are not used; pointers to them are used to refer
 * to arrays containing copied_data_swaths / matches_swaths, in the
 * following format:
 *  - the array begins with the first_byte_in_child pointer; immediately after
 *    that is the number_of_bytes, and then the struct's data describing that
 *    many bytes. (Note that the number refers to the number of bytes in the
 *                 child process's memory that are covered, not the number of
 *                 bytes the struct takes up.)
 *  - in the first position after each such block is another such block
 *    (first byte pointer and number of bytes), or a null pointer and a 0
 *     number of bytes to terminate the data.
 *    (The first_byte_in_child pointers refer to locations in the child;
 *     they cannot be followed except using ptrace())
 */

/*
typedef struct {
    void *first_byte_in_child;
    unsigned long number_of_bytes;
} unknown_type_of_swath;

typedef struct {
    void *first_byte_in_child;
    unsigned long number_of_bytes;
    uint8_t copied_bytes[0];
} copied_data_swath;

typedef struct {
    void *first_byte_in_child;
    unsigned long number_of_bytes;
    match_flags match_info[0];
} matches_swath;
*/

typedef struct {
    void *first_byte_in_child;
    unsigned long number_of_bytes;
    old_value_and_match_info data[0];
} matches_and_old_values_swath;

/*
typedef struct {
    unsigned long bytes_allocated;
    unsigned long max_needed_bytes;
} unknown_type_of_array;

typedef struct {
    unsigned long bytes_allocated;
    unsigned long max_needed_bytes;
    copied_data_swath swaths[0];
} copied_data_array;

typedef struct {
    unsigned long bytes_allocated;
    unsigned long max_needed_bytes;
    matches_swath swaths[0];
} matches_array;
*/

typedef struct {
    unsigned long bytes_allocated;
    unsigned long max_needed_bytes;
    matches_and_old_values_swath swaths[0];
} matches_and_old_values_array;

typedef struct {
    matches_and_old_values_swath *swath;
    long index;
} match_location;


/* public functions */
matches_and_old_values_array *allocate_array(matches_and_old_values_array *,
                                             unsigned long);

matches_and_old_values_array *null_terminate(matches_and_old_values_array *,
                                              matches_and_old_values_swath *);

/* for printable text representation */
void data_to_printable_string(char *, int, matches_and_old_values_swath *,
                               long, int);

/* for bytearray representation */
void data_to_bytearray_text(char *, int, matches_and_old_values_swath *,
                             long, int);

match_location nth_match(matches_and_old_values_array *, unsigned);

matches_and_old_values_array *delete_by_region (matches_and_old_values_array *,
                                                long *, region_t *, bool);

static inline long
index_of_last_element(matches_and_old_values_swath *swath)
{
    return swath->number_of_bytes - 1;
}

static inline void *
remote_address_of_nth_element(matches_and_old_values_swath *swath, long n)
{
    return swath->first_byte_in_child + n;
}

static inline void *
remote_address_of_last_element(matches_and_old_values_swath *swath)
{
    return (remote_address_of_nth_element(swath, index_of_last_element(swath)));
}

static inline void *
local_address_beyond_nth_element(matches_and_old_values_swath *swath, long n)
{
    return &((matches_and_old_values_swath *)swath)->data[n + 1];
}

static inline void *
local_address_beyond_last_element(matches_and_old_values_swath *swath)
{
    return (local_address_beyond_nth_element(swath,
                                             index_of_last_element(swath)));
}

static inline matches_and_old_values_array *
allocate_enough_to_reach(matches_and_old_values_array *arr,
                         void *last_byte_to_reach_plus_one,
                         matches_and_old_values_swath **swath_ptr_to_correct)
{
    unsigned long bytes_needed = (last_byte_to_reach_plus_one - (void *)arr);

    if (bytes_needed <= arr->bytes_allocated) {
        return arr;
    } else {
        matches_and_old_values_array *orig_location = arr;

        /*
         * Allocate twice as much each time
         * so we don't have to do it too often
         */
        unsigned long bytes_to_alloc = arr->bytes_allocated;
        while (bytes_to_alloc < bytes_needed)
            bytes_to_alloc *= 2;

        show_debug("to_allocate %ld, max %ld\n", bytes_to_alloc,
                   arr->max_needed_bytes);

        /* Sometimes we know an absolute max that we will need */
        if (arr->max_needed_bytes < bytes_to_alloc) {
            assert(arr->max_needed_bytes >= bytes_needed);
            bytes_to_alloc = arr->max_needed_bytes;
        }

        if (!(arr = realloc(arr, bytes_to_alloc)))
            return NULL;

        arr->bytes_allocated = bytes_to_alloc;

        /*
         * Put the swath pointer back where it should be, if needed.
         * We cast everything to void pointers in this line to make
         * sure the math works out.
         */
        if (swath_ptr_to_correct)
            (*swath_ptr_to_correct) = (matches_and_old_values_swath *)
                (((void *)(*swath_ptr_to_correct)) +
                 ((void *)arr - (void *)orig_location));

        return arr;
    }
}

/*
 * Returns a pointer to the swath to which the element was added
 * - i.e. the last swath in the array after the operation
 */
static inline matches_and_old_values_swath *
add_element(matches_and_old_values_array **arr,
             matches_and_old_values_swath *swath,
             void *remote_addr, void *new_elem)
{
    if (swath->number_of_bytes == 0) {
        assert(swath->first_byte_in_child == NULL);

        /* We have to overwrite this as a new swath */
        *arr = allocate_enough_to_reach(*arr, (void *)swath +
            sizeof(matches_and_old_values_swath) +
            sizeof(old_value_and_match_info), &swath);

        swath->first_byte_in_child = remote_addr;

    } else {
        unsigned long local_idx_excess =
            remote_addr - remote_address_of_last_element(swath);
        unsigned long local_addr_excess =
            local_idx_excess * sizeof(old_value_and_match_info);
        size_t needed_sz =
            sizeof(matches_and_old_values_swath) +
            sizeof(old_value_and_match_info);

        if (local_addr_excess >= needed_size) {
            /* It is most memory-efficient to start a new swath */
            *arr = allocate_enough_to_reach(*arr,
                local_address_beyond_last_element(swath) +
                sizeof(matches_and_old_values_swath) +
                sizeof(old_value_and_match_info), &swath);

            swath = local_address_beyond_last_element(swath);
            swath->first_byte_in_child = remote_addr;
            swath->number_of_bytes = 0;

        } else {
            /*
             * It is most memory-efficient to write over the intervening
             * space with null values
             */
            *arr = allocate_enough_to_reach(*arr,
                local_address_beyond_last_element(swath) +
                local_addr_excess, &swath);

            switch (local_addr_excess) {
            case 4:
                memset(local_address_beyond_last_element(swath), 0, 4);
                break;
            case 8:
                memset(local_address_beyond_last_element(swath), 0, 8);
                break;
            default:
                /* slow due to unknown size to be zeroed */
                memset(local_address_beyond_last_element(swath), 0,
                       local_address_excess);
                break;
            }
            swath->number_of_bytes += local_idx_excess - 1;
        }
    }

    /* Add me */
    *(old_value_and_match_info *)local_address_beyond_last_element(swath) =
        *(old_value_and_match_info *)new_elem;

    ++swath->number_of_bytes;

    return swath;
}

/*
 * only at most sizeof(int64_t) bytes will be read.
 * if more bytes are needed (e.g. bytearray),
 * read them separately (for performance)
 */
static inline value_t
data_to_val_aux (matches_and_old_values_swath *swath,
                 long idx, long swath_len)
{
    int i;
    value_t val;
    int max_bytes = swath_len - idx;

    zero_value(&val);

    if (max_bytes >  8) max_bytes = 8;
    if (max_bytes >= 8) val.flags.u64b = val.flags.s64b = val.flags.f64b = 1;
    if (max_bytes >= 4) val.flags.u32b = val.flags.s32b = val.flags.f32b = 1;
    if (max_bytes >= 2) val.flags.u16b = val.flags.s16b                  = 1;
    if (max_bytes >= 1) val.flags.u8b  = val.flags.s8b                   = 1;

    for (i = 0; i < max_bytes; i++) {
        uint8_t byte;

        byte = ((matches_and_old_values_swath *)swath)->data[idx + i]
                                                             .old_value;
        *((uint8_t *)(&val.int64_value) + i) = byte;
    }

    return val;
}

static inline value_t
data_to_val (matches_and_old_values_swath *swath, long idx)
{
    return data_to_val_aux(swath, idx, swath->number_of_bytes);
}

#endif /* TARGETMEM_H */
