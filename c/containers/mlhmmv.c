// ================================================================
// Array-only (open addressing) multi-level hash map, with linear probing for collisions.
// All keys, and terminal-level values, are mlrvals.
//
// Notes:
// * null key is not supported.
// * null value is not supported.
//
// See also:
// * http://en.wikipedia.org/wiki/Hash_table
// * http://docs.oracle.com/javase/6/docs/api/java/util/Map.html
// ================================================================

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lib/mlr_globals.h"
#include "lib/mlrutil.h"
#include "containers/mlhmmv.h"

// xxx temp static mlhmmv_level_t* mlhmmv_level_alloc();
static void            mlhmmv_level_init(mlhmmv_level_t *plevel, int length);
static void            mlhmmv_level_free(mlhmmv_level_t* plevel);

static int mlhmmv_level_find_index_for_key(mlhmmv_level_t* plevel, mv_t* plevel_key, int* pideal_index);

static void mlhmmv_level_put_no_enlarge(mlhmmv_level_t* plevel, sllmve_t* prest_keys, mv_t* pterminal_value);
static void mlhmmv_level_move(mlhmmv_level_t* plevel, mv_t* plevel_key, mlhmmv_value_t* plevel_value);

static mlhmmv_level_entry_t* mlhmmv_get_entry_at_level(mlhmmv_level_t* plevel, sllmve_t* prestkeys, int* perror);
static mlhmmv_level_t* mlhmmv_get_or_create_level_aux(mlhmmv_level_t* plevel, sllmve_t* prest_keys);
static mlhmmv_level_t* mlhmmv_get_or_create_level_aux_no_enlarge(mlhmmv_level_t* plevel, sllmve_t* prest_keys);

static void mlhmmv_put_value_at_level(mlhmmv_t* pmap, sllmv_t* pmvkeys, mlhmmv_value_t* pvalue);

static mlhmmv_level_entry_t* mlhmmv_get_next_level_entry(mlhmmv_level_t* pmap, mv_t* plevel_key, int* pindex);
static mlhmmv_value_t* mlhmmv_get_next_level_entry_value(mlhmmv_level_t* pmap, mv_t* plevel_key);

static void mlhmmv_remove_aux(mlhmmv_level_t* plevel, sllmve_t* prestkeys, int* pemptied, int depth);

static void mlhmmv_put_value_at_level_aux(mlhmmv_level_t* plevel, sllmve_t* prest_keys, mlhmmv_value_t* pvalue);
static void mlhmmv_level_put_value_no_enlarge(mlhmmv_level_t* plevel, sllmve_t* prest_keys,
	mlhmmv_value_t* pvalue);
static void mlhmmv_level_enlarge(mlhmmv_level_t* plevel);

// xxx needs to be public -- rename -- static mlhmmv_value_t mlhmmv_copy_aux(mlhmmv_value_t* pvalue);
static sllv_t* mlhmmv_copy_keys_from_submap_aux(mlhmmv_value_t* pvalue);

static void mlhmmv_to_lrecs_aux_across_records(mlhmmv_level_t* plevel, char* prefix, sllmve_t* prestnames,
	lrec_t* ptemplate, sllv_t* poutrecs, int do_full_prefixing, char* flatten_separator);
static void mlhmmv_to_lrecs_aux_within_record(mlhmmv_level_t* plevel, char* prefix,
	lrec_t* poutrec, int do_full_prefixing, char* flatten_separator);

static void mlhmmv_to_lrecs_aux_across_records_lashed(mlhmmv_level_t** plevels, char** prefixes, int num_levels,
	sllmve_t* prestnames, lrec_t* ptemplate, sllv_t* poutrecs, int do_full_prefixing, char* flatten_separator);
static void mlhmmv_to_lrecs_aux_within_record_lashed(mlhmmv_level_t** pplevels, char** prefixes, int num_levels,
	lrec_t* poutrec, int do_full_prefixing, char* flatten_separator);

static void mlhmmv_level_print_single_line(mlhmmv_level_t* plevel, int depth,
	int do_final_comma, int quote_values_always, FILE* ostream);

static void json_decimal_print(char* s, FILE* ostream);

static int mlhmmv_hash_func(mv_t* plevel_key);

// ----------------------------------------------------------------
// Allow compile-time override, e.g using gcc -D.

#ifndef LOAD_FACTOR
#define LOAD_FACTOR          0.7
#endif

#ifndef ENLARGEMENT_FACTOR
#define ENLARGEMENT_FACTOR   2
#endif

#define OCCUPIED             0xa4
#define DELETED              0xb8
#define EMPTY                0xce

// ----------------------------------------------------------------
mlhmmv_t* mlhmmv_alloc() {
	mlhmmv_t* pmap = mlr_malloc_or_die(sizeof(mlhmmv_t));
	pmap->proot_level = mlhmmv_level_alloc();
	return pmap;
}

mlhmmv_value_t mlhmmv_value_alloc_empty_map() {
	mlhmmv_value_t xval = (mlhmmv_value_t) {
		.is_terminal = FALSE,
		.u.pnext_level = mlhmmv_level_alloc()
	};
	return xval;
}

// xxx temp expose static
mlhmmv_level_t* mlhmmv_level_alloc() {
	mlhmmv_level_t* plevel = mlr_malloc_or_die(sizeof(mlhmmv_level_t));
	mlhmmv_level_init(plevel, MLHMMV_INITIAL_ARRAY_LENGTH);
	return plevel;
}

static void mlhmmv_level_init(mlhmmv_level_t *plevel, int length) {
	plevel->num_occupied = 0;
	plevel->num_freed    = 0;
	plevel->array_length = length;

	plevel->entries      = (mlhmmv_level_entry_t*)mlr_malloc_or_die(sizeof(mlhmmv_level_entry_t) * length);
	// Don't do mlhmmv_level_entry_clear() of all entries at init time, since this has a
	// drastic effect on the time needed to construct an empty map (and miller
	// constructs an awful lot of those). The attributes there are don't-cares
	// if the corresponding entry state is EMPTY. They are set on put, and
	// mutated on remove.

	plevel->states       = (mlhmmv_level_entry_state_t*)mlr_malloc_or_die(sizeof(mlhmmv_level_entry_state_t) * length);
	memset(plevel->states, EMPTY, length);

	plevel->phead        = NULL;
	plevel->ptail        = NULL;
}

// ----------------------------------------------------------------
void mlhmmv_free(mlhmmv_t* pmap) {
	if (pmap == NULL)
		return;
	mlhmmv_level_free(pmap->proot_level);
	free(pmap);
}

static void mlhmmv_level_free(mlhmmv_level_t* plevel) {
	for (mlhmmv_level_entry_t* pentry = plevel->phead; pentry != NULL; pentry = pentry->pnext) {
		if (pentry->level_value.is_terminal) {
			mv_free(&pentry->level_value.u.mlrval);
		} else {
			mlhmmv_level_free(pentry->level_value.u.pnext_level);
		}
		mv_free(&pentry->level_key);
	}
	free(plevel->entries);
	free(plevel->states);
	plevel->entries      = NULL;
	plevel->num_occupied = 0;
	plevel->num_freed    = 0;
	plevel->array_length = 0;

	free(plevel);
}

// ----------------------------------------------------------------
// Used by get() and remove().
// Returns >=0 for where the key is *or* should go (end of chain).
static int mlhmmv_level_find_index_for_key(mlhmmv_level_t* plevel, mv_t* plevel_key, int* pideal_index) {
	int hash = mlhmmv_hash_func(plevel_key);
	int index = mlr_canonical_mod(hash, plevel->array_length);
	*pideal_index = index;
	int num_tries = 0;

	while (TRUE) {
		mlhmmv_level_entry_t* pentry = &plevel->entries[index];
		if (plevel->states[index] == OCCUPIED) {
			mv_t* ekey = &pentry->level_key;
			// Existing key found in chain.
			if (mv_equals_si(plevel_key, ekey))
				return index;
		} else if (plevel->states[index] == EMPTY) {
			return index;
		}

		// If the current entry has been freed, i.e. previously occupied,
		// the sought index may be further down the chain.  So we must
		// continue looking.
		if (++num_tries >= plevel->array_length) {
			fprintf(stderr,
				"%s: Coding error:  table full even after enlargement.\n", MLR_GLOBALS.bargv0);
			exit(1);
		}

		// Linear probing.
		if (++index >= plevel->array_length)
			index = 0;
	}
	MLR_INTERNAL_CODING_ERROR();
	return -1; // not reached
}

// ----------------------------------------------------------------
// Example: keys = ["a", 2, "c"] and value = 4.
void mlhmmv_put_terminal(mlhmmv_t* pmap, sllmv_t* pmvkeys, mv_t* pterminal_value) {
	mlhmmv_put_terminal_from_level(pmap->proot_level, pmvkeys->phead, pterminal_value);
}

// Example on recursive calls:
// * level = map, rest_keys = ["a", 2, "c"] , terminal value = 4.
// * level = map["a"], rest_keys = [2, "c"] , terminal value = 4.
// * level = map["a"][2], rest_keys = ["c"] , terminal value = 4.
void mlhmmv_put_terminal_from_level(mlhmmv_level_t* plevel, sllmve_t* prest_keys, mv_t* pterminal_value) {
	if ((plevel->num_occupied + plevel->num_freed) >= (plevel->array_length * LOAD_FACTOR))
		mlhmmv_level_enlarge(plevel);
	mlhmmv_level_put_no_enlarge(plevel, prest_keys, pterminal_value);
}

static void mlhmmv_level_put_no_enlarge(mlhmmv_level_t* plevel, sllmve_t* prest_keys, mv_t* pterminal_value) {
	mv_t* plevel_key = &prest_keys->value;
	int ideal_index = 0;
	int index = mlhmmv_level_find_index_for_key(plevel, plevel_key, &ideal_index);
	mlhmmv_level_entry_t* pentry = &plevel->entries[index];

	if (plevel->states[index] == EMPTY) { // End of chain.
		pentry->ideal_index = ideal_index;
		pentry->level_key = mv_copy(plevel_key);

		if (prest_keys->pnext == NULL) {
			pentry->level_value.is_terminal = TRUE;
			pentry->level_value.u.mlrval = mv_copy(pterminal_value);
		} else {
			pentry->level_value.is_terminal = FALSE;
			pentry->level_value.u.pnext_level = mlhmmv_level_alloc();
		}
		plevel->states[index] = OCCUPIED;

		if (plevel->phead == NULL) { // First entry at this level
			pentry->pprev = NULL;
			pentry->pnext = NULL;
			plevel->phead = pentry;
			plevel->ptail = pentry;
		} else {                     // Subsequent entry at this level
			pentry->pprev = plevel->ptail;
			pentry->pnext = NULL;
			plevel->ptail->pnext = pentry;
			plevel->ptail = pentry;
		}

		plevel->num_occupied++;
		if (prest_keys->pnext != NULL) {
			// RECURSE
			mlhmmv_put_terminal_from_level(pentry->level_value.u.pnext_level, prest_keys->pnext, pterminal_value);
		}

	} else if (plevel->states[index] == OCCUPIED) { // Existing key found in chain
		if (prest_keys->pnext == NULL) { // Place the terminal at this level
			if (pentry->level_value.is_terminal) {
				mv_free(&pentry->level_value.u.mlrval);
			} else {
				mlhmmv_level_free(pentry->level_value.u.pnext_level);
			}
			pentry->level_value.is_terminal = TRUE;
			pentry->level_value.u.mlrval = mv_copy(pterminal_value);

		} else { // The terminal will be placed at a deeper level
			if (pentry->level_value.is_terminal) {
				mv_free(&pentry->level_value.u.mlrval);
				pentry->level_value.is_terminal = FALSE;
				pentry->level_value.u.pnext_level = mlhmmv_level_alloc();
			}
			// RECURSE
			mlhmmv_put_terminal_from_level(pentry->level_value.u.pnext_level, prest_keys->pnext, pterminal_value);
		}

	} else {
		fprintf(stderr, "%s: mlhmmv_level_find_index_for_key did not find end of chain\n", MLR_GLOBALS.bargv0);
		exit(1);
	}
}

// ----------------------------------------------------------------
mlhmmv_level_t* mlhmmv_put_empty_map_from_level(mlhmmv_level_t* plevel, sllmve_t* prest_keys) {
	mv_t x = mv_absent();
	mlhmmv_put_terminal_from_level(plevel, prest_keys, &x); // xxx optimize to avoid 2nd lookup
	int error;
	sllmv_t s = { // xxx simplify API
		.phead = prest_keys,
		.ptail = prest_keys,
		.length = 1
	};
	mlhmmv_value_t* pxval = mlhmmv_get_value_from_level(plevel, &s, &error);
	*pxval = mlhmmv_value_alloc_empty_map();
	return pxval->u.pnext_level;
}

// ----------------------------------------------------------------
// This is done only on map-level enlargement.
// Example:
// * level = map["a"], rest_keys = [2, "c"] ,   terminal_value = 4.
//                     rest_keys = ["e", "f"] , terminal_value = 7.
//                     rest_keys = [6] ,        terminal_value = "g".
//
// which is to say for the purposes of this routine
//
// * level = map["a"], level_key = 2,   level_value = non-terminal ["c"] => terminal_value = 4.
//                     level_key = "e", level_value = non-terminal ["f"] => terminal_value = 7.
//                     level_key = 6,   level_value = terminal_value = "g".

static void mlhmmv_level_move(mlhmmv_level_t* plevel, mv_t* plevel_key, mlhmmv_value_t* plevel_value) {
	int ideal_index = 0;
	int index = mlhmmv_level_find_index_for_key(plevel, plevel_key, &ideal_index);
	mlhmmv_level_entry_t* pentry = &plevel->entries[index];

	if (plevel->states[index] == OCCUPIED) {
		// Existing key found in chain; put value.
		pentry->level_value = *plevel_value;

	} else if (plevel->states[index] == EMPTY) {
		// End of chain.
		pentry->ideal_index = ideal_index;
		pentry->level_key = *plevel_key;
		// For the put API, we copy data passed in. But for internal enlarges, we just need to move pointers around.
		pentry->level_value = *plevel_value;
		plevel->states[index] = OCCUPIED;

		if (plevel->phead == NULL) {
			pentry->pprev = NULL;
			pentry->pnext = NULL;
			plevel->phead = pentry;
			plevel->ptail = pentry;
		} else {
			pentry->pprev = plevel->ptail;
			pentry->pnext = NULL;
			plevel->ptail->pnext = pentry;
			plevel->ptail = pentry;
		}
		plevel->num_occupied++;
	}
	else {
		fprintf(stderr, "%s: mlhmmv_level_find_index_for_key did not find end of chain\n", MLR_GLOBALS.bargv0);
		exit(1);
	}
}

// ----------------------------------------------------------------
// xxx merge these two
mv_t* mlhmmv_get_terminal(mlhmmv_t* pmap, sllmv_t* pmvkeys, int* perror) {
	mlhmmv_level_entry_t* plevel_entry = mlhmmv_get_entry_at_level(pmap->proot_level, pmvkeys->phead, perror);
	if (plevel_entry == NULL) {
		return NULL;
	}
	if (!plevel_entry->level_value.is_terminal) {
		*perror = MLHMMV_ERROR_KEYLIST_TOO_SHALLOW;
		return NULL;
	}
	return &plevel_entry->level_value.u.mlrval;
}

mv_t* mlhmmv_get_terminal_from_level(mlhmmv_level_t* plevel, sllmv_t* pmvkeys, int* perror) {
	mlhmmv_level_entry_t* plevel_entry = mlhmmv_get_entry_at_level(plevel, pmvkeys->phead, perror);
	if (plevel_entry == NULL) {
		return NULL;
	}
	if (!plevel_entry->level_value.is_terminal) {
		*perror = MLHMMV_ERROR_KEYLIST_TOO_SHALLOW;
		return NULL;
	}
	return &plevel_entry->level_value.u.mlrval;
}

// ----------------------------------------------------------------
static mlhmmv_level_entry_t* mlhmmv_get_entry_at_level(mlhmmv_level_t* plevel, sllmve_t* prestkeys, int* perror) {
	if (perror)
		*perror = MLHMMV_ERROR_NONE;
	if (prestkeys == NULL) {
		if (perror)
			*perror = MLHMMV_ERROR_KEYLIST_TOO_SHALLOW;
		return NULL;
	}
	mlhmmv_level_entry_t* plevel_entry = mlhmmv_get_next_level_entry(plevel, &prestkeys->value, NULL);
	while (prestkeys->pnext != NULL) {
		if (plevel_entry == NULL) {
			return NULL;
		}
		if (plevel_entry->level_value.is_terminal) {
			if (perror)
				*perror = MLHMMV_ERROR_KEYLIST_TOO_DEEP;
			return NULL;
		}
		plevel = plevel_entry->level_value.u.pnext_level;
		prestkeys = prestkeys->pnext;
		plevel_entry = mlhmmv_get_next_level_entry(plevel_entry->level_value.u.pnext_level, &prestkeys->value, NULL);
	}
	return plevel_entry;
}

// ----------------------------------------------------------------
// Example on recursive calls:
// * level = map, rest_keys = ["a", 2, "c"]
// * level = map["a"], rest_keys = [2, "c"]
// * level = map["a"][2], rest_keys = ["c"]
mlhmmv_level_t* mlhmmv_get_or_create_level(mlhmmv_t* pmap, sllmv_t* pmvkeys) {
	return mlhmmv_get_or_create_level_aux(pmap->proot_level, pmvkeys->phead);
}
static mlhmmv_level_t* mlhmmv_get_or_create_level_aux(mlhmmv_level_t* plevel, sllmve_t* prest_keys) {
	if ((plevel->num_occupied + plevel->num_freed) >= (plevel->array_length * LOAD_FACTOR))
		mlhmmv_level_enlarge(plevel);
	return mlhmmv_get_or_create_level_aux_no_enlarge(plevel, prest_keys);
}

static mlhmmv_level_t* mlhmmv_get_or_create_level_aux_no_enlarge(mlhmmv_level_t* plevel, sllmve_t* prest_keys) {
	mv_t* plevel_key = &prest_keys->value;
	int ideal_index = 0;
	int index = mlhmmv_level_find_index_for_key(plevel, plevel_key, &ideal_index);
	mlhmmv_level_entry_t* pentry = &plevel->entries[index];

	if (plevel->states[index] == EMPTY) { // End of chain.

		plevel->states[index] = OCCUPIED;
		plevel->num_occupied++;
		pentry->ideal_index = ideal_index;
		pentry->level_key = mv_copy(plevel_key);
		pentry->level_value.is_terminal = FALSE;
		pentry->level_value.u.pnext_level = mlhmmv_level_alloc();

		if (plevel->phead == NULL) { // First entry at this level
			pentry->pprev = NULL;
			pentry->pnext = NULL;
			plevel->phead = pentry;
			plevel->ptail = pentry;
		} else {                     // Subsequent entry at this level
			pentry->pprev = plevel->ptail;
			pentry->pnext = NULL;
			plevel->ptail->pnext = pentry;
			plevel->ptail = pentry;
		}

		if (prest_keys->pnext != NULL) {
			// RECURSE
			return mlhmmv_get_or_create_level_aux(pentry->level_value.u.pnext_level, prest_keys->pnext);
		} else {
			return pentry->level_value.u.pnext_level;
		}

	} else if (plevel->states[index] == OCCUPIED) { // Existing key found in chain

		if (pentry->level_value.is_terminal) {
			mv_free(&pentry->level_value.u.mlrval);
			pentry->level_value.is_terminal = FALSE;
			pentry->level_value.u.pnext_level = mlhmmv_level_alloc();
		}
		if (prest_keys->pnext == NULL) {
			return pentry->level_value.u.pnext_level;
		} else { // RECURSE
			return mlhmmv_get_or_create_level_aux(pentry->level_value.u.pnext_level, prest_keys->pnext);
		}

	} else {
		fprintf(stderr, "%s: mlhmmv_level_find_index_for_key did not find end of chain\n", MLR_GLOBALS.bargv0);
		exit(1);
	}
}

// ----------------------------------------------------------------
mlhmmv_level_t* mlhmmv_get_level(mlhmmv_t* pmap, sllmv_t* pmvkeys, int* perror) {
	*perror = MLHMMV_ERROR_NONE;
	sllmve_t* prest_keys = pmvkeys->phead;
	if (prest_keys == NULL) {
		*perror = MLHMMV_ERROR_KEYLIST_TOO_SHALLOW;
	}
	mlhmmv_level_t* plevel = pmap->proot_level;
	mlhmmv_level_entry_t* plevel_entry = mlhmmv_get_next_level_entry(plevel, &prest_keys->value, NULL);
	while (prest_keys->pnext != NULL) {
		if (plevel_entry == NULL) {
			return NULL;
		} else if (plevel_entry->level_value.is_terminal) {
			*perror = MLHMMV_ERROR_KEYLIST_TOO_DEEP;
			return NULL;
		} else {
			plevel = plevel_entry->level_value.u.pnext_level;
			prest_keys = prest_keys->pnext;
			plevel_entry = mlhmmv_get_next_level_entry(plevel_entry->level_value.u.pnext_level,
				&prest_keys->value, NULL);
		}
	}
	if (plevel_entry == NULL || plevel_entry->level_value.is_terminal) {
		*perror = MLHMMV_ERROR_KEYLIST_TOO_DEEP;
		return NULL;
	}
	return plevel_entry->level_value.u.pnext_level;
}

static mlhmmv_level_entry_t* mlhmmv_get_next_level_entry(mlhmmv_level_t* plevel, mv_t* plevel_key, int* pindex) {
	int ideal_index = 0;
	int index = mlhmmv_level_find_index_for_key(plevel, plevel_key, &ideal_index);
	mlhmmv_level_entry_t* pentry = &plevel->entries[index];

	if (pindex != NULL)
		*pindex = index;

	if (plevel->states[index] == OCCUPIED)
		return pentry;
	else if (plevel->states[index] == EMPTY)
		return NULL;
	else {
		fprintf(stderr, "%s: mlhmmv_level_find_index_for_key did not find end of chain\n", MLR_GLOBALS.bargv0);
		exit(1);
	}
}

static mlhmmv_value_t* mlhmmv_get_next_level_entry_value(mlhmmv_level_t* pmap, mv_t* plevel_key) {
	if (pmap == NULL)
		return NULL;
	mlhmmv_level_entry_t* pentry = mlhmmv_get_next_level_entry(pmap, plevel_key, NULL);
	if (pentry == NULL)
		return NULL;
	else
		return &pentry->level_value;
}

// ----------------------------------------------------------------
mlhmmv_value_t* mlhmmv_get_value_from_level(mlhmmv_level_t* pstart_level, sllmv_t* pmvkeys, int* perror) {
	*perror = MLHMMV_ERROR_NONE;
	sllmve_t* prest_keys = pmvkeys->phead;
	if (prest_keys == NULL) {
		*perror = MLHMMV_ERROR_KEYLIST_TOO_SHALLOW;
	}
	mlhmmv_level_t* plevel = pstart_level;
	mlhmmv_level_entry_t* plevel_entry = mlhmmv_get_next_level_entry(plevel, &prest_keys->value, NULL);
	while (prest_keys->pnext != NULL) {
		if (plevel_entry == NULL) {
			return NULL;
		} else {
			plevel = plevel_entry->level_value.u.pnext_level;
			prest_keys = prest_keys->pnext;
			plevel_entry = mlhmmv_get_next_level_entry(plevel_entry->level_value.u.pnext_level,
				&prest_keys->value, NULL);
		}
	}
	if (plevel_entry == NULL) {
		*perror = MLHMMV_ERROR_KEYLIST_TOO_DEEP;
		return NULL;
	}
	return &plevel_entry->level_value;
}

// ----------------------------------------------------------------
// Removes entries from a specified level downward, unsetting any maps which become empty as a result.  For example, if
// e.g. a=>b=>c=>4 and the c level is to be removed, then all up-nodes are emptied out & should be pruned.
// * If restkeys too long (e.g. 'unset $a["b"]["c"]' with data "a":"b":3): do nothing.
// * If restkeys just right: (e.g. 'unset $a["b"]' with data "a":"b":3) remove the terminal mlrval.
// * If restkeys is too short: (e.g. 'unset $a["b"]' with data "a":"b":"c":4): remove the level and all below.

void mlhmmv_remove(mlhmmv_t* pmap, sllmv_t* prestkeys) {
	if (prestkeys == NULL) {
		return;
	} else if (prestkeys->phead == NULL) {
		mlhmmv_level_free(pmap->proot_level);
		pmap->proot_level = mlhmmv_level_alloc();
		return;
	} else {
		int unused = FALSE;
		mlhmmv_remove_aux(pmap->proot_level, prestkeys->phead, &unused, 0);
	}
}

static void mlhmmv_remove_aux(mlhmmv_level_t* plevel, sllmve_t* prestkeys, int* pemptied, int depth) {
	*pemptied = FALSE;

	if (prestkeys == NULL) // restkeys too short
		return;

	int index = -1;
	mlhmmv_level_entry_t* pentry = mlhmmv_get_next_level_entry(plevel, &prestkeys->value, &index);
	if (pentry == NULL)
		return;

	if (prestkeys->pnext != NULL) {
		// Keep recursing until end of restkeys.
		if (pentry->level_value.is_terminal) // restkeys too long
			return;
		int descendant_emptied = FALSE;
		mlhmmv_remove_aux(pentry->level_value.u.pnext_level, prestkeys->pnext, &descendant_emptied, depth+1);

		// If the recursive call emptied the next-level slot, remove it from our level as well. This may continue all
		// the way back up. Example: the map is '{"a":{"b":{"c":4}}}' and we're asked to remove keylist ["a", "b", "c"].
		// The recursive call to the terminal will leave '{"a":{"b":{}}}' -- note the '{}'. Then we remove
		// that to leave '{"a":{}}'. Since this leaves another '{}', passing emptied==TRUE back to our caller
		// leaves empty top-level map '{}'.
		if (descendant_emptied) {
			plevel->num_occupied--;
			plevel->num_freed++;
			plevel->states[index] = DELETED;
			pentry->ideal_index = -1;
			mv_free(&pentry->level_key);
			pentry->level_key = mv_error();

			if (pentry == plevel->phead) {
				if (pentry == plevel->ptail) {
					plevel->phead = NULL;
					plevel->ptail = NULL;
					*pemptied = TRUE;
				} else {
					plevel->phead = pentry->pnext;
					pentry->pnext->pprev = NULL;
				}
			} else if (pentry == plevel->ptail) {
					plevel->ptail = pentry->pprev;
					pentry->pprev->pnext = NULL;
			} else {
				pentry->pprev->pnext = pentry->pnext;
				pentry->pnext->pprev = pentry->pprev;
			}
			if (pentry->level_value.is_terminal) {
				mv_free(&pentry->level_value.u.mlrval);
			} else {
				mlhmmv_level_free(pentry->level_value.u.pnext_level);
			}
		}

	} else {
		// End of restkeys. Deletion & free logic goes here. Set *pemptied if the level was emptied out.

		// 1. Excise the node and its descendants from the storage tree
		if (plevel->states[index] != OCCUPIED) {
			fprintf(stderr, "%s: mlhmmv_remove: did not find end of chain.\n", MLR_GLOBALS.bargv0);
			exit(1);
		}

		mv_free(&pentry->level_key);
		pentry->ideal_index = -1;
		plevel->states[index] = DELETED;

		if (pentry == plevel->phead) {
			if (pentry == plevel->ptail) {
				plevel->phead = NULL;
				plevel->ptail = NULL;
				*pemptied = TRUE;
			} else {
				plevel->phead = pentry->pnext;
				pentry->pnext->pprev = NULL;
			}
		} else if (pentry == plevel->ptail) {
				plevel->ptail = pentry->pprev;
				pentry->pprev->pnext = NULL;
		} else {
			pentry->pprev->pnext = pentry->pnext;
			pentry->pnext->pprev = pentry->pprev;
		}

		plevel->num_freed++;
		plevel->num_occupied--;

		// 2. Free the memory for the node and its descendants
		if (pentry->level_value.is_terminal) {
			mv_free(&pentry->level_value.u.mlrval);
		} else {
			mlhmmv_level_free(pentry->level_value.u.pnext_level);
		}
	}

}

// ----------------------------------------------------------------
void mlhmmv_clear_level(mlhmmv_level_t* plevel) {
	if (plevel->phead == NULL)
		return;

	for (mlhmmv_level_entry_t* pentry = plevel->phead; pentry != NULL; pentry = pentry->pnext) {
		if (pentry->level_value.is_terminal) {
			mv_free(&pentry->level_value.u.mlrval);
		} else {
			mlhmmv_level_free(pentry->level_value.u.pnext_level);
		}
		mv_free(&pentry->level_key);
	}
	plevel->num_occupied = 0;
	plevel->num_freed    = 0;
	plevel->phead        = NULL;
	plevel->ptail        = NULL;

	memset(plevel->states, EMPTY, plevel->array_length);
}

// ----------------------------------------------------------------
void mlhmmv_copy(mlhmmv_t* pmap, sllmv_t* ptokeys, sllmv_t* pfromkeys) {
	int error = 0;

	mlhmmv_level_entry_t* pfromentry = mlhmmv_get_entry_at_level(pmap->proot_level, pfromkeys->phead, &error);
	if (pfromentry != NULL) {
		mlhmmv_value_t submap = mlhmmv_copy_aux(&pfromentry->level_value);
		mlhmmv_put_value_at_level(pmap, ptokeys, &submap);
	}
}

mlhmmv_value_t mlhmmv_copy_aux(mlhmmv_value_t* pvalue) { // xxx rename
	if (pvalue->is_terminal) {
		return (mlhmmv_value_t) {
			.is_terminal = TRUE,
			.u.mlrval = mv_copy(&pvalue->u.mlrval)
		};

	} else {
		mlhmmv_level_t* psrc_level = pvalue->u.pnext_level;
		mlhmmv_level_t* pdst_level = mlr_malloc_or_die(sizeof(mlhmmv_level_t));

		mlhmmv_level_init(pdst_level, MLHMMV_INITIAL_ARRAY_LENGTH);

		for (
			mlhmmv_level_entry_t* psubentry = psrc_level->phead;
			psubentry != NULL;
			psubentry = psubentry->pnext)
		{
			sllmve_t e = { .value = psubentry->level_key, .free_flags = 0, .pnext = NULL };
			mlhmmv_value_t next_value = mlhmmv_copy_aux(&psubentry->level_value);
			mlhmmv_put_value_at_level_aux(pdst_level, &e, &next_value);
		}

		return (mlhmmv_value_t) {
			.is_terminal = FALSE,
			.u.pnext_level = pdst_level
		};
	}
}

// ----------------------------------------------------------------
mlhmmv_value_t mlhmmv_copy_submap_from_root(mlhmmv_t* pmap, sllmv_t* pmvkeys) {
	int error;
	if (pmvkeys == NULL || pmvkeys->length == 0) {
		mlhmmv_value_t root_value = (mlhmmv_value_t) {
			.is_terminal = FALSE,
			.u.pnext_level = pmap->proot_level,
		};
		return mlhmmv_copy_aux(&root_value);
	} else {
		mlhmmv_level_entry_t* pfromentry = mlhmmv_get_entry_at_level(pmap->proot_level, pmvkeys->phead, &error);
		if (pfromentry != NULL) {
			return mlhmmv_copy_aux(&pfromentry->level_value);
		} else {
			return (mlhmmv_value_t) {
				.is_terminal = FALSE,
				.u.pnext_level = NULL,
			};
		}
	}
}

void mlhmmv_free_submap(mlhmmv_value_t submap) {
	if (submap.is_terminal) {
		mv_free(&submap.u.mlrval);
	} else if (submap.u.pnext_level != NULL) {
		mlhmmv_level_free(submap.u.pnext_level);
	}
}

// ----------------------------------------------------------------
sllv_t* mlhmmv_copy_keys_from_submap(mlhmmv_t* pmap, sllmv_t* pmvkeys) {
	int error;
	if (pmvkeys->length == 0) {
		mlhmmv_value_t root_value = (mlhmmv_value_t) {
			.is_terminal = FALSE,
			.u.pnext_level = pmap->proot_level,
		};
		return mlhmmv_copy_keys_from_submap_aux(&root_value);
	} else {
		mlhmmv_level_entry_t* pfromentry = mlhmmv_get_entry_at_level(pmap->proot_level, pmvkeys->phead, &error);
		if (pfromentry != NULL) {
			return mlhmmv_copy_keys_from_submap_aux(&pfromentry->level_value);
		} else {
			return sllv_alloc();
		}
	}
}

static sllv_t* mlhmmv_copy_keys_from_submap_aux(mlhmmv_value_t* pvalue) {
	sllv_t* pkeys = sllv_alloc();

	if (!pvalue->is_terminal) {
		mlhmmv_level_t* pnext_level = pvalue->u.pnext_level;
		for (mlhmmv_level_entry_t* pe = pnext_level->phead; pe != NULL; pe = pe->pnext) {
			//sllv_append(pkeys, mv_alloc_copy(&pe->level_key));
			mv_t* p = mv_alloc_copy(&pe->level_key);
			sllv_append(pkeys, p);
		}
	}

	return pkeys;
}

// xxx code dedupe
sllv_t* mlhmmv_copy_keys_from_submap_xxx_rename(mlhmmv_value_t* pmvalue, sllmv_t* pmvkeys) {
	int error;
	if (pmvkeys == NULL || pmvkeys->length == 0) {
		return mlhmmv_copy_keys_from_submap_aux(pmvalue);
	} else if (pmvalue->is_terminal) { // xxx copy this check up to oosvar case too
		return sllv_alloc();
	} else {
		mlhmmv_level_entry_t* pfromentry = mlhmmv_get_entry_at_level(pmvalue->u.pnext_level, pmvkeys->phead, &error);
		if (pfromentry != NULL) {
			return mlhmmv_copy_keys_from_submap_aux(&pfromentry->level_value);
		} else {
			return sllv_alloc();
		}
	}
}

// ----------------------------------------------------------------
static void mlhmmv_put_value_at_level(mlhmmv_t* pmap, sllmv_t* pmvkeys, mlhmmv_value_t* pvalue) {
	mlhmmv_put_value_at_level_aux(pmap->proot_level, pmvkeys->phead, pvalue);
}

static void mlhmmv_put_value_at_level_aux(mlhmmv_level_t* plevel, sllmve_t* prest_keys, mlhmmv_value_t* pvalue) {
	if ((plevel->num_occupied + plevel->num_freed) >= (plevel->array_length * LOAD_FACTOR))
		mlhmmv_level_enlarge(plevel);
	mlhmmv_level_put_value_no_enlarge(plevel, prest_keys, pvalue);
}

static void mlhmmv_level_put_value_no_enlarge(mlhmmv_level_t* plevel, sllmve_t* prest_keys,
	mlhmmv_value_t* pvalue)
{
	mv_t* plevel_key = &prest_keys->value;
	int ideal_index = 0;
	int index = mlhmmv_level_find_index_for_key(plevel, plevel_key, &ideal_index);
	mlhmmv_level_entry_t* pentry = &plevel->entries[index];

	if (plevel->states[index] == EMPTY) { // End of chain.
		pentry->ideal_index = ideal_index;
		pentry->level_key = mv_copy(plevel_key);

		if (prest_keys->pnext == NULL) {
			pentry->level_value = *pvalue;
		} else {
			pentry->level_value.is_terminal = FALSE;
			pentry->level_value.u.pnext_level = mlhmmv_level_alloc();
		}
		plevel->states[index] = OCCUPIED;

		if (plevel->phead == NULL) { // First entry at this level
			pentry->pprev = NULL;
			pentry->pnext = NULL;
			plevel->phead = pentry;
			plevel->ptail = pentry;
		} else {                     // Subsequent entry at this level
			pentry->pprev = plevel->ptail;
			pentry->pnext = NULL;
			plevel->ptail->pnext = pentry;
			plevel->ptail = pentry;
		}

		plevel->num_occupied++;
		if (prest_keys->pnext != NULL) {
			// RECURSE
			mlhmmv_put_value_at_level_aux(pentry->level_value.u.pnext_level, prest_keys->pnext, pvalue);
		}

	} else if (plevel->states[index] == OCCUPIED) { // Existing key found in chain
		if (prest_keys->pnext == NULL) { // Place the terminal at this level
			if (pentry->level_value.is_terminal) {
				mv_free(&pentry->level_value.u.mlrval);
			} else {
				mlhmmv_level_free(pentry->level_value.u.pnext_level);
			}
			pentry->level_value = *pvalue;

		} else { // The terminal will be placed at a deeper level
			if (pentry->level_value.is_terminal) {
				mv_free(&pentry->level_value.u.mlrval);
				pentry->level_value.is_terminal = FALSE;
				pentry->level_value.u.pnext_level = mlhmmv_level_alloc();
			}
			// RECURSE
			mlhmmv_put_value_at_level_aux(pentry->level_value.u.pnext_level, prest_keys->pnext, pvalue);
		}

	} else {
		fprintf(stderr, "%s: mlhmmv_level_find_index_for_key did not find end of chain\n", MLR_GLOBALS.bargv0);
		exit(1);
	}
}

// ----------------------------------------------------------------
static void mlhmmv_level_enlarge(mlhmmv_level_t* plevel) {
	mlhmmv_level_entry_t*       old_entries = plevel->entries;
	mlhmmv_level_entry_state_t* old_states  = plevel->states;
	mlhmmv_level_entry_t*       old_head    = plevel->phead;

	mlhmmv_level_init(plevel, plevel->array_length*ENLARGEMENT_FACTOR);

	for (mlhmmv_level_entry_t* pentry = old_head; pentry != NULL; pentry = pentry->pnext) {
		mlhmmv_level_move(plevel, &pentry->level_key, &pentry->level_value);
	}
	free(old_entries);
	free(old_states);
}

// ----------------------------------------------------------------
// For 'emit all' and 'emitp all'.
void mlhmmv_all_to_lrecs(mlhmmv_t* pmap, sllmv_t* pnames, sllv_t* poutrecs, int do_full_prefixing,
	char* flatten_separator)
{
	for (mlhmmv_level_entry_t* pentry = pmap->proot_level->phead; pentry != NULL; pentry = pentry->pnext) {
		sllmv_t* pkey = sllmv_single_no_free(&pentry->level_key);
		mlhmmv_to_lrecs(pmap, pkey, pnames, poutrecs, do_full_prefixing, flatten_separator);
		sllmv_free(pkey);
	}
}

// ----------------------------------------------------------------
// For 'emit' and 'emitp': the latter has do_full_prefixing == TRUE.  These allocate lrecs, appended to the poutrecs
// list.

// * pmap is the base-level oosvar multi-level hashmap.
// * pkeys specify the level in the mlhmmv at which to produce data.
// * pnames is used to pull subsequent-level keys out into separate fields.
// * In case pnames isn't long enough to reach a terminal mlrval level in the mlhmmv,
//   do_full_prefixing specifies whether to concatenate nested mlhmmv keys into single lrec keys.
//
// Examples:

// * pkeys reaches a terminal level:
//
//   $ mlr --opprint put -q '@sum += $x; end { emit @sum }' ../data/small
//   sum
//   4.536294

// * pkeys reaches terminal levels:
//
//   $ mlr --opprint put -q '@sum[$a][$b] += $x; end { emit @sum, "a", "b" }' ../data/small
//   a   b   sum
//   pan pan 0.346790
//   pan wye 0.502626
//   eks pan 0.758680
//   eks wye 0.381399
//   eks zee 0.611784
//   wye wye 0.204603
//   wye pan 0.573289
//   zee pan 0.527126
//   zee wye 0.598554
//   hat wye 0.031442

// * pkeys reaches non-terminal levels: non-prefixed:
//
//   $ mlr --opprint put -q '@sum[$a][$b] += $x; end { emit @sum, "a" }' ../data/small
//   a   pan      wye
//   pan 0.346790 0.502626
//
//   a   pan      wye      zee
//   eks 0.758680 0.381399 0.611784
//
//   a   wye      pan
//   wye 0.204603 0.573289
//
//   a   pan      wye
//   zee 0.527126 0.598554
//
//   a   wye
//   hat 0.031442

// * pkeys reaches non-terminal levels: prefixed:
//
//   $ mlr --opprint put -q '@sum[$a][$b] += $x; end { emitp @sum, "a" }' ../data/small
//   a   sum:pan  sum:wye
//   pan 0.346790 0.502626
//
//   a   sum:pan  sum:wye  sum:zee
//   eks 0.758680 0.381399 0.611784
//
//   a   sum:wye  sum:pan
//   wye 0.204603 0.573289
//
//   a   sum:pan  sum:wye
//   zee 0.527126 0.598554
//
//   a   sum:wye
//   hat 0.031442

void mlhmmv_to_lrecs(mlhmmv_t* pmap, sllmv_t* pkeys, sllmv_t* pnames, sllv_t* poutrecs, int do_full_prefixing,
	char* flatten_separator)
{
	mv_t* pfirstkey = &pkeys->phead->value;

	mlhmmv_level_entry_t* ptop_entry = mlhmmv_get_entry_at_level(pmap->proot_level, pkeys->phead, NULL);
	if (ptop_entry == NULL) {
		// No such entry in the mlhmmv results in no output records
	} else if (ptop_entry->level_value.is_terminal) {
		// E.g. '@v = 3' at the top level of the mlhmmv.
		lrec_t* poutrec = lrec_unbacked_alloc();
		lrec_put(poutrec,
			mv_alloc_format_val(pfirstkey),
			mv_alloc_format_val(&ptop_entry->level_value.u.mlrval), FREE_ENTRY_KEY|FREE_ENTRY_VALUE);
		sllv_append(poutrecs, poutrec);
	} else {
		// E.g. '@v = {...}' at the top level of the mlhmmv: the map value keyed by oosvar-name 'v' is itself a hashmap.
		// This needs to be flattened down to an lrec which is a list of key-value pairs.  We recursively invoke
		// mlhmmv_to_lrecs_aux_across_records for each of the name-list entries, one map level deeper each call, then
		// from there invoke mlhmmv_to_lrecs_aux_within_record on any remaining map levels.
		lrec_t* ptemplate = lrec_unbacked_alloc();
		char* oosvar_name = mv_alloc_format_val(pfirstkey);
		mlhmmv_to_lrecs_aux_across_records(ptop_entry->level_value.u.pnext_level, oosvar_name, pnames->phead,
			ptemplate, poutrecs, do_full_prefixing, flatten_separator);
		free(oosvar_name);
		lrec_free(ptemplate);
	}
}

static void mlhmmv_to_lrecs_aux_across_records(
	mlhmmv_level_t* plevel,
	char*           prefix,
	sllmve_t*       prestnames,
	lrec_t*         ptemplate,
	sllv_t*         poutrecs,
	int             do_full_prefixing,
	char*           flatten_separator)
{
	if (prestnames != NULL) {
		// If there is a namelist entry, pull it out to its own field on the output lrecs.
		for (mlhmmv_level_entry_t* pe = plevel->phead; pe != NULL; pe = pe->pnext) {
			mlhmmv_value_t* plevel_value = &pe->level_value;
			lrec_t* pnextrec = lrec_copy(ptemplate);
			lrec_put(pnextrec,
				mv_alloc_format_val(&prestnames->value),
				mv_alloc_format_val(&pe->level_key), FREE_ENTRY_KEY|FREE_ENTRY_VALUE);
			if (plevel_value->is_terminal) {
				lrec_put(pnextrec,
					mlr_strdup_or_die(prefix),
					mv_alloc_format_val(&plevel_value->u.mlrval), FREE_ENTRY_KEY|FREE_ENTRY_VALUE);
				sllv_append(poutrecs, pnextrec);
			} else {
				mlhmmv_to_lrecs_aux_across_records(pe->level_value.u.pnext_level,
					prefix, prestnames->pnext, pnextrec, poutrecs, do_full_prefixing, flatten_separator);
				lrec_free(pnextrec);
			}
		}

	} else {
		// If there are no more remaining namelist entries, flatten remaining map levels using the join separator
		// (default ":") and use them to create lrec values.
		lrec_t* pnextrec = lrec_copy(ptemplate);
		int emit = TRUE;
		for (mlhmmv_level_entry_t* pe = plevel->phead; pe != NULL; pe = pe->pnext) {
			mlhmmv_value_t* plevel_value = &pe->level_value;
			if (plevel_value->is_terminal) {
				char* temp = mv_alloc_format_val(&pe->level_key);
				char* next_prefix = do_full_prefixing
					? mlr_paste_3_strings(prefix, flatten_separator, temp)
					: mlr_strdup_or_die(temp);
				free(temp);
				lrec_put(pnextrec,
					next_prefix,
					mv_alloc_format_val(&plevel_value->u.mlrval),
					FREE_ENTRY_KEY|FREE_ENTRY_VALUE);
			} else if (do_full_prefixing) {
				char* temp = mv_alloc_format_val(&pe->level_key);
				char* next_prefix = mlr_paste_3_strings(prefix, flatten_separator, temp);
				free(temp);
				mlhmmv_to_lrecs_aux_within_record(plevel_value->u.pnext_level, next_prefix, pnextrec,
					do_full_prefixing, flatten_separator);
				free(next_prefix);
			} else {
				mlhmmv_to_lrecs_aux_across_records(pe->level_value.u.pnext_level,
					prefix, NULL, pnextrec, poutrecs, do_full_prefixing, flatten_separator);
				emit = FALSE;
			}
		}
		if (emit)
			sllv_append(poutrecs, pnextrec);
		else
			lrec_free(pnextrec);
	}
}

static void mlhmmv_to_lrecs_aux_within_record(
	mlhmmv_level_t* plevel,
	char*           prefix,
	lrec_t*         poutrec,
	int             do_full_prefixing,
	char*           flatten_separator)
{
	for (mlhmmv_level_entry_t* pe = plevel->phead; pe != NULL; pe = pe->pnext) {
		mlhmmv_value_t* plevel_value = &pe->level_value;
		char* temp = mv_alloc_format_val(&pe->level_key);
		char* next_prefix = do_full_prefixing
			? mlr_paste_3_strings(prefix, flatten_separator, temp)
			: mlr_strdup_or_die(temp);
		free(temp);
		if (plevel_value->is_terminal) {
			lrec_put(poutrec,
				next_prefix,
				mv_alloc_format_val(&plevel_value->u.mlrval),
				FREE_ENTRY_KEY|FREE_ENTRY_VALUE);
		} else {
			mlhmmv_to_lrecs_aux_within_record(plevel_value->u.pnext_level, next_prefix, poutrec,
				do_full_prefixing, flatten_separator);
			free(next_prefix);
		}
	}
}

// ----------------------------------------------------------------
void mlhmmv_to_lrecs_lashed(mlhmmv_t* pmap, sllmv_t** ppkeys, int num_keylists, sllmv_t* pnames,
	sllv_t* poutrecs, int do_full_prefixing, char* flatten_separator)
{
	mlhmmv_level_entry_t** ptop_entries = mlr_malloc_or_die(num_keylists * sizeof(mlhmmv_level_entry_t*));
	for (int i = 0; i < num_keylists; i++) {
		ptop_entries[i] = mlhmmv_get_entry_at_level(pmap->proot_level, ppkeys[i]->phead, NULL);
	}

	// First is primary and rest are lashed to it (lookups with same keys as primary).
	if (ptop_entries[0] == NULL) {
		// No such entry in the mlhmmv results in no output records
	} else if (ptop_entries[0]->level_value.is_terminal) {
		lrec_t* poutrec = lrec_unbacked_alloc();
		for (int i = 0; i < num_keylists; i++) {
			// E.g. '@v = 3' at the top level of the mlhmmv.
			if (ptop_entries[i]->level_value.is_terminal) {
				lrec_put(poutrec,
					mv_alloc_format_val(&ppkeys[i]->phead->value),
					mv_alloc_format_val(&ptop_entries[i]->level_value.u.mlrval), FREE_ENTRY_KEY|FREE_ENTRY_VALUE);
			}
		}
		sllv_append(poutrecs, poutrec);
	} else {
		// E.g. '@v = {...}' at the top level of the mlhmmv: the map value keyed by
		// oosvar-name 'v' is itself a hashmap.  This needs to be flattened down to an
		// lrec which is a list of key-value pairs.  We recursively invoke
		// mlhmmv_to_lrecs_aux_across_records_lashed for each of the name-list entries,
		// one map level deeper each call, then from there invoke
		// mlhmmv_to_lrecs_aux_within_record_lashed on any remaining map levels.

		lrec_t* ptemplate = lrec_unbacked_alloc();

		mlhmmv_level_t** ppnext_levels = mlr_malloc_or_die(num_keylists * sizeof(mlhmmv_level_t*));
		char** oosvar_names = mlr_malloc_or_die(num_keylists * sizeof(char*));
		for (int i = 0; i < num_keylists; i++) {
			if (ptop_entries[i] == NULL || ptop_entries[i]->level_value.is_terminal) {
				ppnext_levels[i] = NULL;
				oosvar_names[i] = NULL;
			} else {
				ppnext_levels[i] = ptop_entries[i]->level_value.u.pnext_level;
				oosvar_names[i] = mv_alloc_format_val(&ppkeys[i]->phead->value);
			}
		}

		mlhmmv_to_lrecs_aux_across_records_lashed(ppnext_levels, oosvar_names, num_keylists,
			pnames->phead, ptemplate, poutrecs, do_full_prefixing, flatten_separator);

		for (int i = 0; i < num_keylists; i++) {
			free(oosvar_names[i]);
		}
		free(oosvar_names);
		free(ppnext_levels);

		lrec_free(ptemplate);
	}

	free(ptop_entries);
}

static void mlhmmv_to_lrecs_aux_across_records_lashed(
	mlhmmv_level_t** pplevels,
	char**           prefixes,
	int              num_levels,
	sllmve_t*        prestnames,
	lrec_t*          ptemplate,
	sllv_t*          poutrecs,
	int              do_full_prefixing,
	char*            flatten_separator)
{
	if (prestnames != NULL) {
		// If there is a namelist entry, pull it out to its own field on the output lrecs.
		// First is iterated over and the rest are lashed (lookups with same keys as primary).
		for (mlhmmv_level_entry_t* pe = pplevels[0]->phead; pe != NULL; pe = pe->pnext) {
			mlhmmv_value_t* pfirst_level_value = &pe->level_value;
			lrec_t* pnextrec = lrec_copy(ptemplate);
			lrec_put(pnextrec,
				mv_alloc_format_val(&prestnames->value),
				mv_alloc_format_val(&pe->level_key), FREE_ENTRY_KEY|FREE_ENTRY_VALUE);

			if (pfirst_level_value->is_terminal) {
				for (int i = 0; i < num_levels; i++) {
					mlhmmv_value_t* plevel_value = mlhmmv_get_next_level_entry_value(pplevels[i], &pe->level_key);
					if (plevel_value != NULL && plevel_value->is_terminal) {
						lrec_put(pnextrec,
							mlr_strdup_or_die(prefixes[i]),
							mv_alloc_format_val(&plevel_value->u.mlrval), FREE_ENTRY_KEY|FREE_ENTRY_VALUE);
					}
				}
				sllv_append(poutrecs, pnextrec);
			} else {
				mlhmmv_level_t** ppnext_levels = mlr_malloc_or_die(num_levels * sizeof(mlhmmv_level_t*));
				for (int i = 0; i < num_levels; i++) {
					mlhmmv_value_t* plevel_value = mlhmmv_get_next_level_entry_value(pplevels[i], &pe->level_key);
					if (plevel_value == NULL || plevel_value->is_terminal) {
						ppnext_levels[i] = NULL;
					} else {
						ppnext_levels[i] = plevel_value->u.pnext_level;
					}
				}

				mlhmmv_to_lrecs_aux_across_records_lashed(ppnext_levels, prefixes, num_levels,
					prestnames->pnext, pnextrec, poutrecs, do_full_prefixing, flatten_separator);

				free(ppnext_levels);
				lrec_free(pnextrec);
			}
		}

	} else {
		// If there are no more remaining namelist entries, flatten remaining map levels using the join separator
		// (default ":") and use them to create lrec values.
		lrec_t* pnextrec = lrec_copy(ptemplate);
		int emit = TRUE;
		for (mlhmmv_level_entry_t* pe = pplevels[0]->phead; pe != NULL; pe = pe->pnext) {
			if (pe->level_value.is_terminal) {
				char* temp = mv_alloc_format_val(&pe->level_key);
				for (int i = 0; i < num_levels; i++) {
					mlhmmv_value_t* plevel_value = mlhmmv_get_next_level_entry_value(pplevels[i], &pe->level_key);
					if (plevel_value != NULL && plevel_value->is_terminal) {
						char* name = do_full_prefixing
							? mlr_paste_3_strings(prefixes[i], flatten_separator, temp)
							: mlr_strdup_or_die(temp);
						lrec_put(pnextrec,
							name,
							mv_alloc_format_val(&plevel_value->u.mlrval),
							FREE_ENTRY_KEY|FREE_ENTRY_VALUE);
					}
				}
				free(temp);
			} else if (do_full_prefixing) {
				char* temp = mv_alloc_format_val(&pe->level_key);
				mlhmmv_level_t** ppnext_levels = mlr_malloc_or_die(num_levels * sizeof(mlhmmv_level_t*));
				char** next_prefixes = mlr_malloc_or_die(num_levels * sizeof(char*));
				for (int i = 0; i < num_levels; i++) {
					mlhmmv_value_t* plevel_value = mlhmmv_get_next_level_entry_value(pplevels[i], &pe->level_key);
					if (plevel_value != NULL && !plevel_value->is_terminal) {
						ppnext_levels[i] = plevel_value->u.pnext_level;
						next_prefixes[i] = mlr_paste_3_strings(prefixes[i], flatten_separator, temp);
					} else {
						ppnext_levels[i] = NULL;
						next_prefixes[i] = NULL;
					}
					mlhmmv_to_lrecs_aux_within_record_lashed(ppnext_levels, next_prefixes, num_levels,
						pnextrec, do_full_prefixing, flatten_separator);
				}
				for (int i = 0; i < num_levels; i++) {
					free(next_prefixes[i]);
				}
				free(next_prefixes);
				free(ppnext_levels);
				free(temp);
			} else {
				mlhmmv_level_t** ppnext_levels = mlr_malloc_or_die(num_levels * sizeof(mlhmmv_level_t*));
				for (int i = 0; i < num_levels; i++) {
					mlhmmv_value_t* plevel_value = mlhmmv_get_next_level_entry_value(pplevels[i], &pe->level_key);
					if (plevel_value->is_terminal) {
						ppnext_levels[i] = NULL;
					} else {
						ppnext_levels[i] = plevel_value->u.pnext_level;
					}
				}

				mlhmmv_to_lrecs_aux_across_records_lashed(ppnext_levels, prefixes, num_levels,
					NULL, pnextrec, poutrecs, do_full_prefixing, flatten_separator);

				free(ppnext_levels);
				emit = FALSE;
			}
		}
		if (emit)
			sllv_append(poutrecs, pnextrec);
		else
			lrec_free(pnextrec);
	}
}

static void mlhmmv_to_lrecs_aux_within_record_lashed(
	mlhmmv_level_t** pplevels,
	char**           prefixes,
	int              num_levels,
	lrec_t*          poutrec,
	int              do_full_prefixing,
	char*            flatten_separator)
{
	for (mlhmmv_level_entry_t* pe = pplevels[0]->phead; pe != NULL; pe = pe->pnext) {
		mlhmmv_value_t* pfirst_level_value = &pe->level_value;
		char* temp = mv_alloc_format_val(&pe->level_key);
		if (pfirst_level_value->is_terminal) {
			for (int i = 0; i < num_levels; i++) {
				mlhmmv_value_t* plevel_value = mlhmmv_get_next_level_entry_value(pplevels[i], &pe->level_key);
				if (plevel_value != NULL && plevel_value->is_terminal) {
					char* name = do_full_prefixing
						? mlr_paste_3_strings(prefixes[i], flatten_separator, temp)
						: mlr_strdup_or_die(temp);
					lrec_put(poutrec,
						name,
						mv_alloc_format_val(&plevel_value->u.mlrval),
						FREE_ENTRY_KEY|FREE_ENTRY_VALUE);
				}
			}
		} else {
			mlhmmv_level_t** ppnext_levels = mlr_malloc_or_die(num_levels * sizeof(mlhmmv_level_t*));
			char** next_prefixes = mlr_malloc_or_die(num_levels * sizeof(char*));
			for (int i = 0; i < num_levels; i++) {
				mlhmmv_value_t* plevel_value = mlhmmv_get_next_level_entry_value(pplevels[i], &pe->level_key);
				if (plevel_value->is_terminal) {
					ppnext_levels[i] = NULL;
					next_prefixes[i] = NULL;
				} else {
					ppnext_levels[i] = plevel_value->u.pnext_level;
					next_prefixes[i] = do_full_prefixing
						? mlr_paste_3_strings(prefixes[i], flatten_separator, temp)
						: mlr_strdup_or_die(temp);
				}
			}

			mlhmmv_to_lrecs_aux_within_record_lashed(ppnext_levels, next_prefixes, num_levels,
				poutrec, do_full_prefixing, flatten_separator);

			for (int i = 0; i < num_levels; i++) {
				free(next_prefixes[i]);
			}
			free(next_prefixes);
			free(ppnext_levels);
		}
		free(temp);
	}
}

// ----------------------------------------------------------------
// This is simply JSON. Example output:
// {
//   "0":  {
//     "fghij":  {
//       "0":  17
//     }
//   },
//   "3":  4,
//   "abcde":  {
//     "-6":  7
//   }
// }

void mlhmmv_print_json_stacked(mlhmmv_t* pmap, int quote_values_always, char* line_indent, FILE* ostream) {
	mlhmmv_level_print_stacked(pmap->proot_level, 0, FALSE, quote_values_always, line_indent, ostream);
}

void mlhmmv_print_terminal(mv_t* pmv, int quote_values_always, FILE* ostream) {
	char* level_value_string = mv_alloc_format_val(pmv);

	if (quote_values_always) {
		fprintf(ostream, "\"%s\"", level_value_string);
	} else if (pmv->type == MT_STRING) {
		double unused;
		if (mlr_try_float_from_string(level_value_string, &unused))
			json_decimal_print(level_value_string, ostream);
		else if (streq(level_value_string, "true") || streq(level_value_string, "false"))
			fprintf(ostream, "%s", level_value_string);
		else
			fprintf(ostream, "\"%s\"", level_value_string);
	} else {
		fprintf(ostream, "%s", level_value_string);
	}
}

void mlhmmv_level_print_stacked(mlhmmv_level_t* plevel, int depth,
	int do_final_comma, int quote_values_always, char* line_indent, FILE* ostream)
{
	static char* leader = "  ";
	// Top-level opening brace goes on a line by itself; subsequents on the same line after the level key.
	if (depth == 0)
		fprintf(ostream, "%s{\n", line_indent);
	for (mlhmmv_level_entry_t* pentry = plevel->phead; pentry != NULL; pentry = pentry->pnext) {
		fprintf(ostream, "%s", line_indent);
		for (int i = 0; i <= depth; i++)
			fprintf(ostream, "%s", leader);
		char* level_key_string = mv_alloc_format_val(&pentry->level_key);
			fprintf(ostream, "\"%s\": ", level_key_string);
		free(level_key_string);

		if (pentry->level_value.is_terminal) {
			mlhmmv_print_terminal(&pentry->level_value.u.mlrval, quote_values_always, ostream);

			if (pentry->pnext != NULL)
				fprintf(ostream, ",\n");
			else
				fprintf(ostream, "\n");
		} else {
			fprintf(ostream, "%s{\n", line_indent);
			mlhmmv_level_print_stacked(pentry->level_value.u.pnext_level, depth + 1,
				pentry->pnext != NULL, quote_values_always, line_indent, ostream);
		}
	}
	for (int i = 0; i < depth; i++)
		fprintf(ostream, "%s", leader);
	if (do_final_comma)
		fprintf(ostream, "%s},\n", line_indent);
	else
		fprintf(ostream, "%s}\n", line_indent);
}

// ----------------------------------------------------------------
void mlhmmv_print_json_single_line(mlhmmv_t* pmap, int quote_values_always, FILE* ostream) {
	mlhmmv_level_print_single_line(pmap->proot_level, 0, FALSE, quote_values_always, ostream);
	fprintf(ostream, "\n");
}

static void mlhmmv_level_print_single_line(mlhmmv_level_t* plevel, int depth,
	int do_final_comma, int quote_values_always, FILE* ostream)
{
	// Top-level opening brace goes on a line by itself; subsequents on the same line after the level key.
	if (depth == 0)
		fprintf(ostream, "{ ");
	for (mlhmmv_level_entry_t* pentry = plevel->phead; pentry != NULL; pentry = pentry->pnext) {
		char* level_key_string = mv_alloc_format_val(&pentry->level_key);
			fprintf(ostream, "\"%s\": ", level_key_string);
		free(level_key_string);

		if (pentry->level_value.is_terminal) {
			char* level_value_string = mv_alloc_format_val(&pentry->level_value.u.mlrval);

			if (quote_values_always) {
				fprintf(ostream, "\"%s\"", level_value_string);
			} else if (pentry->level_value.u.mlrval.type == MT_STRING) {
				double unused;
				if (mlr_try_float_from_string(level_value_string, &unused))
					json_decimal_print(level_value_string, ostream);
				else if (streq(level_value_string, "true") || streq(level_value_string, "false"))
					fprintf(ostream, "%s", level_value_string);
				else
					fprintf(ostream, "\"%s\"", level_value_string);
			} else {
				fprintf(ostream, "%s", level_value_string);
			}

			free(level_value_string);
			if (pentry->pnext != NULL)
				fprintf(ostream, ", ");
		} else {
			fprintf(ostream, "{");
			mlhmmv_level_print_single_line(pentry->level_value.u.pnext_level, depth + 1,
				pentry->pnext != NULL, quote_values_always, ostream);
		}
	}
	if (do_final_comma)
		fprintf(ostream, " },");
	else
		fprintf(ostream, " }");
}

// ----------------------------------------------------------------
// 0.123 is valid JSON; .123 is not. Meanwhile is a format-converter tool so if there is
// perfectly legitimate CSV/DKVP/etc. data to be JSON-formatted, we make it JSON-compliant.
//
// Precondition: the caller has already checked that the string represents a number.
static void json_decimal_print(char* s, FILE* ostream) {
	if (s[0] == '.') {
		fprintf(ostream, "0%s", s);
	} else if (s[0] == '-' && s[1] == '.') {
		fprintf(ostream, "-0.%s", &s[2]);
	} else {
		fprintf(ostream, "%s", s);
	}
}

// ----------------------------------------------------------------
typedef int mlhmmv_typed_hash_func(mv_t* pa);

static int mlhmmv_string_hash_func(mv_t* pa) {
	return mlr_string_hash_func(pa->u.strv);
}
static int mlhmmv_int_hash_func(mv_t* pa) {
	return pa->u.intv;
}
static int mlhmmv_other_hash_func(mv_t* pa) {
	fprintf(stderr, "%s: map keys must be of type %s or %s; got %s.\n",
		MLR_GLOBALS.bargv0,
		mt_describe_type(MT_STRING),
		mt_describe_type(MT_INT),
		mt_describe_type(pa->type));
	exit(1);
}
static mlhmmv_typed_hash_func* mlhmmv_hash_func_dispositions[MT_DIM] = {
	/*ERROR*/  mlhmmv_other_hash_func,
	/*ABSENT*/ mlhmmv_other_hash_func,
	/*EMPTY*/  mlhmmv_other_hash_func,
	/*STRING*/ mlhmmv_string_hash_func,
	/*INT*/    mlhmmv_int_hash_func,
	/*FLOAT*/  mlhmmv_other_hash_func,
	/*BOOL*/   mlhmmv_other_hash_func,
};

static int mlhmmv_hash_func(mv_t* pa) {
	return mlhmmv_hash_func_dispositions[pa->type](pa);
}
