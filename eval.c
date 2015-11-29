#include "bfs.h"
#include "bftw.h"
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

struct eval_state {
	/** Data about the current file. */
	struct BFTW *ftwbuf;
	/** The parsed command line. */
	const cmdline *cl;
	/** The bftw() callback return value. */
	bftw_action action;
	/** A stat() buffer, if necessary. */
	struct stat statbuf;
};

/**
 * Perform a stat() call if necessary.
 */
static void fill_statbuf(eval_state *state) {
	struct BFTW *ftwbuf = state->ftwbuf;
	if (ftwbuf->statbuf) {
		return;
	}

	if (fstatat(ftwbuf->at_fd, ftwbuf->at_path, &state->statbuf, AT_SYMLINK_NOFOLLOW) == 0) {
		ftwbuf->statbuf = &state->statbuf;
	} else {
		perror("fstatat()");
	}
}

/**
 * -true test.
 */
bool eval_true(const expression *expr, eval_state *state) {
	return true;
}

/**
 * -false test.
 */
bool eval_false(const expression *expr, eval_state *state) {
	return false;
}

/**
 * -executable, -readable, -writable action.
 */
bool eval_access(const expression *expr, eval_state *state) {
	struct BFTW *ftwbuf = state->ftwbuf;
	return faccessat(ftwbuf->at_fd, ftwbuf->at_path, expr->idata, AT_SYMLINK_NOFOLLOW) == 0;
}

/**
 * -delete action.
 */
bool eval_delete(const expression *expr, eval_state *state) {
	struct BFTW *ftwbuf = state->ftwbuf;

	int flag = 0;
	if (ftwbuf->typeflag == BFTW_DIR) {
		flag |= AT_REMOVEDIR;
	}

	if (unlinkat(ftwbuf->at_fd, ftwbuf->at_path, flag) != 0) {
		print_error(state->cl->colors, ftwbuf->path, errno);
		state->action = BFTW_STOP;
	}

	return true;
}

/**
 * -prune action.
 */
bool eval_prune(const expression *expr, eval_state *state) {
	state->action = BFTW_SKIP_SUBTREE;
	return true;
}

/**
 * -hidden test.
 */
bool eval_hidden(const expression *expr, eval_state *state) {
	struct BFTW *ftwbuf = state->ftwbuf;
	return ftwbuf->nameoff > 0 && ftwbuf->path[ftwbuf->nameoff] == '.';
}

/**
 * -nohidden action.
 */
bool eval_nohidden(const expression *expr, eval_state *state) {
	if (eval_hidden(expr, state)) {
		eval_prune(expr, state);
		return false;
	} else {
		return true;
	}
}

/**
 * -name test.
 */
bool eval_name(const expression *expr, eval_state *state) {
	struct BFTW *ftwbuf = state->ftwbuf;
	return fnmatch(expr->sdata, ftwbuf->path + ftwbuf->nameoff, 0) == 0;
}

/**
 * -path test.
 */
bool eval_path(const expression *expr, eval_state *state) {
	struct BFTW *ftwbuf = state->ftwbuf;
	return fnmatch(expr->sdata, ftwbuf->path, 0) == 0;
}

/**
 * -print action.
 */
bool eval_print(const expression *expr, eval_state *state) {
	color_table *colors = state->cl->colors;
	if (colors) {
		fill_statbuf(state);
	}
	pretty_print(colors, state->ftwbuf);
	return true;
}

/**
 * -print0 action.
 */
bool eval_print0(const expression *expr, eval_state *state) {
	const char *path = state->ftwbuf->path;
	fwrite(path, 1, strlen(path) + 1, stdout);
	return true;
}

/**
 * -quit action.
 */
bool eval_quit(const expression *expr, eval_state *state) {
	state->action = BFTW_STOP;
	return true;
}

/**
 * -type test.
 */
bool eval_type(const expression *expr, eval_state *state) {
	return state->ftwbuf->typeflag == expr->idata;
}

/**
 * Evaluate a negation.
 */
bool eval_not(const expression *expr, eval_state *state) {
	return !expr->rhs->eval(expr, state);
}

/**
 * Evaluate a conjunction.
 */
bool eval_and(const expression *expr, eval_state *state) {
	return expr->lhs->eval(expr->lhs, state) && expr->rhs->eval(expr->rhs, state);
}

/**
 * Evaluate a disjunction.
 */
bool eval_or(const expression *expr, eval_state *state) {
	return expr->lhs->eval(expr->lhs, state) || expr->rhs->eval(expr->rhs, state);
}

/**
 * Evaluate the comma operator.
 */
bool eval_comma(const expression *expr, eval_state *state) {
	expr->lhs->eval(expr->lhs, state);
	return expr->rhs->eval(expr->rhs, state);
}

/**
 * Infer the number of open file descriptors we're allowed to have.
 */
static int infer_nopenfd() {
	int ret = 4096;

	struct rlimit rl;
	if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
		if (rl.rlim_cur != RLIM_INFINITY) {
			ret = rl.rlim_cur;
		}
	}

	// Account for std{in,out,err}
	if (ret > 3) {
		ret -= 3;
	}

	return ret;
}

/**
 * bftw() callback.
 */
static bftw_action cmdline_callback(struct BFTW *ftwbuf, void *ptr) {
	const cmdline *cl = ptr;

	if (ftwbuf->typeflag == BFTW_ERROR) {
		print_error(cl->colors, ftwbuf->path, ftwbuf->error);
		return BFTW_SKIP_SUBTREE;
	}

	eval_state state = {
		.ftwbuf = ftwbuf,
		.cl = cl,
		.action = BFTW_CONTINUE,
	};

	if (ftwbuf->depth >= cl->maxdepth) {
		state.action = BFTW_SKIP_SUBTREE;
	}

	// In -depth mode, only handle directories on the BFTW_POST visit
	bftw_visit expected_visit = BFTW_PRE;
	if ((cl->flags & BFTW_DEPTH)
	    && ftwbuf->typeflag == BFTW_DIR
	    && ftwbuf->depth < cl->maxdepth) {
		expected_visit = BFTW_POST;
	}

	if (ftwbuf->visit == expected_visit
	    && ftwbuf->depth >= cl->mindepth
	    && ftwbuf->depth <= cl->maxdepth) {
		cl->expr->eval(cl->expr, &state);
	}

	return state.action;
}

/**
 * Evaluate the command line.
 */
int eval_cmdline(cmdline *cl) {
	int ret = 0;
	int nopenfd = infer_nopenfd();

	for (size_t i = 0; i < cl->nroots; ++i) {
		if (bftw(cl->roots[i], cmdline_callback, nopenfd, cl->flags, cl) != 0) {
			ret = -1;
			perror("bftw()");
		}
	}

	return ret;
}
