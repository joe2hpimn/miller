noinst_LTLIBRARIES=	libmapping.la
libmapping_la_SOURCES=	\
			context_flags.h \
			function_manager.h \
			function_manager.c \
			rval_evaluator.h \
			rval_evaluators.h \
			rval_expr_evaluators.c \
			rxval_expr_evaluators.c \
			rval_func_evaluators.c \
			rval_list_evaluators.c \
			mapper.h \
			mapper_bar.c \
			mapper_bootstrap.c \
			mapper_cat.c \
			mapper_check.c \
			mapper_cut.c \
			mapper_decimate.c \
			mapper_grep.c \
			mapper_group_like.c \
			mapper_having_fields.c \
			mapper_head.c \
			mapper_histogram.c \
			mapper_join.c \
			mapper_label.c \
			mapper_merge_fields.c \
			mapper_most_or_least_frequent.c \
			mapper_nest.c \
			mapper_nothing.c \
			mapper_put_or_filter.c \
			mapper_regularize.c \
			mapper_rename.c \
			mapper_reorder.c \
			mapper_repeat.c \
			mapper_reshape.c \
			mapper_sample.c \
			mapper_sec2gmt.c \
			mapper_sec2gmtdate.c \
			mapper_seqgen.c \
			mapper_shuffle.c \
			mapper_sort.c \
			mapper_stats1.c \
			mapper_stats2.c \
			mapper_step.c \
			mapper_tac.c \
			mapper_tail.c \
			mapper_tee.c \
			mapper_top.c \
			mapper_uniq.c \
			mappers.h \
			mlr_dsl_ast.c \
			mlr_dsl_ast.h \
			mlr_dsl_blocked_ast.c \
			mlr_dsl_blocked_ast.h \
			mlr_dsl_cst.c \
			mlr_dsl_cst.h \
			mlr_dsl_cst_func_subr.c \
			mlr_dsl_cst_keywords.c \
			mlr_dsl_cst_statements.c \
			mlr_dsl_cst_assignment_statements.c \
			mlr_dsl_cst_unset_statements.c \
			mlr_dsl_cst_condish_statements.c \
			mlr_dsl_cst_for_srec_statements.c \
			mlr_dsl_cst_for_map_statements.c \
			mlr_dsl_cst_triple_for_statements.c \
			mlr_dsl_cst_loop_control_statements.c \
			mlr_dsl_cst_return_statements.c \
			mlr_dsl_cst_output_statements.c \
			mlr_dsl_stack_allocate.c \
			stats1_accumulators.c \
			stats1_accumulators.h \
			type_inference.h
libmapping_la_LIBADD=	../lib/libmlr.la ../cli/libcli.la ../input/libinput.la

# TODO: replace the interesting content with unit tests; jettison the rest
# TODO: Does not build
#noinst_PROGRAMS=	evl
#evl_SOURCES=		rval_evaluators.c
#evl_LDADD=		libmapping.la ../containers/libcontainers.la ../lib/libmlr.la
#evl_CPPFLAGS=		-D__LREC_EVALUATORS_MAIN__ ${AM_CPPFLAGS}

AM_CPPFLAGS=	-I${srcdir}/../
AM_CFLAGS=	-std=gnu99
