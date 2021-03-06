#include <stdlib.h>
#include "lib/mlr_globals.h"
#include "lib/mlrutil.h"
#include "mlr_dsl_cst.h"
#include "context_flags.h"

// ----------------------------------------------------------------
static file_output_mode_t file_output_mode_from_ast_node_type(mlr_dsl_ast_node_type_t mlr_dsl_ast_node_type) {
	switch(mlr_dsl_ast_node_type) {
	case MD_AST_NODE_TYPE_FILE_APPEND:
		return MODE_APPEND;
	case MD_AST_NODE_TYPE_PIPE:
		return MODE_PIPE;
	case MD_AST_NODE_TYPE_FILE_WRITE:
		return MODE_WRITE;
	default:
		MLR_INTERNAL_CODING_ERROR();
		return MODE_WRITE; // not reached
	}
}

// ================================================================
typedef struct _tee_state_t {
	FILE*                stdfp;
	file_output_mode_t   file_output_mode;
	rval_evaluator_t*    poutput_filename_evaluator;
	int                  flush_every_record;
	lrec_writer_t*       psingle_lrec_writer;
	multi_lrec_writer_t* pmulti_lrec_writer;
} tee_state_t;

static mlr_dsl_cst_statement_handler_t handle_tee_to_stdfp;
static mlr_dsl_cst_statement_handler_t handle_tee_to_file;
static mlr_dsl_cst_statement_freer_t free_tee;

static lrec_t* handle_tee_common(
	tee_state_t*   pstate,
	variables_t*   pvars,
	cst_outputs_t* pcst_outputs);

// ----------------------------------------------------------------
mlr_dsl_cst_statement_t* alloc_tee(mlr_dsl_cst_t* pcst, mlr_dsl_ast_node_t* pnode,
	int type_inferencing, int context_flags)
{
	tee_state_t* pstate = mlr_malloc_or_die(sizeof(tee_state_t));

	pstate->stdfp                      = NULL;
	pstate->poutput_filename_evaluator = NULL;
	pstate->psingle_lrec_writer        = NULL;
	pstate->pmulti_lrec_writer         = NULL;

	mlr_dsl_ast_node_t* poutput_node = pnode->pchildren->phead->pvvalue;
	mlr_dsl_ast_node_t* pfilename_node = poutput_node->pchildren->phead->pvvalue;

	pstate->flush_every_record = pcst->flush_every_record;
	if (pfilename_node->type == MD_AST_NODE_TYPE_STDOUT || pfilename_node->type == MD_AST_NODE_TYPE_STDERR) {
		pstate->stdfp = (pfilename_node->type == MD_AST_NODE_TYPE_STDOUT) ? stdout : stderr;

		return mlr_dsl_cst_statement_valloc(
			pnode,
			handle_tee_to_stdfp,
			free_tee,
			pstate);

	} else {
		pstate->poutput_filename_evaluator = rval_evaluator_alloc_from_ast(pfilename_node, pcst->pfmgr,
			type_inferencing, context_flags);
		pstate->file_output_mode = file_output_mode_from_ast_node_type(poutput_node->type);

		return mlr_dsl_cst_statement_valloc(
			pnode,
			handle_tee_to_file,
			free_tee,
			pstate);
	}
}

// ----------------------------------------------------------------
static void handle_tee_to_stdfp(
	mlr_dsl_cst_statement_t* pstatement,
	variables_t*             pvars,
	cst_outputs_t*           pcst_outputs)
{
	tee_state_t* pstate = pstatement->pvstate;

	// The opts aren't complete at alloc time so we need to handle them on first use.
	if (pstate->psingle_lrec_writer == NULL)
		pstate->psingle_lrec_writer = lrec_writer_alloc_or_die(pcst_outputs->pwriter_opts);

	lrec_t* pcopy = handle_tee_common(pstate, pvars, pcst_outputs);

	// The writer frees the lrec
	pstate->psingle_lrec_writer->pprocess_func(pstate->psingle_lrec_writer->pvstate,
		pstate->stdfp, pcopy);
	if (pstate->flush_every_record)
		fflush(pstate->stdfp);
}

// ----------------------------------------------------------------
static void handle_tee_to_file(
	mlr_dsl_cst_statement_t* pstatement,
	variables_t*             pvars,
	cst_outputs_t*           pcst_outputs)
{
	tee_state_t* pstate = pstatement->pvstate;

	// The opts aren't complete at alloc time so we need to handle them on first use.
	if (pstate->pmulti_lrec_writer == NULL)
		pstate->pmulti_lrec_writer = multi_lrec_writer_alloc(pcst_outputs->pwriter_opts);

	rval_evaluator_t* poutput_filename_evaluator = pstate->poutput_filename_evaluator;
	mv_t filename_mv = poutput_filename_evaluator->pprocess_func(poutput_filename_evaluator->pvstate, pvars);

	lrec_t* pcopy = handle_tee_common(pstate, pvars, pcst_outputs);

	char fn_free_flags = 0;
	char* filename = mv_format_val(&filename_mv, &fn_free_flags);
	// The writer frees the lrec
	multi_lrec_writer_output_srec(pstate->pmulti_lrec_writer, pcopy, filename,
		pstate->file_output_mode, pstate->flush_every_record);

	if (fn_free_flags)
		free(filename);
	mv_free(&filename_mv);
}

// ----------------------------------------------------------------
static lrec_t* handle_tee_common(
	tee_state_t*   pstate,
	variables_t*   pvars,
	cst_outputs_t* pcst_outputs)
{

	lrec_t* pcopy = lrec_copy(pvars->pinrec);

	// Write the output fields from the typed overlay back to the lrec.
	for (lhmsmve_t* pe = pvars->ptyped_overlay->phead; pe != NULL; pe = pe->pnext) {
		char* output_field_name = pe->key;
		mv_t* pval = &pe->value;

		// Ownership transfer from mv_t to lrec.
		if (pval->type == MT_STRING || pval->type == MT_EMPTY) {
			lrec_put(pcopy, output_field_name, mlr_strdup_or_die(pval->u.strv), FREE_ENTRY_VALUE);
		} else {
			char free_flags = NO_FREE;
			char* string = mv_format_val(pval, &free_flags);
			lrec_put(pcopy, output_field_name, string, free_flags);
		}
	}
	return pcopy;
}

// ----------------------------------------------------------------
static void free_tee(mlr_dsl_cst_statement_t* pstatement) { // xxx from mlr_dsl_cst_statement_free
	tee_state_t* pstate = pstatement->pvstate;

	if (pstate->poutput_filename_evaluator != NULL) {
		pstate->poutput_filename_evaluator->pfree_func(pstate->poutput_filename_evaluator);
	}

	if (pstate->psingle_lrec_writer != NULL) {
		pstate->psingle_lrec_writer->pfree_func(pstate->psingle_lrec_writer);
	}

	if (pstate->pmulti_lrec_writer != NULL) {
		multi_lrec_writer_drain(pstate->pmulti_lrec_writer);
		multi_lrec_writer_free(pstate->pmulti_lrec_writer);
	}

	free(pstate);
}

// ================================================================
// Most statements have one item, except emit and emitf.
struct _emitf_item_t;
typedef void emitf_item_handler_t(
	struct _emitf_item_t* pemitf_item,
	variables_t*          pvars,
	cst_outputs_t*        pcst_outputs);

typedef struct _emitf_item_t {
	emitf_item_handler_t* pemitf_item_handler;
	char*                 srec_field_name;
	rval_evaluator_t*     parg_evaluator;

} emitf_item_t;

static emitf_item_t* alloc_blank_emitf_item(char* srec_field_name, rval_evaluator_t* parg_evaluator);
static void free_emitf_item(emitf_item_t* pemitf_item);

// ----------------------------------------------------------------
typedef struct _emitf_state_t {
	FILE*                stdfp;
	file_output_mode_t   file_output_mode;
	rval_evaluator_t*    poutput_filename_evaluator;
	int                  flush_every_record;
	lrec_writer_t*       psingle_lrec_writer;
	multi_lrec_writer_t* pmulti_lrec_writer;
	sllv_t*              pemitf_items;
} emitf_state_t;

static mlr_dsl_cst_statement_handler_t handle_emitf;
static mlr_dsl_cst_statement_freer_t free_emitf;

static void handle_emitf(
	mlr_dsl_cst_statement_t* pstatement,
	variables_t*             pvars,
	cst_outputs_t*           pcst_outputs);

static void handle_emitf_to_stdfp(
	mlr_dsl_cst_statement_t* pstatement,
	variables_t*             pvars,
	cst_outputs_t*           pcst_outputs);

static void handle_emitf_to_file(
	mlr_dsl_cst_statement_t* pstatement,
	variables_t*             pvars,
	cst_outputs_t*           pcst_outputs);

static void handle_emitf_common(
	emitf_state_t* pstate,
	variables_t*   pvars,
	sllv_t*        poutrecs);

// ----------------------------------------------------------------
mlr_dsl_cst_statement_t* alloc_emitf(mlr_dsl_cst_t* pcst, mlr_dsl_ast_node_t* pnode,
	int type_inferencing, int context_flags)
{
	emitf_state_t* pstate = mlr_malloc_or_die(sizeof(emitf_state_t));

	pstate->stdfp                      = NULL;
	pstate->poutput_filename_evaluator = NULL;
	pstate->psingle_lrec_writer        = NULL;
	pstate->pmulti_lrec_writer         = NULL;
	pstate->pemitf_items               = NULL;

	mlr_dsl_ast_node_t* pnamesnode = pnode->pchildren->phead->pvvalue;

	// Loop over oosvar names to emit in e.g. 'emitf @a, @b, @c'.
	pstate->pemitf_items = sllv_alloc();
	for (sllve_t* pe = pnamesnode->pchildren->phead; pe != NULL; pe = pe->pnext) {
		mlr_dsl_ast_node_t* pwalker = pe->pvvalue;
		mlr_dsl_ast_node_t* pchild = pwalker->pchildren->phead->pvvalue;
		// This could be enforced in the lemon parser but it's easier to do it here.
		sllv_append(pstate->pemitf_items,
			alloc_blank_emitf_item(
				pchild->text,
				rval_evaluator_alloc_from_ast(pwalker, pcst->pfmgr, type_inferencing, context_flags)));
	}

	mlr_dsl_ast_node_t* poutput_node = pnode->pchildren->phead->pnext->pvvalue;
	mlr_dsl_ast_node_t* pfilename_node = poutput_node->pchildren == NULL
		? NULL
		: poutput_node->pchildren->phead == NULL
		? NULL
		: poutput_node->pchildren->phead->pvvalue;
	mlr_dsl_cst_statement_handler_t* phandler = NULL;
	if (poutput_node->type == MD_AST_NODE_TYPE_STREAM) {
		phandler = handle_emitf;
	} else if (pfilename_node->type == MD_AST_NODE_TYPE_STDOUT || pfilename_node->type == MD_AST_NODE_TYPE_STDERR) {
		pstate->stdfp = (pfilename_node->type == MD_AST_NODE_TYPE_STDOUT) ? stdout : stderr;
		phandler = handle_emitf_to_stdfp;
	} else {
		pstate->poutput_filename_evaluator = rval_evaluator_alloc_from_ast(pfilename_node, pcst->pfmgr,
			type_inferencing, context_flags);
		pstate->file_output_mode = file_output_mode_from_ast_node_type(poutput_node->type);
		phandler = handle_emitf_to_file;
	}
	pstate->flush_every_record = pcst->flush_every_record;

	return mlr_dsl_cst_statement_valloc(
		pnode,
		phandler,
		free_emitf,
		pstate);
}

static emitf_item_t* alloc_blank_emitf_item(char* srec_field_name, rval_evaluator_t* parg_evaluator) {
	emitf_item_t* pemitf_item = mlr_malloc_or_die(sizeof(emitf_item_t));
	pemitf_item->srec_field_name = srec_field_name;
	pemitf_item->parg_evaluator  = parg_evaluator;
	return pemitf_item;
}

static void free_emitf_item(emitf_item_t* pemitf_item) {
	pemitf_item->parg_evaluator->pfree_func(pemitf_item->parg_evaluator);
	free(pemitf_item);
}

// ----------------------------------------------------------------
static void handle_emitf(
	mlr_dsl_cst_statement_t* pstatement,
	variables_t*             pvars,
	cst_outputs_t*           pcst_outputs)
{
	emitf_state_t* pstate = pstatement->pvstate;
	handle_emitf_common(pstate, pvars, pcst_outputs->poutrecs);
}

static void handle_emitf_to_stdfp(
	mlr_dsl_cst_statement_t* pstatement,
	variables_t*             pvars,
	cst_outputs_t*           pcst_outputs)
{
	emitf_state_t* pstate = pstatement->pvstate;

	// The opts aren't complete at alloc time so we need to handle them on first use.
	if (pstate->psingle_lrec_writer == NULL)
		pstate->psingle_lrec_writer = lrec_writer_alloc_or_die(pcst_outputs->pwriter_opts);

	sllv_t* poutrecs = sllv_alloc();

	handle_emitf_common(pstate, pvars, poutrecs);

	lrec_writer_print_all(pstate->psingle_lrec_writer, pstate->stdfp, poutrecs);
	if (pstate->flush_every_record)
		fflush(pstate->stdfp);
	sllv_free(poutrecs);
}

static void handle_emitf_to_file(
	mlr_dsl_cst_statement_t* pstatement,
	variables_t*             pvars,
	cst_outputs_t*           pcst_outputs)
{
	emitf_state_t* pstate = pstatement->pvstate;

	// The opts aren't complete at alloc time so we need to handle them on first use.
	if (pstate->pmulti_lrec_writer == NULL)
		pstate->pmulti_lrec_writer = multi_lrec_writer_alloc(pcst_outputs->pwriter_opts);

	rval_evaluator_t* poutput_filename_evaluator = pstate->poutput_filename_evaluator;
	mv_t filename_mv = poutput_filename_evaluator->pprocess_func(poutput_filename_evaluator->pvstate, pvars);

	sllv_t* poutrecs = sllv_alloc();

	handle_emitf_common(pstate, pvars, poutrecs);

	char fn_free_flags = 0;
	char* filename = mv_format_val(&filename_mv, &fn_free_flags);
	multi_lrec_writer_output_list(pstate->pmulti_lrec_writer, poutrecs, filename,
		pstate->file_output_mode, pstate->flush_every_record);

	sllv_free(poutrecs);
	if (fn_free_flags)
		free(filename);
	mv_free(&filename_mv);
}

static void handle_emitf_common(
	emitf_state_t* pstate,
	variables_t*   pvars,
	sllv_t*        poutrecs)
{
	lrec_t* prec_to_emit = lrec_unbacked_alloc();
	for (sllve_t* pf = pstate->pemitf_items->phead; pf != NULL; pf = pf->pnext) {
		emitf_item_t* pemitf_item = pf->pvvalue;
		char* srec_field_name = pemitf_item->srec_field_name;
		rval_evaluator_t* parg_evaluator = pemitf_item->parg_evaluator;

		// This is overkill ... the grammar allows only for oosvar names as args to emit.  So we could bypass
		// that and just hashmap-get keyed by srec_field_name here.
		mv_t val = parg_evaluator->pprocess_func(parg_evaluator->pvstate, pvars);

		if (val.type == MT_STRING) {
			// Ownership transfer from (newly created) mlrval to (newly created) lrec.
			lrec_put(prec_to_emit, srec_field_name, val.u.strv, val.free_flags);
		} else {
			char free_flags = NO_FREE;
			char* string = mv_format_val(&val, &free_flags);
			lrec_put(prec_to_emit, srec_field_name, string, free_flags);
		}

	}
	sllv_append(poutrecs, prec_to_emit);
}

static void free_emitf(mlr_dsl_cst_statement_t* pstatement) { // xxx
	emitf_state_t* pstate = pstatement->pvstate;

	if (pstate->poutput_filename_evaluator != NULL) {
		pstate->poutput_filename_evaluator->pfree_func(pstate->poutput_filename_evaluator);
	}

	if (pstate->psingle_lrec_writer != NULL) {
		pstate->psingle_lrec_writer->pfree_func(pstate->psingle_lrec_writer);
	}

	if (pstate->pmulti_lrec_writer != NULL) {
		multi_lrec_writer_drain(pstate->pmulti_lrec_writer);
		multi_lrec_writer_free(pstate->pmulti_lrec_writer);
	}

	if (pstate->pemitf_items != NULL) {
		for (sllve_t* pe = pstate->pemitf_items->phead; pe != NULL; pe = pe->pnext)
			free_emitf_item(pe->pvvalue);
		sllv_free(pstate->pemitf_items);
	}

	free(pstate);
}

// ================================================================
typedef struct _emit_state_t {
	rval_evaluator_t*  poutput_filename_evaluator;
	FILE*              stdfp;
	file_output_mode_t file_output_mode;
	sllv_t*            pemit_oosvar_namelist_evaluators;
	int                do_full_prefixing;

	// Unlashed emit and emitp; indices ["a", 1, $2] in 'for (k,v in @a[1][$2]) {...}'.
	sllv_t* pemit_keylist_evaluators;

	// Lashed emit and emitp; indices ["a", 1, $2] in 'for (k,v in @a[1][$2]) {...}'.
	int num_emit_keylist_evaluators;
	sllv_t** ppemit_keylist_evaluators;

	lrec_writer_t* psingle_lrec_writer; // emit/tee to stdout/stderr
	multi_lrec_writer_t* pmulti_lrec_writer; // emit-to-file

	int flush_every_record;
} emit_state_t;

static mlr_dsl_cst_statement_handler_t handle_emit;
static mlr_dsl_cst_statement_handler_t handle_emit_to_stdfp;
static mlr_dsl_cst_statement_handler_t handle_emit_to_file;

static void handle_emit_common(
	emit_state_t* pstate,
	variables_t*  pvars,
	sllv_t*       pcst_outputs,
	char*         f);

static mlr_dsl_cst_statement_handler_t handle_emit_lashed;
static mlr_dsl_cst_statement_handler_t handle_emit_lashed_to_stdfp;
static mlr_dsl_cst_statement_handler_t handle_emit_lashed_to_file;
static void handle_emit_lashed_common(
	emit_state_t* pstate,
	variables_t*  pvars,
	sllv_t*       pcst_outputs,
	char*         f);

static mlr_dsl_cst_statement_handler_t handle_emit_all;
static mlr_dsl_cst_statement_handler_t handle_emit_all_to_stdfp;
static mlr_dsl_cst_statement_handler_t handle_emit_all_to_file;

static mlr_dsl_cst_statement_freer_t free_emit;

// ----------------------------------------------------------------
// $ mlr -n put -v 'emit @a[2][3], "x", "y", "z"'
// AST ROOT:
// text="list", type=statement_list:
//     text="emit", type=emit:
//         text="emit", type=emit:
//             text="oosvar_keylist", type=oosvar_keylist:
//                 text="a", type=string_literal.
//                 text="2", type=numeric_literal.
//                 text="3", type=numeric_literal.
//             text="emit_namelist", type=emit:
//                 text="x", type=numeric_literal.
//                 text="y", type=numeric_literal.
//                 text="z", type=numeric_literal.
//         text="stream", type=stream:
//
// $ mlr -n put -v 'emit all, "x", "y", "z"'
// AST ROOT:
// text="list", type=statement_list:
//     text="emit", type=emit:
//         text="emit", type=emit:
//             text="all", type=all.
//             text="emit_namelist", type=emit:
//                 text="x", type=numeric_literal.
//                 text="y", type=numeric_literal.
//                 text="z", type=numeric_literal.
//         text="stream", type=stream:

mlr_dsl_cst_statement_t* alloc_emit(
	mlr_dsl_cst_t*      pcst,
	mlr_dsl_ast_node_t* pnode,
	int                 type_inferencing,
	int                 context_flags,
	int                 do_full_prefixing)
{
	emit_state_t* pstate = mlr_malloc_or_die(sizeof(emit_state_t));

	pstate->poutput_filename_evaluator       = NULL;
	pstate->stdfp                            = NULL;
	pstate->pemit_oosvar_namelist_evaluators = NULL;
	pstate->pemit_keylist_evaluators         = NULL;
	pstate->num_emit_keylist_evaluators      = 0;
	pstate->ppemit_keylist_evaluators        = NULL;
	pstate->psingle_lrec_writer              = NULL;
	pstate->pmulti_lrec_writer               = NULL;

	mlr_dsl_ast_node_t* pemit_node = pnode->pchildren->phead->pvvalue;
	mlr_dsl_ast_node_t* poutput_node = pnode->pchildren->phead->pnext->pvvalue;
	mlr_dsl_ast_node_t* pkeylist_node = pemit_node->pchildren->phead->pvvalue;

	int output_all = FALSE;
	// The grammar allows only 'emit all', not 'emit @x, all, $y'.
	// So if 'all' appears at all, it's the only name.
	if (pkeylist_node->type == MD_AST_NODE_TYPE_ALL || pkeylist_node->type == MD_AST_NODE_TYPE_FULL_OOSVAR) {
		output_all = TRUE;

		sllv_t* pemit_oosvar_namelist_evaluators = sllv_alloc();
		if (pemit_node->pchildren->length == 2) {
			mlr_dsl_ast_node_t* pnamelist_node = pemit_node->pchildren->phead->pnext->pvvalue;
			for (sllve_t* pe = pnamelist_node->pchildren->phead; pe != NULL; pe = pe->pnext) {
				mlr_dsl_ast_node_t* pkeynode = pe->pvvalue;
				sllv_append(pemit_oosvar_namelist_evaluators,
					rval_evaluator_alloc_from_ast(pkeynode, pcst->pfmgr, type_inferencing, context_flags));
			}
		}
		pstate->pemit_oosvar_namelist_evaluators = pemit_oosvar_namelist_evaluators;

	} else if (pkeylist_node->type == MD_AST_NODE_TYPE_OOSVAR_KEYLIST) {

		pstate->pemit_keylist_evaluators = allocate_keylist_evaluators_from_ast_node(
			pkeylist_node, pcst->pfmgr, type_inferencing, context_flags);

		sllv_t* pemit_oosvar_namelist_evaluators = sllv_alloc();
		if (pemit_node->pchildren->length == 2) {
			mlr_dsl_ast_node_t* pnamelist_node = pemit_node->pchildren->phead->pnext->pvvalue;
			for (sllve_t* pe = pnamelist_node->pchildren->phead; pe != NULL; pe = pe->pnext) {
				mlr_dsl_ast_node_t* pkeynode = pe->pvvalue;
				sllv_append(pemit_oosvar_namelist_evaluators,
					rval_evaluator_alloc_from_ast(pkeynode, pcst->pfmgr, type_inferencing, context_flags));
			}
		}
		pstate->pemit_oosvar_namelist_evaluators = pemit_oosvar_namelist_evaluators;

	} else {
		MLR_INTERNAL_CODING_ERROR();
	}

	mlr_dsl_cst_statement_handler_t* phandler = NULL;

	pstate->do_full_prefixing = do_full_prefixing;
	mlr_dsl_ast_node_t* pfilename_node = poutput_node->pchildren == NULL
		? NULL
		: poutput_node->pchildren->phead == NULL
		? NULL
		: poutput_node->pchildren->phead->pvvalue;
	if (poutput_node->type == MD_AST_NODE_TYPE_STREAM) {
		phandler = output_all ? handle_emit_all : handle_emit;
	} else if (pfilename_node->type == MD_AST_NODE_TYPE_STDOUT || pfilename_node->type == MD_AST_NODE_TYPE_STDERR) {
		phandler = output_all ? handle_emit_all_to_stdfp : handle_emit_to_stdfp;
		pstate->stdfp = (pfilename_node->type == MD_AST_NODE_TYPE_STDOUT) ? stdout : stderr;
	} else {
		pstate->poutput_filename_evaluator = rval_evaluator_alloc_from_ast(pfilename_node, pcst->pfmgr,
			type_inferencing, context_flags);
		pstate->file_output_mode = file_output_mode_from_ast_node_type(poutput_node->type);
		phandler = output_all ? handle_emit_all_to_file : handle_emit_to_file;
	}
	pstate->flush_every_record = pcst->flush_every_record;

	return mlr_dsl_cst_statement_valloc(
		pnode,
		phandler,
		free_emit,
		pstate);
}

// ----------------------------------------------------------------
mlr_dsl_cst_statement_t* alloc_emit_lashed(mlr_dsl_cst_t* pcst, mlr_dsl_ast_node_t* pnode,
	int type_inferencing, int context_flags, int do_full_prefixing)
{
	emit_state_t* pstate = mlr_malloc_or_die(sizeof(emit_state_t));

	mlr_dsl_ast_node_t* pemit_node = pnode->pchildren->phead->pvvalue;
	mlr_dsl_ast_node_t* poutput_node = pnode->pchildren->phead->pnext->pvvalue;

	mlr_dsl_ast_node_t* pkeylists_node = pemit_node->pchildren->phead->pvvalue;

	pstate->poutput_filename_evaluator       = NULL;
	pstate->stdfp                            = NULL;
	pstate->pemit_oosvar_namelist_evaluators = NULL;
	pstate->pemit_keylist_evaluators         = NULL;
	pstate->num_emit_keylist_evaluators      = 0;
	pstate->ppemit_keylist_evaluators        = NULL;
	pstate->psingle_lrec_writer              = NULL;
	pstate->pmulti_lrec_writer               = NULL;

	pstate->num_emit_keylist_evaluators = pkeylists_node->pchildren->length;
	pstate->ppemit_keylist_evaluators = mlr_malloc_or_die(pstate->num_emit_keylist_evaluators
		* sizeof(sllv_t*));
	int i = 0;
	for (sllve_t* pe = pkeylists_node->pchildren->phead; pe != NULL; pe = pe->pnext, i++) {
		mlr_dsl_ast_node_t* pkeylist_node = pe->pvvalue;
		pstate->ppemit_keylist_evaluators[i] = allocate_keylist_evaluators_from_ast_node(
			pkeylist_node, pcst->pfmgr, type_inferencing, context_flags);
	}

	sllv_t* pemit_oosvar_namelist_evaluators = sllv_alloc();
	if (pemit_node->pchildren->length == 2) {
		mlr_dsl_ast_node_t* pnamelist_node = pemit_node->pchildren->phead->pnext->pvvalue;
		for (sllve_t* pe = pnamelist_node->pchildren->phead; pe != NULL; pe = pe->pnext) {
			mlr_dsl_ast_node_t* pkeynode = pe->pvvalue;
			sllv_append(pemit_oosvar_namelist_evaluators,
				rval_evaluator_alloc_from_ast(pkeynode, pcst->pfmgr, type_inferencing, context_flags));
		}
	}
	pstate->pemit_oosvar_namelist_evaluators = pemit_oosvar_namelist_evaluators;

	mlr_dsl_cst_statement_handler_t* phandler = NULL;
	pstate->do_full_prefixing = do_full_prefixing;
	mlr_dsl_ast_node_t* pfilename_node = poutput_node->pchildren == NULL
		? NULL
		: poutput_node->pchildren->phead == NULL
		? NULL
		: poutput_node->pchildren->phead->pvvalue;
	if (poutput_node->type == MD_AST_NODE_TYPE_STREAM) {
		phandler = handle_emit_lashed;
	} else if (pfilename_node->type == MD_AST_NODE_TYPE_STDOUT || pfilename_node->type == MD_AST_NODE_TYPE_STDERR) {
		phandler = handle_emit_lashed_to_stdfp;
		pstate->stdfp = (pfilename_node->type == MD_AST_NODE_TYPE_STDOUT) ? stdout : stderr;
	} else {
		pstate->poutput_filename_evaluator = NULL;
		pstate->poutput_filename_evaluator = rval_evaluator_alloc_from_ast(pfilename_node, pcst->pfmgr,
			type_inferencing, context_flags);
		pstate->file_output_mode = file_output_mode_from_ast_node_type(poutput_node->type);
		phandler = handle_emit_lashed_to_file;
	}

	return mlr_dsl_cst_statement_valloc(
		pnode,
		phandler,
		free_emit,
		pstate);
}

// ----------------------------------------------------------------
static void handle_emit(
	mlr_dsl_cst_statement_t* pstatement,
	variables_t*             pvars,
	cst_outputs_t*           pcst_outputs)
{
	emit_state_t* pstate = pstatement->pvstate;
	handle_emit_common(pstate, pvars, pcst_outputs->poutrecs, pcst_outputs->oosvar_flatten_separator);
}

// ----------------------------------------------------------------
static void handle_emit_to_stdfp(
	mlr_dsl_cst_statement_t* pstatement,
	variables_t*             pvars,
	cst_outputs_t*           pcst_outputs)
{
	emit_state_t* pstate = pstatement->pvstate;
	sllv_t* poutrecs = sllv_alloc();

	handle_emit_common(pstate, pvars, poutrecs, pcst_outputs->oosvar_flatten_separator);

	// The opts aren't complete at alloc time so we need to handle them on first use.
	if (pstate->psingle_lrec_writer == NULL)
		pstate->psingle_lrec_writer = lrec_writer_alloc_or_die(pcst_outputs->pwriter_opts);

	lrec_writer_print_all(pstate->psingle_lrec_writer, pstate->stdfp, poutrecs);
	if (pstate->flush_every_record)
		fflush(pstate->stdfp);

	sllv_free(poutrecs);
}

// ----------------------------------------------------------------
static void handle_emit_to_file(
	mlr_dsl_cst_statement_t* pstatement,
	variables_t*             pvars,
	cst_outputs_t*           pcst_outputs)
{
	emit_state_t* pstate = pstatement->pvstate;

	// The opts aren't complete at alloc time so we need to handle them on first use.
	if (pstate->pmulti_lrec_writer == NULL)
		pstate->pmulti_lrec_writer = multi_lrec_writer_alloc(pcst_outputs->pwriter_opts);

	sllv_t* poutrecs = sllv_alloc();

	handle_emit_common(pstate, pvars, poutrecs, pcst_outputs->oosvar_flatten_separator);

	rval_evaluator_t* poutput_filename_evaluator = pstate->poutput_filename_evaluator;
	mv_t filename_mv = poutput_filename_evaluator->pprocess_func(poutput_filename_evaluator->pvstate, pvars);
	char fn_free_flags = 0;
	char* filename = mv_format_val(&filename_mv, &fn_free_flags);

	multi_lrec_writer_output_list(pstate->pmulti_lrec_writer, poutrecs, filename,
		pstate->file_output_mode, pstate->flush_every_record);
	sllv_free(poutrecs);

	if (fn_free_flags)
		free(filename);
	mv_free(&filename_mv);
}

// ----------------------------------------------------------------
static void handle_emit_common(
	emit_state_t* pstate,
	variables_t*  pvars,
	sllv_t*       poutrecs,
	char*         oosvar_flatten_separator)
{
	int keys_all_non_null_or_error = TRUE;
	sllmv_t* pmvkeys = evaluate_list(pstate->pemit_keylist_evaluators, pvars, &keys_all_non_null_or_error);
	if (keys_all_non_null_or_error) {
		int names_all_non_null_or_error = TRUE;
		sllmv_t* pmvnames = evaluate_list(pstate->pemit_oosvar_namelist_evaluators, pvars,
			&names_all_non_null_or_error);
		if (names_all_non_null_or_error) {
			mlhmmv_to_lrecs(pvars->poosvars, pmvkeys, pmvnames, poutrecs,
				pstate->do_full_prefixing, oosvar_flatten_separator);
		}
		sllmv_free(pmvnames);
	}
	sllmv_free(pmvkeys);
}

// ----------------------------------------------------------------
static void handle_emit_lashed(
	mlr_dsl_cst_statement_t* pstatement,
	variables_t*             pvars,
	cst_outputs_t*           pcst_outputs)
{
	emit_state_t* pstate = pstatement->pvstate;
	handle_emit_lashed_common(pstate, pvars, pcst_outputs->poutrecs, pcst_outputs->oosvar_flatten_separator);
}

static void handle_emit_lashed_to_stdfp(
	mlr_dsl_cst_statement_t* pstatement,
	variables_t*             pvars,
	cst_outputs_t*           pcst_outputs)
{
	emit_state_t* pstate = pstatement->pvstate;

	// The opts aren't complete at alloc time so we need to handle them on first use.
	if (pstate->psingle_lrec_writer == NULL)
		pstate->psingle_lrec_writer = lrec_writer_alloc_or_die(pcst_outputs->pwriter_opts);

	sllv_t* poutrecs = sllv_alloc();

	handle_emit_lashed_common(pstate, pvars, poutrecs, pcst_outputs->oosvar_flatten_separator);

	lrec_writer_print_all(pstate->psingle_lrec_writer, pstate->stdfp, poutrecs);
	if (pstate->flush_every_record)
		fflush(pstate->stdfp);

	sllv_free(poutrecs);
}

// ----------------------------------------------------------------
static void handle_emit_lashed_to_file(
	mlr_dsl_cst_statement_t* pstatement,
	variables_t*             pvars,
	cst_outputs_t*           pcst_outputs)
{
	emit_state_t* pstate = pstatement->pvstate;

	// The opts aren't complete at alloc time so we need to handle them on first use.
	if (pstate->pmulti_lrec_writer == NULL)
		pstate->pmulti_lrec_writer = multi_lrec_writer_alloc(pcst_outputs->pwriter_opts);

	sllv_t* poutrecs = sllv_alloc();

	handle_emit_lashed_common(pstate, pvars, poutrecs, pcst_outputs->oosvar_flatten_separator);

	rval_evaluator_t* poutput_filename_evaluator = pstate->poutput_filename_evaluator;
	mv_t filename_mv = poutput_filename_evaluator->pprocess_func(poutput_filename_evaluator->pvstate, pvars);
	char fn_free_flags = 0;
	char* filename = mv_format_val(&filename_mv, &fn_free_flags);

	multi_lrec_writer_output_list(pstate->pmulti_lrec_writer, poutrecs, filename,
		pstate->file_output_mode, pstate->flush_every_record);

	sllv_free(poutrecs);

	if (fn_free_flags)
		free(filename);
	mv_free(&filename_mv);
}

// ----------------------------------------------------------------
static void handle_emit_lashed_common(
	emit_state_t* pstate,
	variables_t*  pvars,
	sllv_t*       poutrecs,
	char*         oosvar_flatten_separator)
{
	int keys_all_non_null_or_error = TRUE;
	sllmv_t** ppmvkeys = evaluate_lists(pstate->ppemit_keylist_evaluators, pstate->num_emit_keylist_evaluators,
		pvars, &keys_all_non_null_or_error);
	if (keys_all_non_null_or_error) {
		int names_all_non_null_or_error = TRUE;
		sllmv_t* pmvnames = evaluate_list(pstate->pemit_oosvar_namelist_evaluators, pvars,
			&names_all_non_null_or_error);
		if (names_all_non_null_or_error) {
			mlhmmv_to_lrecs_lashed(pvars->poosvars, ppmvkeys, pstate->num_emit_keylist_evaluators, pmvnames,
				poutrecs, pstate->do_full_prefixing, oosvar_flatten_separator);
		}
		sllmv_free(pmvnames);
	}
	for (int i = 0; i < pstate->num_emit_keylist_evaluators; i++) {
		sllmv_free(ppmvkeys[i]);
	}
	free(ppmvkeys);
}

// ----------------------------------------------------------------
static void handle_emit_all(
	mlr_dsl_cst_statement_t* pstatement,
	variables_t*             pvars,
	cst_outputs_t*           pcst_outputs)
{
	emit_state_t* pstate = pstatement->pvstate;
	int all_non_null_or_error = TRUE;
	sllmv_t* pmvnames = evaluate_list(pstate->pemit_oosvar_namelist_evaluators, pvars, &all_non_null_or_error);
	if (all_non_null_or_error) {
		mlhmmv_all_to_lrecs(pvars->poosvars, pmvnames, pcst_outputs->poutrecs,
			pstate->do_full_prefixing, pcst_outputs->oosvar_flatten_separator);
	}
	sllmv_free(pmvnames);
}

// ----------------------------------------------------------------
static void handle_emit_all_to_stdfp(
	mlr_dsl_cst_statement_t* pstatement,
	variables_t*             pvars,
	cst_outputs_t*           pcst_outputs)
{
	emit_state_t* pstate = pstatement->pvstate;

	// The opts aren't complete at alloc time so we need to handle them on first use.
	if (pstate->psingle_lrec_writer == NULL)
		pstate->psingle_lrec_writer = lrec_writer_alloc_or_die(pcst_outputs->pwriter_opts);

	sllv_t* poutrecs = sllv_alloc();
	int all_non_null_or_error = TRUE;
	sllmv_t* pmvnames = evaluate_list(pstate->pemit_oosvar_namelist_evaluators, pvars, &all_non_null_or_error);
	if (all_non_null_or_error) {
		mlhmmv_all_to_lrecs(pvars->poosvars, pmvnames, poutrecs,
			pstate->do_full_prefixing, pcst_outputs->oosvar_flatten_separator);
	}
	sllmv_free(pmvnames);

	lrec_writer_print_all(pstate->psingle_lrec_writer, pstate->stdfp, poutrecs);
	if (pstate->flush_every_record)
		fflush(pstate->stdfp);
	sllv_free(poutrecs);
}

// ----------------------------------------------------------------
static void handle_emit_all_to_file(
	mlr_dsl_cst_statement_t* pstatement,
	variables_t*             pvars,
	cst_outputs_t*           pcst_outputs)
{
	emit_state_t* pstate = pstatement->pvstate;

	// The opts aren't complete at alloc time so we need to handle them on first use.
	if (pstate->pmulti_lrec_writer == NULL)
		pstate->pmulti_lrec_writer = multi_lrec_writer_alloc(pcst_outputs->pwriter_opts);

	sllv_t* poutrecs = sllv_alloc();
	rval_evaluator_t* poutput_filename_evaluator = pstate->poutput_filename_evaluator;
	mv_t filename_mv = poutput_filename_evaluator->pprocess_func(poutput_filename_evaluator->pvstate, pvars);
	int all_non_null_or_error = TRUE;
	sllmv_t* pmvnames = evaluate_list(pstate->pemit_oosvar_namelist_evaluators, pvars, &all_non_null_or_error);
	if (all_non_null_or_error) {
		mlhmmv_all_to_lrecs(pvars->poosvars, pmvnames, poutrecs,
			pstate->do_full_prefixing, pcst_outputs->oosvar_flatten_separator);
	}

	char fn_free_flags = 0;
	char* filename = mv_format_val(&filename_mv, &fn_free_flags);
	multi_lrec_writer_output_list(pstate->pmulti_lrec_writer, poutrecs, filename,
		pstate->file_output_mode, pstate->flush_every_record);
	sllv_free(poutrecs);

	if (fn_free_flags)
		free(filename);
	mv_free(&filename_mv);
	sllmv_free(pmvnames);
}

// ----------------------------------------------------------------
static void free_emit(mlr_dsl_cst_statement_t* pstatement) { // emit
	emit_state_t* pstate = pstatement->pvstate;

	if (pstate->poutput_filename_evaluator != NULL) {
		pstate->poutput_filename_evaluator->pfree_func(pstate->poutput_filename_evaluator);
	}

	if (pstate->pemit_oosvar_namelist_evaluators != NULL) {
		for (sllve_t* pe = pstate->pemit_oosvar_namelist_evaluators->phead; pe != NULL; pe = pe->pnext) {
			rval_evaluator_t* phandler = pe->pvvalue;
			phandler->pfree_func(phandler);
		}
		sllv_free(pstate->pemit_oosvar_namelist_evaluators);
	}

	if (pstate->pemit_keylist_evaluators != NULL) {
		for (sllve_t* pe = pstate->pemit_keylist_evaluators->phead; pe != NULL; pe = pe->pnext) {
			rval_evaluator_t* phandler = pe->pvvalue;
			phandler->pfree_func(phandler);
		}
		sllv_free(pstate->pemit_keylist_evaluators);
	}

	if (pstate->ppemit_keylist_evaluators != NULL) {
		for (int i = 0; i < pstate->num_emit_keylist_evaluators; i++) {
			sllv_t* pemit_keylist_evaluators = pstate->ppemit_keylist_evaluators[i];
			for (sllve_t* pe = pemit_keylist_evaluators->phead; pe != NULL; pe = pe->pnext) {
				rval_evaluator_t* phandler = pe->pvvalue;
				phandler->pfree_func(phandler);
			}
			sllv_free(pemit_keylist_evaluators);
		}
		free(pstate->ppemit_keylist_evaluators);
	}

	if (pstate->psingle_lrec_writer != NULL) {
		pstate->psingle_lrec_writer->pfree_func(pstate->psingle_lrec_writer);
	}

	if (pstate->pmulti_lrec_writer != NULL) {
		multi_lrec_writer_drain(pstate->pmulti_lrec_writer);
		multi_lrec_writer_free(pstate->pmulti_lrec_writer);
	}

	free(pstate);
}

// ================================================================
typedef struct _print_state_t {
	rval_evaluator_t*    prhs_evaluator;
	FILE*                stdfp;
	file_output_mode_t   file_output_mode;
	rval_evaluator_t*    poutput_filename_evaluator;
	int                  flush_every_record;
	multi_out_t*         pmulti_out;
	char*                print_terminator;
} print_state_t;

static mlr_dsl_cst_statement_handler_t handle_print;
static mlr_dsl_cst_statement_freer_t free_print;

// ----------------------------------------------------------------
mlr_dsl_cst_statement_t* alloc_print(
	mlr_dsl_cst_t*      pcst,
	mlr_dsl_ast_node_t* pnode,
	int                 type_inferencing,
	int                 context_flags,
	char*               print_terminator)
{
	print_state_t* pstate = mlr_malloc_or_die(sizeof(print_state_t));

	pstate->prhs_evaluator             = NULL;
	pstate->stdfp                      = NULL;
	pstate->poutput_filename_evaluator = NULL;
	pstate->pmulti_out                 = NULL;

	MLR_INTERNAL_CODING_ERROR_IF((pnode->pchildren == NULL) || (pnode->pchildren->length != 2));
	mlr_dsl_ast_node_t* pvalue_node = pnode->pchildren->phead->pvvalue;
	pstate->prhs_evaluator = rval_evaluator_alloc_from_ast(pvalue_node, pcst->pfmgr,
		type_inferencing, context_flags);
	pstate->print_terminator = print_terminator;

	mlr_dsl_ast_node_t* poutput_node = pnode->pchildren->phead->pnext->pvvalue;
	mlr_dsl_ast_node_t* pfilename_node = poutput_node->pchildren->phead->pvvalue;
	if (pfilename_node->type == MD_AST_NODE_TYPE_STDOUT) {
		pstate->stdfp = stdout;
	} else if (pfilename_node->type == MD_AST_NODE_TYPE_STDERR) {
		pstate->stdfp = stderr;
	} else {
		pstate->poutput_filename_evaluator = rval_evaluator_alloc_from_ast(pfilename_node, pcst->pfmgr,
			type_inferencing, context_flags);
		pstate->file_output_mode = file_output_mode_from_ast_node_type(poutput_node->type);
		pstate->pmulti_out = multi_out_alloc();
	}
	pstate->flush_every_record = pcst->flush_every_record;

	return mlr_dsl_cst_statement_valloc(
		pnode,
		handle_print,
		free_print,
		pstate);
}

// ----------------------------------------------------------------
static void handle_print(
	mlr_dsl_cst_statement_t* pstatement,
	variables_t*             pvars,
	cst_outputs_t*           pcst_outputs)
{
	print_state_t* pstate = pstatement->pvstate;

	rval_evaluator_t* prhs_evaluator = pstate->prhs_evaluator;
	mv_t val = prhs_evaluator->pprocess_func(prhs_evaluator->pvstate, pvars);
	char sfree_flags;
	char* sval = mv_format_val(&val, &sfree_flags);

	rval_evaluator_t* poutput_filename_evaluator = pstate->poutput_filename_evaluator;
	if (poutput_filename_evaluator == NULL) {
		fprintf(pstate->stdfp, "%s%s", sval, pstate->print_terminator);
	} else {
		mv_t filename_mv = poutput_filename_evaluator->pprocess_func(poutput_filename_evaluator->pvstate, pvars);

		char fn_free_flags;
		char* filename = mv_format_val(&filename_mv, &fn_free_flags);

		FILE* outfp = multi_out_get(pstate->pmulti_out, filename, pstate->file_output_mode);
		fprintf(outfp, "%s%s", sval, pstate->print_terminator);
		if (pstate->flush_every_record)
			fflush(outfp);

		if (fn_free_flags)
			free(filename);
		mv_free(&filename_mv);
	}

	if (sfree_flags)
		free(sval);
	mv_free(&val);
}

static void free_print(mlr_dsl_cst_statement_t* pstatement) { // print
	print_state_t* pstate = pstatement->pvstate;

	if (pstate->prhs_evaluator != NULL) {
		pstate->prhs_evaluator->pfree_func(pstate->prhs_evaluator);
	}

	if (pstate->poutput_filename_evaluator != NULL) {
		pstate->poutput_filename_evaluator->pfree_func(pstate->poutput_filename_evaluator);
	}

	if (pstate->pmulti_out != NULL) {
		multi_out_close(pstate->pmulti_out);
		multi_out_free(pstate->pmulti_out);
	}

	free(pstate);
}

// ================================================================
struct _dump_state_t; // forward reference
typedef void dump_target_getter_t(variables_t* pvars, struct _dump_state_t* pstate,
	mv_t** ppval, mlhmmv_level_t** pplevel);

static dump_target_getter_t all_oosvar_target_getter;
static dump_target_getter_t oosvar_target_getter;
static dump_target_getter_t nonindexed_local_variable_target_getter;
static dump_target_getter_t indexed_local_variable_target_getter;

typedef struct _dump_state_t {
	int                   target_vardef_frame_relative_index;
	sllv_t*               ptarget_keylist_evaluators;
	dump_target_getter_t* pdump_target_getter;

	FILE*                 stdfp;
	file_output_mode_t    file_output_mode;
	rval_evaluator_t*     poutput_filename_evaluator;
	int                   flush_every_record;
	multi_out_t*          pmulti_out;
} dump_state_t;

static mlr_dsl_cst_statement_handler_t handle_dump;
static mlr_dsl_cst_statement_handler_t handle_dump_to_file;
static mlr_dsl_cst_statement_freer_t   free_dump;

// ----------------------------------------------------------------
mlr_dsl_cst_statement_t* alloc_dump(mlr_dsl_cst_t* pcst, mlr_dsl_ast_node_t* pnode,
	int type_inferencing, int context_flags)
{
	dump_state_t* pstate = mlr_malloc_or_die(sizeof(dump_state_t));

	pstate->target_vardef_frame_relative_index = MD_UNUSED_INDEX;
	pstate->ptarget_keylist_evaluators         = NULL;
	pstate->pdump_target_getter                = NULL;

	pstate->stdfp                              = NULL;
	pstate->poutput_filename_evaluator         = NULL;
	pstate->pmulti_out                         = NULL;
	pstate->flush_every_record = pcst->flush_every_record;

	mlr_dsl_ast_node_t* poutput_node = pnode->pchildren->phead->pvvalue;
	mlr_dsl_ast_node_t* pfilename_node = poutput_node->pchildren->phead->pvvalue;
	mlr_dsl_cst_statement_handler_t* phandler = NULL;

	if (pnode->pchildren->length == 1) { // dump all oosvars
		pstate->pdump_target_getter = all_oosvar_target_getter;

	} else if (pnode->pchildren->length == 2) { // dump specific oosvar or local
		mlr_dsl_ast_node_t* ptarget_node = pnode->pchildren->phead->pnext->pvvalue;

		if (ptarget_node->type == MD_AST_NODE_TYPE_OOSVAR_KEYLIST) {
			pstate->pdump_target_getter = oosvar_target_getter;
			pstate->ptarget_keylist_evaluators = allocate_keylist_evaluators_from_ast_node(
				ptarget_node, pcst->pfmgr, type_inferencing, context_flags);

		} else if (ptarget_node->type == MD_AST_NODE_TYPE_NONINDEXED_LOCAL_VARIABLE) {
			pstate->pdump_target_getter = nonindexed_local_variable_target_getter;
			MLR_INTERNAL_CODING_ERROR_IF(ptarget_node->vardef_frame_relative_index == MD_UNUSED_INDEX);
			pstate->target_vardef_frame_relative_index = ptarget_node->vardef_frame_relative_index;

		} else if (ptarget_node->type == MD_AST_NODE_TYPE_INDEXED_LOCAL_VARIABLE) {
			pstate->pdump_target_getter = indexed_local_variable_target_getter;
			MLR_INTERNAL_CODING_ERROR_IF(ptarget_node->vardef_frame_relative_index == MD_UNUSED_INDEX);
			pstate->target_vardef_frame_relative_index = ptarget_node->vardef_frame_relative_index;
			pstate->ptarget_keylist_evaluators = allocate_keylist_evaluators_from_ast_node(
				ptarget_node, pcst->pfmgr, type_inferencing, context_flags);

		} else {
			MLR_INTERNAL_CODING_ERROR();
		}

	} else {
		MLR_INTERNAL_CODING_ERROR();
	}

	if (pfilename_node->type == MD_AST_NODE_TYPE_STDOUT) {
		phandler = handle_dump;
		pstate->stdfp = stdout;
	} else if (pfilename_node->type == MD_AST_NODE_TYPE_STDERR) {
		phandler = handle_dump;
		pstate->stdfp = stderr;
	} else {
		pstate->poutput_filename_evaluator = rval_evaluator_alloc_from_ast(pfilename_node, pcst->pfmgr,
			type_inferencing, context_flags);
		pstate->file_output_mode = file_output_mode_from_ast_node_type(poutput_node->type);
		pstate->pmulti_out = multi_out_alloc();
		phandler = handle_dump_to_file;
	}

	return mlr_dsl_cst_statement_valloc(
		pnode,
		phandler,
		free_dump,
		pstate);
}

// ----------------------------------------------------------------
static void all_oosvar_target_getter(variables_t* pvars, dump_state_t* pstate,
	mv_t** ppval, mlhmmv_level_t** pplevel)
{
	*ppval   = NULL;
	*pplevel = pvars->poosvars->proot_level;
}

static void oosvar_target_getter(variables_t* pvars, dump_state_t* pstate,
	mv_t** ppval, mlhmmv_level_t** pplevel)
{
	*ppval = NULL;

	int all_non_null_or_error = TRUE;
	sllmv_t* pmvkeys = evaluate_list(pstate->ptarget_keylist_evaluators, pvars, &all_non_null_or_error);
	if (all_non_null_or_error) {
		int lookup_error = FALSE;
		mlhmmv_level_t* plevel = mlhmmv_get_level(pvars->poosvars, pmvkeys, &lookup_error);
		if (plevel != NULL) {
			*pplevel = plevel;
		}
	}
	sllmv_free(pmvkeys);
}

static void nonindexed_local_variable_target_getter(variables_t* pvars, dump_state_t* pstate,
	mv_t** ppval, mlhmmv_level_t** pplevel)
{
	local_stack_frame_t* pframe = local_stack_get_top_frame(pvars->plocal_stack);

	mlhmmv_value_t* pmval = local_stack_frame_get_map_value(pframe,
		pstate->target_vardef_frame_relative_index, NULL);
	if (pmval == NULL) {
		*ppval   = NULL;
		*pplevel = NULL;
	} else if (pmval->is_terminal) {
		*ppval   = &pmval->u.mlrval;
		*pplevel = NULL;
	} else {
		*ppval   = NULL;
		*pplevel = pmval->u.pnext_level;
	}
}

static void indexed_local_variable_target_getter(variables_t* pvars, dump_state_t* pstate,
	mv_t** ppval, mlhmmv_level_t** pplevel)
{
	*ppval   = NULL;
	*pplevel = NULL;

	local_stack_frame_t* pframe = local_stack_get_top_frame(pvars->plocal_stack);

	int all_non_null_or_error = TRUE;
	sllmv_t* pmvkeys = evaluate_list(pstate->ptarget_keylist_evaluators, pvars, &all_non_null_or_error);
	if (all_non_null_or_error) {

		mlhmmv_value_t* pmval = local_stack_frame_get_map_value(pframe,
			pstate->target_vardef_frame_relative_index, pmvkeys);
		if (pmval != NULL) {
			if (pmval->is_terminal) {
				*ppval   = &pmval->u.mlrval;
				*pplevel = NULL;
			} else {
				*ppval   = NULL;
				*pplevel = pmval->u.pnext_level;
			}
		}
	}

	sllmv_free(pmvkeys);
}

// ----------------------------------------------------------------
static void handle_dump(
	mlr_dsl_cst_statement_t* pstatement,
	variables_t*             pvars,
	cst_outputs_t*           pcst_outputs)
{
	dump_state_t* pstate = pstatement->pvstate;
	mv_t* pval = NULL;
	mlhmmv_level_t* plevel = NULL;
	pstate->pdump_target_getter(pvars, pstate, &pval, &plevel);
	if (pval != NULL) {
		mlhmmv_print_terminal(pval, FALSE, pstate->stdfp);
		fprintf(pstate->stdfp, "\n");
	} else if (plevel != NULL) {
		mlhmmv_level_print_stacked(plevel, 0, FALSE, FALSE, "", pstate->stdfp); // xxx mk simpler call w/ dfl args
	}
}

// ----------------------------------------------------------------
static void handle_dump_to_file(
	mlr_dsl_cst_statement_t* pstatement,
	variables_t*             pvars,
	cst_outputs_t*           pcst_outputs)
{
	dump_state_t* pstate = pstatement->pvstate;
	rval_evaluator_t* poutput_filename_evaluator = pstate->poutput_filename_evaluator;
	mv_t filename_mv = poutput_filename_evaluator->pprocess_func(poutput_filename_evaluator->pvstate, pvars);
	char fn_free_flags;
	char* filename = mv_format_val(&filename_mv, &fn_free_flags);

	FILE* outfp = multi_out_get(pstate->pmulti_out, filename, pstate->file_output_mode);

	mv_t* pval = NULL;
	mlhmmv_level_t* plevel = NULL;
	pstate->pdump_target_getter(pvars, pstate, &pval, &plevel);
	if (pval != NULL) {
		mlhmmv_print_terminal(pval, FALSE, outfp);
		fprintf(outfp, "\n");
	} else if (plevel != NULL) {
		mlhmmv_level_print_stacked(plevel, 0, FALSE, FALSE, "", outfp); // xxx mk simpler call w/ dfl args
	}

	if (pstate->flush_every_record)
		fflush(outfp);

	if (fn_free_flags)
		free(filename);
	mv_free(&filename_mv);
}

// ----------------------------------------------------------------
static void free_dump(mlr_dsl_cst_statement_t* pstatement) {
	dump_state_t* pstate = pstatement->pvstate;

	if (pstate->poutput_filename_evaluator != NULL) {
		pstate->poutput_filename_evaluator->pfree_func(pstate->poutput_filename_evaluator);
	}

	if (pstate->pmulti_out != NULL) {
		multi_out_close(pstate->pmulti_out);
		multi_out_free(pstate->pmulti_out);
	}

	free(pstate);
}
