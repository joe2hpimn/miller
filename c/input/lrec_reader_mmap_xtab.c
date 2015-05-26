#include <stdio.h>
#include <stdlib.h>
#include "lib/mlrutil.h"
#include "input/file_reader_mmap.h"
#include "input/lrec_readers.h"

typedef struct _lrec_reader_mmap_xtab_state_t {
	char irs;
	char ips; // xxx make me real
	int allow_repeat_ips;
	int at_eof;
} lrec_reader_mmap_xtab_state_t;

// ----------------------------------------------------------------
static lrec_t* lrec_reader_mmap_xtab_process(file_reader_mmap_state_t* phandle, void* pvstate, context_t* pctx) {
	lrec_reader_mmap_xtab_state_t* pstate = pvstate;

	if (pstate->at_eof)
		return NULL;
	else
		return lrec_parse_mmap_xtab(phandle, pstate->irs, pstate->ips, pstate->allow_repeat_ips);
}

static void lrec_reader_mmap_xtab_sof(void* pvstate) {
	lrec_reader_mmap_xtab_state_t* pstate = pvstate;
	pstate->at_eof = FALSE;
}

lrec_reader_mmap_t* lrec_reader_mmap_xtab_alloc(char irs, char ips, int allow_repeat_ips) {
	lrec_reader_mmap_t* plrec_reader_mmap = mlr_malloc_or_die(sizeof(lrec_reader_mmap_t));

	lrec_reader_mmap_xtab_state_t* pstate = mlr_malloc_or_die(sizeof(lrec_reader_mmap_xtab_state_t));
	//pstate->ips              = ips;
	//pstate->allow_repeat_ips = allow_repeat_ips;
	pstate->irs                 = irs;
	pstate->ips                 = ' ';
	pstate->allow_repeat_ips    = TRUE;
	pstate->at_eof              = FALSE;

	plrec_reader_mmap->pvstate       = (void*)pstate;
	plrec_reader_mmap->pprocess_func = &lrec_reader_mmap_xtab_process;
	plrec_reader_mmap->psof_func     = &lrec_reader_mmap_xtab_sof;

	return plrec_reader_mmap;
}

// ----------------------------------------------------------------
lrec_t* lrec_parse_mmap_xtab(file_reader_mmap_state_t* phandle, char irs, char ips, int allow_repeat_ips) {

	if (phandle->sol >= phandle->eof)
		return NULL;

	lrec_t* prec = lrec_unbacked_alloc();
	while (*phandle->sol == irs)
		phandle->sol++;

	if (phandle->sol >= phandle->eof)
		return NULL;

	// Loop over fields, one per line
	while (TRUE) {
		char* line  = phandle->sol;
		char* key   = line;
		char* value = "";
		char* eol   = NULL;
		char* p;

		// Construct one field
		for (p = line; *p; ) {
			if (*p == irs) {
				*p = 0;
				eol = p;
				phandle->sol = p+1;
				break;
			} else if (*p == ips) {
				key = line;
				*p = 0;

				p++;
				if (allow_repeat_ips) {
					while (*p == ips)
						p++;
				}
				value = p;
			} else {
				p++;
			}
		}

		lrec_put_no_free(prec, key, value);

		if (phandle->sol >= phandle->eof || *phandle->sol == irs)
			break;
	}
	if (prec->field_count == 0) {
		lrec_free(prec);
		return NULL;
	} else {
		return prec;
	}
}
