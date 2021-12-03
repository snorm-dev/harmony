#include <sys/time.h>
#include <unistd.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <errno.h>
#include <assert.h>

#ifndef HARMONY_COMBINE
#include "global.h"
#include "charm.h"
#include "ops.h"
#include "dot.h"
#include "strbuf.h"
#include "iface/iface.h"
#endif

#ifdef __APPLE__

#define PTHREAD_BARRIER_SERIAL_THREAD 1

typedef int pthread_barrierattr_t;
typedef struct
{
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    unsigned int threads_required;
    unsigned int threads_left;
    unsigned int cycle;
} pthread_barrier_t;


int pthread_barrier_init(
    pthread_barrier_t *barrier,
    const pthread_barrierattr_t *attr,
    unsigned int count
){
    barrier->threads_required = barrier->threads_left = count;
    barrier->cycle = 0;
    pthread_mutex_init(&barrier->mutex, NULL);
    pthread_cond_init(&barrier->cond, NULL);
    return 0;
}

int pthread_barrier_destroy(pthread_barrier_t *barrier) {
    pthread_cond_destroy(&barrier->cond);
    pthread_mutex_destroy(&barrier->mutex);
    return 0;
}

int pthread_barrier_wait(pthread_barrier_t *barrier) {
    pthread_mutex_lock(&barrier->mutex);

    if (--barrier->threads_left == 0) {
        barrier->cycle++;
        barrier->threads_left = barrier->threads_required;
        pthread_cond_broadcast(&barrier->cond);
        pthread_mutex_unlock(&barrier->mutex);
        return PTHREAD_BARRIER_SERIAL_THREAD;
    }
    else {
        unsigned int cycle = barrier->cycle;

        while (cycle == barrier->cycle)
            pthread_cond_wait(&barrier->cond, &barrier->mutex);
        pthread_mutex_unlock(&barrier->mutex);
        return 0;
    }
}

#endif // __APPLE__


#define CHUNKSIZE   (1 << 12)

// One of these per worker thread
struct worker {
    struct global_t *global;     // global state
    double timeout;
    pthread_barrier_t *start_barrier, *end_barrier;

    int index;                   // index of worker
    pthread_t tid;               // thread identifier
    struct minheap *todo;        // set of states to evaluate
    int timecnt;                 // to reduce gettime() overhead
    struct step inv_step;        // for evaluating invariants

    struct minheap *results[2]; // out-queues for weights 0 and 1
};

bool invariant_check(struct global_t *global, struct state *state, struct step *step, int end){
    assert(step->ctx->sp == 0);
    assert(step->ctx->failure == 0);
    step->ctx->pc++;
    while (step->ctx->pc != end) {
        struct op_info *oi = global->code.instrs[step->ctx->pc].oi;
        int oldpc = step->ctx->pc;
        (*oi->op)(global->code.instrs[oldpc].env, state, step, global);
        if (step->ctx->failure != 0) {
            step->ctx->sp = 0;
            return false;
        }
        assert(step->ctx->pc != oldpc);
        assert(!step->ctx->terminated);
    }
    assert(step->ctx->sp == 1);
    step->ctx->sp = 0;
    assert(step->ctx->fp == 0);
    uint64_t b = step->ctx->stack[0];
    assert((b & VALUE_MASK) == VALUE_BOOL);
    return b >> VALUE_BITS;
}

void check_invariants(struct worker *w, struct node *node, struct step *step){
    struct global_t *global = w->global;
    struct state *state = node->state;
    extern int invariant_cnt(const void *env);

    assert((state->invariants & VALUE_MASK) == VALUE_SET);
    assert(step->ctx->sp == 0);
    int size;
    uint64_t *vals = value_get(state->invariants, &size);
    size /= sizeof(uint64_t);
    for (int i = 0; i < size; i++) {
        assert((vals[i] & VALUE_MASK) == VALUE_PC);
        step->ctx->pc = vals[i] >> VALUE_BITS;
        assert(strcmp(global->code.instrs[step->ctx->pc].oi->name, "Invariant") == 0);
        int end = invariant_cnt(global->code.instrs[step->ctx->pc].env);
        bool b = invariant_check(global, state, step, end);
        if (step->ctx->failure != 0) {
            printf("Invariant failed: %s\n", value_string(step->ctx->failure));
            b = false;
        }
        if (!b) {
            struct failure *f = new_alloc(struct failure);
            node->ftype = FAIL_INVARIANT;
            break;
        }
    }
}

static double gettime(){
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + (double) tv.tv_usec / 1000000;
}

static bool onestep(
    struct worker *w,       // thread info
    struct node *node,      // starting node
    struct state *sc,       // actual state
    uint64_t ctx,           // context identifier
    struct step *step,      // step info
    uint64_t choice,        // if about to make a choice, which choice?
    bool interrupt,         // start with invoking interrupt handler
    bool infloop_detect,    // try to detect infloop from the start
    int multiplicity        // #contexts that are in the current state
) {
    assert(!step->ctx->terminated);
    assert(step->ctx->failure == 0);

    struct global_t *global = w->global;

    // See if we should also try an interrupt.
    if (interrupt) {
		assert(step->ctx->trap_pc != 0);
        interrupt_invoke(step);
    }

    // Copy the choice
    uint64_t choice_copy = choice;

    // bool log_occurred = global->code.instrs[step->ctx->pc].choose;
    bool log_occurred = false;
    bool choosing = false, infinite_loop = false;
    struct dict *infloop = NULL;        // infinite loop detector
    int loopcnt = 0;
    for (;; loopcnt++) {
        int pc = step->ctx->pc;

        if (w->index == 0 && w->timecnt-- == 0) {
            double now = gettime();
            if (now - global->lasttime > 1) {
                if (global->lasttime != 0) {
                    char *p = value_string(step->ctx->name);
                    fprintf(stderr, "%s pc=%d diameter=%d states=%d queue=%d\n",
                            p, step->ctx->pc, node->len, global->enqueued, global->enqueued - global->dequeued);
                    free(p);
                }
                global->lasttime = now;
                if (now > w->timeout) {
                    fprintf(stderr, "charm: timeout exceeded\n");
                    exit(1);
                }
            }
            w->timecnt = 100;
        }

        struct instr_t *instrs = global->code.instrs;
        struct op_info *oi = instrs[pc].oi;
        if (instrs[pc].choose) {
            step->ctx->stack[step->ctx->sp - 1] = choice;
            step->ctx->pc++;
        }
        else {
            if (instrs[pc].load || instrs[pc].store || instrs[pc].del) {
                step->ai = graph_ai_alloc(multiplicity, step->ctx->atomic, pc);
            }
            (*oi->op)(instrs[pc].env, sc, step, global);
            step->ai = NULL;
        }
		assert(step->ctx->pc >= 0);
		assert(step->ctx->pc < global->code.len);

        if (!step->ctx->terminated && step->ctx->failure == 0 && (infloop_detect || loopcnt > 1000)) {
            if (infloop == NULL) {
                infloop = dict_new(0);
            }

            int stacksize = step->ctx->sp * sizeof(uint64_t);
            int combosize = sizeof(struct combined) + stacksize;
            struct combined *combo = calloc(1, combosize);
            combo->state = *sc;
            memcpy(&combo->context, step->ctx, sizeof(*step->ctx) + stacksize);
            void **p = dict_insert(infloop, combo, combosize);
            free(combo);
            if (*p == (void *) 0) {
                *p = (void *) 1;
            }
            else if (infloop_detect) {
                step->ctx->failure = value_put_atom(&global->values, "infinite loop", 13);
                infinite_loop = true;
            }
            else {
                // start over, as twostep does not have loopcnt optimization
                return false;
            }
        }

        if (step->ctx->terminated || step->ctx->failure != 0 || step->ctx->stopped) {
            break;
        }
        if (step->ctx->pc == pc) {
            fprintf(stderr, ">>> %s\n", oi->name);
        }
        assert(step->ctx->pc != pc);
		assert(step->ctx->pc >= 0);
		assert(step->ctx->pc < global->code.len);

        /* Peek at the next instruction.
         */
        oi = global->code.instrs[step->ctx->pc].oi;
        if (global->code.instrs[step->ctx->pc].choose) {
            assert(step->ctx->sp > 0);
            if (0 && step->ctx->readonly > 0) {    // TODO
                value_ctx_failure(step->ctx, &global->values, "can't choose in assertion or invariant");
                break;
            }
            uint64_t s = step->ctx->stack[step->ctx->sp - 1];
            if ((s & VALUE_MASK) != VALUE_SET) {
                value_ctx_failure(step->ctx, &global->values, "choose operation requires a set");
                break;
            }
            int size;
            uint64_t *vals = value_get(s, &size);
            size /= sizeof(uint64_t);
            if (size == 0) {
                value_ctx_failure(step->ctx, &global->values, "choose operation requires a non-empty set");
                break;
            }
            if (size == 1) {            // TODO.  This optimization is probably not worth it
                choice = vals[0];
            }
            else {
                choosing = true;
                break;
            }
        }

        // See if we need to break out of this step
        struct instr_t *next_instr = &global->code.instrs[step->ctx->pc];
        if (!step->ctx->atomicFlag && (next_instr->breakable || next_instr->log)) {
            if (!step->ctx->atomicFlag && step->ctx->atomic > 0) {
                step->ctx->atomicFlag = true;
            }
            if (!step->ctx->atomicFlag && next_instr->log && !log_occurred) {
                log_occurred = true;
            }
            else {
                break;
            }
        }
    }

    if (infloop != NULL) {
        dict_delete(infloop);
    }

    // Remove old context from the bag
    uint64_t count = value_dict_load(sc->ctxbag, ctx);
    assert((count & VALUE_MASK) == VALUE_INT);
    count -= 1 << VALUE_BITS;
    if (count == VALUE_INT) {
        sc->ctxbag = value_dict_remove(&global->values, sc->ctxbag, ctx);
    }
    else {
        sc->ctxbag = value_dict_store(&global->values, sc->ctxbag, ctx, count);
    }

    // Store new context in value directory.  Must be immutable now.
    uint64_t after = value_put_context(&global->values, step->ctx);

    // If choosing, save in state
    if (choosing) {
        assert(!step->ctx->terminated);
        sc->choosing = after;
    }

    // Add new context to state unless it's terminated or stopped
    if (step->ctx->stopped) {
        sc->stopbag = value_bag_add(&global->values, sc->stopbag, after, 1);
    }
    else if (!step->ctx->terminated) {
        sc->ctxbag = value_bag_add(&global->values, sc->ctxbag, after, 1);
    }

    // Weight of this step
    int weight = ctx == node->after ? 0 : 1;

    struct node *next = new_alloc(struct node);
    next->parent = node;
    next->state = sc;
    next->before = ctx;
    next->choice = choice_copy;
    next->interrupt = interrupt;
    next->after = after;
    next->len = node->len + weight;
    next->steps = node->steps + loopcnt;
    next->weight = weight;

    next->ai = step->ai;
    step->ai = NULL;
    next->log = step->log;
    next->nlog = step->nlog;
    step->log = NULL;
    step->nlog = 0;

    if (step->ctx->failure != 0) {
        next->ftype = infinite_loop ? FAIL_TERMINATION : FAIL_SAFETY;
    }
    else if (sc->choosing == 0 && sc->invariants != VALUE_SET) {
        check_invariants(w, next, &w->inv_step);
    }

    minheap_insert(w->results[weight], next);

    return true;
}

static void make_step(
    struct worker *w,
    struct node *node,
    uint64_t ctx,
    uint64_t choice,       // if about to make a choice, which choice?
    int multiplicity       // #contexts that are in the current state
) {
    struct step step;
    memset(&step, 0, sizeof(step));

    // Make a copy of the state
    struct state *sc = new_alloc(struct state);
    memcpy(sc, node->state, sizeof(*sc));

    // Make a copy of the context
    step.ctx = value_copy(ctx, NULL);

    // See if we need to interrupt
    if (sc->choosing == 0 && step.ctx->trap_pc != 0 && !step.ctx->interruptlevel) {
        bool succ = onestep(w, node, sc, ctx, &step, choice, true, false, multiplicity);
        if (!succ) {
            (void) onestep(w, node, sc, ctx, &step, choice, true, true, multiplicity);
        }
        // sc = new_alloc(struct state);
        memcpy(sc, node->state, sizeof(*sc));
        free(step.ctx);
        step.ctx = value_copy(ctx, NULL);
    }

    sc->choosing = 0;
    bool succ = onestep(w, node, sc, ctx, &step, choice, false, false, multiplicity);
    if (!succ) {
        (void) onestep(w, node, sc, ctx, &step, choice, false, true, multiplicity);
    }

    free(step.ctx);
}

void print_vars(FILE *file, uint64_t v){
    assert((v & VALUE_MASK) == VALUE_DICT);
    int size;
    uint64_t *vars = value_get(v, &size);
    size /= sizeof(uint64_t);
    fprintf(file, "{");
    for (int i = 0; i < size; i += 2) {
        if (i > 0) {
            fprintf(file, ",");
        }
        char *k = value_string(vars[i]);
		int len = strlen(k);
        char *v = value_json(vars[i+1]);
        fprintf(file, " \"%.*s\": %s", len - 2, k + 1, v);
        free(k);
        free(v);
    }
    fprintf(file, " }");
}

static char *json_escape(const char *s, unsigned int len){
	struct strbuf sb;

	strbuf_init(&sb);
	while (len > 0) {
		switch (*s) {		// TODO.  More cases
		case '"':
			strbuf_append(&sb, "\\\"", 2);
			break;
		default:
			strbuf_append(&sb, s, 1);
		}
		s++;
		len--;
	}
	return strbuf_getstr(&sb);
}

char *json_escape_value(uint64_t v){
    char *s = value_string(v);
    char *r = json_escape(s, strlen(s));
    free(s);
    return r;
}

bool print_trace(
    struct global_t *global,
    FILE *file,
    struct context *ctx,
    int pc,
    int fp,
    uint64_t vars
) {
    if (fp == 0) {
        return false;
    }
    assert(fp >= 4);

	int level = 0, orig_pc = pc;
    if (strcmp(global->code.instrs[pc].oi->name, "Frame") == 0) {
        uint64_t ct = ctx->stack[ctx->sp - 2];
        assert((ct & VALUE_MASK) == VALUE_INT);
        switch (ct >> VALUE_BITS) {
        case CALLTYPE_PROCESS:
            pc++;
            break;
        case CALLTYPE_INTERRUPT:
        case CALLTYPE_NORMAL:
            {
                uint64_t retaddr = ctx->stack[ctx->sp - 3];
                assert((retaddr & VALUE_MASK) == VALUE_PC);
                pc = retaddr >> VALUE_BITS;
            }
            break;
        default:
            fprintf(stderr, "call type: %"PRIx64" %d %d %d\n", ct, ctx->sp, ctx->fp, ctx->pc);
            // panic("print_trace: bad call type 1");
        }
    }
    while (--pc >= 0) {
        const char *name = global->code.instrs[pc].oi->name;

        if (strcmp(name, "Return") == 0) {
			level++;
		}
        else if (strcmp(name, "Frame") == 0) {
			if (level == 0) {
				if (fp >= 5) {
                    assert((ctx->stack[fp - 5] & VALUE_MASK) == VALUE_PC);
					int npc = ctx->stack[fp - 5] >> VALUE_BITS;
					uint64_t nvars = ctx->stack[fp - 2];
					int nfp = ctx->stack[fp - 1] >> VALUE_BITS;
					if (print_trace(global, file, ctx, npc, nfp, nvars)) {
                        fprintf(file, ",\n");
                    }
				}
				fprintf(file, "            {\n");
				fprintf(file, "              \"pc\": \"%d\",\n", orig_pc);
				fprintf(file, "              \"xpc\": \"%d\",\n", pc);

				const struct env_Frame *ef = global->code.instrs[pc].env;
				char *s = value_string(ef->name), *a = NULL;
				int len = strlen(s);
                a = json_escape_value(ctx->stack[fp - 3]);
				if (*a == '(') {
					fprintf(file, "              \"method\": \"%.*s%s\",\n", len - 2, s + 1, a);
				}
				else {
					fprintf(file, "              \"method\": \"%.*s(%s)\",\n", len - 2, s + 1, a);
				}

                uint64_t ct = ctx->stack[fp - 4];
                assert((ct & VALUE_MASK) == VALUE_INT);
                switch (ct >> VALUE_BITS) {
                case CALLTYPE_PROCESS:
                    fprintf(file, "              \"calltype\": \"process\",\n");
                    break;
                case CALLTYPE_NORMAL:
                    fprintf(file, "              \"calltype\": \"normal\",\n");
                    break;
                case CALLTYPE_INTERRUPT:
                    fprintf(file, "              \"calltype\": \"interrupt\",\n");
                    break;
                default:
                    panic("print_trace: bad call type 2");
                }

				free(s);
				free(a);
				fprintf(file, "              \"vars\": ");
				print_vars(file, vars);
				fprintf(file, "\n");
				fprintf(file, "            }");
				return true;
			}
            else {
                assert(level > 0);
                level--;
            }
        }
    }
    return false;
}

char *ctx_status(struct node *node, uint64_t ctx) {
    if (node->state->choosing == ctx) {
        return "choosing";
    }
    while (node->state->choosing != 0) {
        node = node->parent;
    }
    struct edge *edge;
    for (edge = node->fwd; edge != NULL; edge = edge->next) {
        if (edge->ctx == ctx) {
            break;
        }
    };
    if (edge != NULL && edge->node == node) {
        return "blocked";
    }
    return "runnable";
}

void print_context(
    struct global_t *global,
    FILE *file,
    uint64_t ctx,
    int tid,
    struct node *node
) {
    char *s, *a;

    fprintf(file, "        {\n");
    fprintf(file, "          \"tid\": \"%d\",\n", tid);
    fprintf(file, "          \"yhash\": \"%"PRIx64"\",\n", ctx);

    struct context *c = value_get(ctx, NULL);

    s = value_string(c->name);
	int len = strlen(s);
    a = json_escape_value(c->arg);
    if (*a == '(') {
        fprintf(file, "          \"name\": \"%.*s%s\",\n", len - 2, s + 1, a);
    }
    else {
        fprintf(file, "          \"name\": \"%.*s(%s)\",\n", len - 2, s + 1, a);
    }
    free(s);
    free(a);

    // assert((c->entry & VALUE_MASK) == VALUE_PC);   TODO
    fprintf(file, "          \"entry\": \"%d\",\n", (int) (c->entry >> VALUE_BITS));

    fprintf(file, "          \"pc\": \"%d\",\n", c->pc);
    fprintf(file, "          \"fp\": \"%d\",\n", c->fp);

#ifdef notdef
    {
        fprintf(file, "STACK %d:\n", c->fp);
        for (int x = 0; x < c->sp; x++) {
            fprintf(file, "    %d: %s\n", x, value_string(c->stack[x]));
        }
    }
#endif

    fprintf(file, "          \"trace\": [\n");
    print_trace(global, file, c, c->pc, c->fp, c->vars);
    fprintf(file, "\n");
    fprintf(file, "          ],\n");

    if (c->failure != 0) {
        s = value_string(c->failure);
		int len = strlen(s);
        fprintf(file, "          \"failure\": \"%.*s\",\n", len - 2, s + 1);
        free(s);
    }

    if (c->trap_pc != 0) {
        s = value_string(c->trap_pc);
        a = value_string(c->trap_arg);
        if (*a == '(') {
            fprintf(file, "          \"trap\": \"%s%s\",\n", s, a);
        }
        else {
            fprintf(file, "          \"trap\": \"%s(%s)\",\n", s, a);
        }
        free(s);
    }

    if (c->interruptlevel) {
        fprintf(file, "          \"interruptlevel\": \"1\",\n");
    }

    if (c->atomic != 0) {
        fprintf(file, "          \"atomic\": \"%d\",\n", c->atomic);
    }
    if (c->readonly != 0) {
        fprintf(file, "          \"readonly\": \"%d\",\n", c->readonly);
    }

    if (c->terminated) {
        fprintf(file, "          \"mode\": \"terminated\",\n");
    }
    else if (c->failure != 0) {
        fprintf(file, "          \"mode\": \"failed\",\n");
    }
    else if (c->stopped) {
        fprintf(file, "          \"mode\": \"stopped\",\n");
    }
    else {
        fprintf(file, "          \"mode\": \"%s\",\n", ctx_status(node, ctx));
    }

#ifdef notdef
    fprintf(file, "          \"stack\": [\n");
    for (int i = 0; i < c->sp; i++) {
        s = value_string(c->stack[i]);
        if (i < c->sp - 1) {
            fprintf(file, "            \"%s\",\n", s);
        }
        else {
            fprintf(file, "            \"%s\"\n", s);
        }
        free(s);
    }
    fprintf(file, "          ],\n");
#endif

    s = value_json(c->this);
    fprintf(file, "          \"this\": %s\n", s);
    free(s);

    fprintf(file, "        }");
}

void print_state(
    struct global_t *global,
    FILE *file,
    struct node *node
) {

#ifdef notdef
    fprintf(file, "      \"shared\": ");
    print_vars(file, node->state->vars);
    fprintf(file, ",\n");
#endif

    struct state *state = node->state;
    extern int invariant_cnt(const void *env);
    struct step inv_step;
    memset(&inv_step, 0, sizeof(inv_step));
    inv_step.ctx = new_alloc(struct context);

    // uint64_t inv_nv = value_put_atom("name", 4);
    // uint64_t inv_tv = value_put_atom("tag", 3);
    inv_step.ctx->name = value_put_atom(&global->values, "__invariant__", 13);
    inv_step.ctx->arg = VALUE_DICT;
    inv_step.ctx->this = VALUE_DICT;
    inv_step.ctx->vars = VALUE_DICT;
    inv_step.ctx->atomic = inv_step.ctx->readonly = 1;
    inv_step.ctx->interruptlevel = false;

    fprintf(file, "      \"invfails\": [");
    assert((state->invariants & VALUE_MASK) == VALUE_SET);
    int size;
    uint64_t *vals = value_get(state->invariants, &size);
    size /= sizeof(uint64_t);
    int nfailures = 0;
    for (int i = 0; i < size; i++) {
        assert((vals[i] & VALUE_MASK) == VALUE_PC);
        inv_step.ctx->pc = vals[i] >> VALUE_BITS;
        assert(strcmp(global->code.instrs[inv_step.ctx->pc].oi->name, "Invariant") == 0);
        int end = invariant_cnt(global->code.instrs[inv_step.ctx->pc].env);
        bool b = invariant_check(global, state, &inv_step, end);
        if (inv_step.ctx->failure != 0) {
            b = false;
        }
        if (!b) {
            if (nfailures != 0) {
                fprintf(file, ",");
            }
            fprintf(file, "\n        {\n");
            fprintf(file, "          \"pc\": \"%"PRIu64"\",\n", vals[i] >> VALUE_BITS);
            if (inv_step.ctx->failure == 0) {
                fprintf(file, "          \"reason\": \"invariant violated\"\n");
            }
            else {
                char *val = value_string(inv_step.ctx->failure);
				int len = strlen(val);
                fprintf(file, "          \"reason\": \"%.*s\"\n", len - 2, val + 1);
                free(val);
            }
            nfailures++;
            fprintf(file, "        }");
        }
    }
    fprintf(file, "\n      ],\n");
    free(inv_step.ctx);

    fprintf(file, "      \"contexts\": [\n");
    for (int i = 0; i < global->nprocesses; i++) {
        print_context(global, file, global->processes[i], i, node);
        if (i < global->nprocesses - 1) {
            fprintf(file, ",");
        }
        fprintf(file, "\n");
    }
    fprintf(file, "      ]\n");
}

void diff_state(
    struct global_t *global,
    FILE *file,
    struct state *oldstate,
    struct state *newstate,
    struct context *oldctx,
    struct context *newctx,
    bool interrupt,
    bool choose,
    uint64_t choice
) {
    if (global->dumpfirst) {
        global->dumpfirst = false;
    }
    else {
        fprintf(file, ",");
    }
    fprintf(file, "\n        {\n");
    if (newstate->vars != oldstate->vars) {
        fprintf(file, "          \"shared\": ");
        print_vars(file, newstate->vars);
        fprintf(file, ",\n");
    }
    if (interrupt) {
        fprintf(file, "          \"interrupt\": \"True\",\n");
    }
    if (choose) {
        char *val = value_json(choice);
        fprintf(file, "          \"choose\": %s,\n", val);
        free(val);
    }
    fprintf(file, "          \"npc\": \"%d\",\n", newctx->pc);
    if (newctx->fp != oldctx->fp) {
        fprintf(file, "          \"fp\": \"%d\",\n", newctx->fp);
        fprintf(file, "          \"trace\": [\n");
        print_trace(global, file, newctx, newctx->pc, newctx->fp, newctx->vars);
        fprintf(file, "\n");
        fprintf(file, "          ],\n");
    }
    if (newctx->this != oldctx->this) {
        char *val = value_json(newctx->this);
        fprintf(file, "          \"this\": %s,\n", val);
        free(val);
    }
    if (newctx->vars != oldctx->vars) {
        fprintf(file, "          \"local\": ");
        print_vars(file, newctx->vars);
        fprintf(file, ",\n");
    }
    if (newctx->atomic != oldctx->atomic) {
        fprintf(file, "          \"atomic\": \"%d\",\n", newctx->atomic);
    }
    if (newctx->readonly != oldctx->readonly) {
        fprintf(file, "          \"readonly\": \"%d\",\n", newctx->readonly);
    }
    if (newctx->interruptlevel != oldctx->interruptlevel) {
        fprintf(file, "          \"interruptlevel\": \"%d\",\n", newctx->interruptlevel ? 1 : 0);
    }
    if (newctx->failure != 0) {
        char *val = value_string(newctx->failure);
		int len = strlen(val);
        fprintf(file, "          \"failure\": \"%.*s\",\n", len - 2, val + 1);
        fprintf(file, "          \"mode\": \"failed\",\n");
        free(val);
    }
    else if (newctx->terminated) {
        fprintf(file, "          \"mode\": \"terminated\",\n");
    }

    int common;
    for (common = 0; common < newctx->sp && common < oldctx->sp; common++) {
        if (newctx->stack[common] != oldctx->stack[common]) {
            break;
        }
    }
    if (common < oldctx->sp) {
        fprintf(file, "          \"pop\": \"%d\",\n", oldctx->sp - common);
    }
    fprintf(file, "          \"push\": [");
    for (int i = common; i < newctx->sp; i++) {
        if (i > common) {
            fprintf(file, ",");
        }
        char *val = value_json(newctx->stack[i]);
        fprintf(file, " %s", val);
        free(val);
    }
    fprintf(file, " ],\n");

    fprintf(file, "          \"pc\": \"%d\"\n", oldctx->pc);

    fprintf(file, "        }");
}

void diff_dump(
    struct global_t *global,
    FILE *file,
    struct state *oldstate,
    struct state *newstate,
    struct context **oldctx,
    struct context *newctx,
    bool interrupt,
    bool choose,
    uint64_t choice
) {
    int newsize = sizeof(*newctx) + (newctx->sp * sizeof(uint64_t));

    if (memcmp(oldstate, newstate, sizeof(struct state)) == 0 &&
            (*oldctx)->sp == newctx->sp &&
            memcmp(*oldctx, newctx, newsize) == 0) {
        return;
    }

    // Keep track of old state and context for taking diffs
    diff_state(global, file, oldstate, newstate, *oldctx, newctx, interrupt, choose, choice);
    *oldstate = *newstate;
    free(*oldctx);
    *oldctx = malloc(newsize);
    memcpy(*oldctx, newctx, newsize);
}

// similar to onestep.  TODO.  Use flag to onestep?
uint64_t twostep(
    struct global_t *global,
    FILE *file,
    struct node *node,
    uint64_t ctx,
    uint64_t choice,
    bool interrupt,
    struct state *oldstate,
    struct context **oldctx,
    uint64_t nextvars
){
    // Make a copy of the state
    struct state *sc = new_alloc(struct state);
    memcpy(sc, node->state, sizeof(*sc));
    sc->choosing = 0;

    struct step step;
    memset(&step, 0, sizeof(step));
    step.ctx = value_copy(ctx, NULL);
    if (step.ctx->terminated || step.ctx->failure != 0) {
        free(step.ctx);
        return ctx;
    }

    if (interrupt) {
		assert(step.ctx->trap_pc != 0);
        interrupt_invoke(&step);
        diff_dump(global, file, oldstate, sc, oldctx, step.ctx, true, false, 0);
    }

    struct dict *infloop = NULL;        // infinite loop detector
    for (int loopcnt = 0;; loopcnt++) {
        int pc = step.ctx->pc;

        struct op_info *oi = global->code.instrs[pc].oi;
        if (global->code.instrs[pc].choose) {
            step.ctx->stack[step.ctx->sp - 1] = choice;
            step.ctx->pc++;
        }
        else {
            (*oi->op)(global->code.instrs[pc].env, sc, &step, global);
        }

        if (!step.ctx->terminated && step.ctx->failure == 0) {
            if (infloop == NULL) {
                infloop = dict_new(0);
            }

            int stacksize = step.ctx->sp * sizeof(uint64_t);
            int combosize = sizeof(struct combined) + stacksize;
            struct combined *combo = calloc(1, combosize);
            combo->state = *sc;
            memcpy(&combo->context, step.ctx, sizeof(*step.ctx) + stacksize);
            void **p = dict_insert(infloop, combo, combosize);
            free(combo);
            if (*p == (void *) 0) {
                *p = (void *) 1;
            }
            else {
                step.ctx->failure = value_put_atom(&global->values, "infinite loop", 13);
            }
        }

        diff_dump(global, file, oldstate, sc, oldctx, step.ctx, false, global->code.instrs[pc].choose, choice);
        if (step.ctx->terminated || step.ctx->failure != 0 || step.ctx->stopped) {
            break;
        }
        if (step.ctx->pc == pc) {
            fprintf(stderr, ">>> %s\n", oi->name);
        }
        assert(step.ctx->pc != pc);

        /* Peek at the next instruction.
         */
        oi = global->code.instrs[step.ctx->pc].oi;
        if (global->code.instrs[step.ctx->pc].choose) {
            assert(step.ctx->sp > 0);
            if (0 && step.ctx->readonly > 0) {    // TODO
                value_ctx_failure(step.ctx, &global->values, "can't choose in assertion or invariant");
                diff_dump(global, file, oldstate, sc, oldctx, step.ctx, false, global->code.instrs[pc].choose, choice);
                break;
            }
            uint64_t s = step.ctx->stack[step.ctx->sp - 1];
            if ((s & VALUE_MASK) != VALUE_SET) {
                value_ctx_failure(step.ctx, &global->values, "choose operation requires a set");
                diff_dump(global, file, oldstate, sc, oldctx, step.ctx, false, global->code.instrs[pc].choose, choice);
                break;
            }
            int size;
            uint64_t *vals = value_get(s, &size);
            size /= sizeof(uint64_t);
            if (size == 0) {
                value_ctx_failure(step.ctx, &global->values, "choose operation requires a non-empty set");
                diff_dump(global, file, oldstate, sc, oldctx, step.ctx, false, global->code.instrs[pc].choose, choice);
                break;
            }
            if (size == 1) {
                choice = vals[0];
            }
            else {
                break;
            }
        }

        if (!step.ctx->atomicFlag && sc->ctxbag != VALUE_DICT &&
                                    global->code.instrs[step.ctx->pc].breakable) {
            if (!step.ctx->atomicFlag && step.ctx->atomic > 0) {
                step.ctx->atomicFlag = true;
            }
            break;
        }
    }

    // assert(sc->vars == nextvars);
    ctx = value_put_context(&global->values, step.ctx);

    free(sc);
    free(step.ctx);
    free(step.log);

    return ctx;
}

void path_dump(
    struct global_t *global,
    FILE *file,
    struct node *last,
    uint64_t choice,
    struct state *oldstate,
    struct context **oldctx,
    bool interrupt
) {
    struct node *node = last;

    last = last->parent;
    if (last->parent == NULL) {
        fprintf(file, "\n");
    }
    else {
        path_dump(global, file, last, last->choice, oldstate, oldctx, last->interrupt);
        fprintf(file, ",\n");
    }

    fprintf(file, "    {\n");
    fprintf(file, "      \"id\": \"%d\",\n", node->id);

    /* Find the starting context in the list of processes.
     */
    uint64_t ctx = node->before;
    int pid;
    for (pid = 0; pid < global->nprocesses; pid++) {
        if (global->processes[pid] == ctx) {
            break;
        }
    }

    struct context *context = value_get(ctx, NULL);
    assert(!context->terminated);
    char *name = value_string(context->name);
	int len = strlen(name);
    char *arg = json_escape_value(context->arg);
    // char *c = value_string(choice);
    fprintf(file, "      \"tid\": \"%d\",\n", pid);
    fprintf(file, "      \"xhash\": \"%"PRIx64"\",\n", ctx);
    if (*arg == '(') {
        fprintf(file, "      \"name\": \"%.*s%s\",\n", len - 2, name + 1, arg);
    }
    else {
        fprintf(file, "      \"name\": \"%.*s(%s)\",\n", len - 2, name + 1, arg);
    }
    // fprintf(file, "      \"choice\": \"%s\",\n", c);
    global->dumpfirst = true;
    fprintf(file, "      \"microsteps\": [");
    free(name);
    free(arg);
    // free(c);
    memset(*oldctx, 0, sizeof(**oldctx));
    (*oldctx)->pc = context->pc;

    // Recreate the steps
    assert(pid < global->nprocesses);
    global->processes[pid] = twostep(
        global,
        file,
        last,
        ctx,
        choice,
        interrupt,
        oldstate,
        oldctx,
        node->state->vars
    );
    fprintf(file, "\n      ],\n");

    /* Match each context to a process.
     */
    bool *matched = calloc(global->nprocesses, sizeof(bool));
    int nctxs;
    uint64_t *ctxs = value_get(node->state->ctxbag, &nctxs);
    nctxs /= sizeof(uint64_t);
    for (int i = 0; i < nctxs; i += 2) {
        assert((ctxs[i] & VALUE_MASK) == VALUE_CONTEXT);
        assert((ctxs[i+1] & VALUE_MASK) == VALUE_INT);
        int cnt = ctxs[i+1] >> VALUE_BITS;
        for (int j = 0; j < cnt; j++) {
            int k;
            for (k = 0; k < global->nprocesses; k++) {
                if (!matched[k] && global->processes[k] == ctxs[i]) {
                    matched[k] = true;
                    break;
                }
            }
            if (k == global->nprocesses) {
                global->processes = realloc(global->processes, (global->nprocesses + 1) * sizeof(uint64_t));
                matched = realloc(matched, (global->nprocesses + 1) * sizeof(bool));
                global->processes[global->nprocesses] = ctxs[i];
                matched[global->nprocesses] = true;
                global->nprocesses++;
            }
        }
    }
    free(matched);
  
    print_state(global, file, node);
    fprintf(file, "    }");
}

static char *json_string_encode(char *s, int len){
    char *result = malloc(4 * len), *p = result;

    while (len > 0) {
        switch (*s) {
        case '\r':
            *p++ = '\\'; *p++ = 'r';
            break;
        case '\n':
            *p++ = '\\'; *p++ = 'n';
            break;
        case '\f':
            *p++ = '\\'; *p++ = 'f';
            break;
        case '\t':
            *p++ = '\\'; *p++ = 't';
            break;
        case '"':
            *p++ = '\\'; *p++ = '"';
            break;
        case '\\':
            *p++ = '\\'; *p++ = '\\';
            break;
        default:
            *p++ = *s;
        }
        s++;
        len--;
    }
    *p++ = 0;
    return result;
}

struct enum_loc_env_t {
    FILE *out;
    struct dict *code_map;
};

static void enum_loc(
    void *env,
    const void *key,
    unsigned int key_size,
    HASHDICT_VALUE_TYPE value
) {
    static bool notfirst = false;
    struct enum_loc_env_t *enum_loc_env = env;
    FILE *out = enum_loc_env->out;
    struct dict *code_map = enum_loc_env->code_map;

    if (notfirst) {
        fprintf(out, ",\n");
    }
    else {
        notfirst = true;
        fprintf(out, "\n");
    }

    // Get program counter
    char *pcc = malloc(key_size + 1);
    memcpy(pcc, key, key_size);
    pcc[key_size] = 0;
    int pc = atoi(pcc);
    free(pcc);

    fprintf(out, "    \"%.*s\": { ", key_size, (char *) key);

    struct json_value *jv = value;
    assert(jv->type == JV_MAP);

    struct json_value *file = dict_lookup(jv->u.map, "file", 4);
    assert(file->type == JV_ATOM);
    fprintf(out, "\"file\": \"%s\", ", json_string_encode(file->u.atom.base, file->u.atom.len));

    struct json_value *line = dict_lookup(jv->u.map, "line", 4);
    assert(line->type == JV_ATOM);
    fprintf(out, "\"line\": \"%.*s\", ", line->u.atom.len, line->u.atom.base);

    void **p = dict_insert(code_map, &pc, sizeof(pc));
    int r = asprintf((char **) p, "%.*s:%.*s", file->u.atom.len, file->u.atom.base, line->u.atom.len, line->u.atom.base);

    struct json_value *code = dict_lookup(jv->u.map, "code", 4);
    assert(code->type == JV_ATOM);
    fprintf(out, "\"code\": \"%s\"", json_string_encode(code->u.atom.base, code->u.atom.len));
    fprintf(out, " }");
}

enum busywait { BW_ESCAPE, BW_RETURN, BW_VISITED };
static enum busywait is_stuck(
    struct node *start,
    struct node *node,
    uint64_t ctx,
    bool change
) {
	if (node->component != start->component) {
		return BW_ESCAPE;
	}
	if (node->visited) {
		return BW_VISITED;
	}
    change = change || (node->state->vars != start->state->vars);
	node->visited = true;
	enum busywait result = BW_ESCAPE;
    for (struct edge *edge = node->fwd; edge != NULL; edge = edge->next) {
        if (edge->ctx == ctx) {
			if (edge->node == node) {
				node->visited = false;
				return BW_ESCAPE;
			}
			if (edge->node == start) {
				if (!change) {
					node->visited = false;
					return BW_ESCAPE;
				}
				result = BW_RETURN;
			}
			else {
				enum busywait bw = is_stuck(start, edge->node, edge->after, change);
				switch (bw) {
				case BW_ESCAPE:
					node->visited = false;
					return BW_ESCAPE;
				case BW_RETURN:
					result = BW_RETURN;
					break;
				case BW_VISITED:
					break;
				default:
					assert(false);
				}
			}
        }
    }
	node->visited = false;
    return result;
}

static void detect_busywait(struct minheap *failures, struct node *node){
	// Get the contexts
	int size;
	uint64_t *ctxs = value_get(node->state->ctxbag, &size);
	size /= sizeof(uint64_t);

	for (int i = 0; i < size; i += 2) {
		if (is_stuck(node, node, ctxs[i], false) == BW_RETURN) {
			struct failure *f = new_alloc(struct failure);
			f->type = FAIL_BUSYWAIT;
			f->choice = node->choice;
			f->node = node;
			minheap_insert(failures, f);
			break;
		}
	}
}

static int node_cmp(void *n1, void *n2){
    struct node *node1 = n1, *node2 = n2;

    if (node1->len != node2->len) {
        return node1->len - node2->len;
    }
    if (node1->steps != node2->steps) {
        return node1->steps - node2->steps;
    }
    return node1->id - node2->id;
}

static int fail_cmp(void *f1, void *f2){
    struct failure *fail1 = f1, *fail2 = f2;

    return node_cmp(fail1->node, fail2->node);
}

void possibly_check(struct dict *possibly_cnt, struct code_t *code, int pc) {
    const struct env_Possibly *ep = code->instrs[pc].env;

    void *cnt = dict_lookup(possibly_cnt, &pc, sizeof(pc));
    if (cnt == 0) {
        char *loc = dict_lookup(code->code_map, &pc, sizeof(pc));
        if (loc == NULL) {
            printf("invalidated possibly pc=%d/%d\n", pc, ep->index);
        }
        else {
            printf("invalidated possibly %s/%d\n", loc, ep->index);
        }
    }
}

static void do_work(struct worker *w){
	while (!minheap_empty(w->todo)) {
		struct node *node = minheap_getmin(w->todo);
		struct state *state = node->state;
		w->global->dequeued++; // TODO race condition

		if (state->choosing != 0) {
			assert((state->choosing & VALUE_MASK) == VALUE_CONTEXT);
			if (false) {
				printf("CHOOSING %"PRIx64"\n", state->choosing);
			}

			struct context *cc = value_get(state->choosing, NULL);
			assert(cc != NULL);
			assert(cc->sp > 0);
			uint64_t s = cc->stack[cc->sp - 1];
			assert((s & VALUE_MASK) == VALUE_SET);
			int size;
			uint64_t *vals = value_get(s, &size);
			size /= sizeof(uint64_t);
			assert(size > 0);
			for (int i = 0; i < size; i++) {
				make_step(
					w,
					node,
					state->choosing,
					vals[i],
					1
				);
			}
		}
		else {
			int size;
			uint64_t *ctxs = value_get(state->ctxbag, &size);
			size /= sizeof(uint64_t);
			assert(size >= 0);
			for (int i = 0; i < size; i += 2) {
				assert((ctxs[i] & VALUE_MASK) == VALUE_CONTEXT);
				assert((ctxs[i+1] & VALUE_MASK) == VALUE_INT);
				make_step(
					w,
					node,
					ctxs[i],
					0,
					ctxs[i+1] >> VALUE_BITS
				);
			}
		}
	}
}

static void *worker(void *arg){
    struct worker *w = arg;

    for (int epoch = 0;; epoch++) {
        pthread_barrier_wait(w->start_barrier);
		// printf("WORKER %d starting\n", w->index);
		do_work(w);
		// printf("WORKER %d finished\n", w->index);
        pthread_barrier_wait(w->end_barrier);
    }

    return NULL;
}

void process_results(
    struct global_t *global,
    struct dict *visited,
    struct minheap *todo[2],
    struct minheap *results
) {
    while (!minheap_empty(results)) {
        struct node *node = minheap_getmin(results);
        struct node *parent = node->parent, *next;
        bool must_free;     // whether node must be freed or not

        /* See if we already visited this node.
         */
        void **p = dict_insert(visited, node->state, sizeof(struct state));
        if ((next = *p) == NULL) {
            next = *p = node;
            graph_add(&global->graph, node);   // sets node->id
            minheap_insert(todo[0], node);
            global->enqueued++;
            must_free = false;
        }
        else {
            next = *p;
            must_free = true;
        }

        if (node->ftype != FAIL_NONE) {
            struct failure *f = new_alloc(struct failure);
            f->type = node->ftype;
            f->choice = node->choice;
            f->node = next;
            minheap_insert(global->failures, f);
        }

        // printf("ADD %d <=> %d\n", parent->id, node->id);

        // Add a forward edge from parent
        struct edge *fwd = new_alloc(struct edge);
        fwd->ctx = node->before;
        fwd->choice = node->choice;
        fwd->interrupt = node->interrupt;
        fwd->node = next;
        fwd->weight = node->weight;
        fwd->after = node->after;
        fwd->ai = node->ai;
        fwd->log = node->log;
        fwd->nlog = node->nlog;
        fwd->next = parent->fwd;
        parent->fwd = fwd;

        // Add a backward edge from node to parent.
        struct edge *bwd = new_alloc(struct edge);
        bwd->ctx = node->before;
        bwd->choice = node->choice;
        bwd->interrupt = node->interrupt;
        bwd->node = parent;
        bwd->weight = node->weight;
        bwd->after = node->after;
        bwd->ai = node->ai;
        bwd->log = node->log;
        bwd->nlog = node->nlog;
        bwd->next = node->bwd;
        node->bwd = bwd;

        if (must_free) {
            free(node);
        }
    }
}

char *state_string(struct state *state){
    void alloc_printf(char **r, char *fmt, ...);
    void append_printf(char **p, char *fmt, ...);

    char *r, *v;
    alloc_printf(&r, "{");
    v = value_string(state->vars);
    append_printf(&r, "%s", v); free(v);
    v = value_string(state->seqs);
    append_printf(&r, ",%s", v); free(v);
    v = value_string(state->choosing);
    append_printf(&r, ",%s", v); free(v);
    v = value_string(state->ctxbag);
    append_printf(&r, ",%s", v); free(v);
    v = value_string(state->stopbag);
    append_printf(&r, ",%s", v); free(v);
    v = value_string(state->termbag);
    append_printf(&r, ",%s", v); free(v);
    v = value_string(state->invariants);
    append_printf(&r, ",%s}", v); free(v);
    return r;
}

static void pr_state(struct global_t *global, FILE *fp, struct state *state, int index){
    char *v = state_string(state);
    fprintf(fp, "%s\n", v);
    free(v);
}

static void usage(char *prog){
    fprintf(stderr, "Usage: %s [-c] [-t maxtime] file.json\n", prog);
    exit(1);
}

int main(int argc, char **argv){
    bool cflag = false;
    int i, maxtime = 300000000 /* about 10 years */;
    for (i = 1; i < argc; i++) {
        if (*argv[i] != '-') {
            break;
        }
        switch (argv[i][1]) {
        case 'c':
            cflag = true;
            break;
        case 't':
            maxtime = atoi(&argv[i][2]);
            if (maxtime <= 0) {
                fprintf(stderr, "%s: negative timeout\n", argv[0]);
                exit(1);
            }
            break;
        case 'x':
            printf("Charm model checker working\n");
            return 0;
        default:
            usage(argv[0]);
        }
    }
    if (argc - i > 1) {
        usage(argv[0]);
    }
    char *fname = i == argc ? "harmony.hvm" : argv[i];

    char *outfile, *dotloc = strrchr(fname, '.');
    int r;
    if (dotloc == NULL) {
        r = asprintf(&outfile, "%s.hco", fname);
    }
    else {
        r = asprintf(&outfile, "%.*s.hco", (int) (dotloc - fname), fname);
    }
    if (r < 0) {       // mostly to suppress asprintf return value warning
        outfile = "harmony.hco";
    }

    double timeout = gettime() + maxtime;

    // initialize modules
    struct global_t *global = malloc(sizeof(struct global_t));
    value_init(&global->values);
    ops_init(global);
    graph_init(&global->graph, 1024);
    global->failures = minheap_create(fail_cmp);
    global->processes = NULL;
    global->nprocesses = 0;
    global->lasttime = 0;
    global->enqueued = 0;
    global->dequeued = 0;
    global->dumpfirst = false;
    global->possibly_cnt = dict_new(0);

    // open the file
    FILE *fp = fopen(fname, "r");
    if (fp == NULL) {
        fprintf(stderr, "%s: can't open %s\n", argv[0], fname);
        exit(1);
    }

    // read the file
    json_buf_t buf;
    buf.base = malloc(CHUNKSIZE);
    buf.len = 0;
    int n;
    while ((n = fread(&buf.base[buf.len], 1, CHUNKSIZE, fp)) > 0) {
        buf.len += n;
        buf.base = realloc(buf.base, buf.len + CHUNKSIZE);
    }
    fclose(fp);

    // parse the contents
    struct json_value *jv = json_parse_value(&buf);
    assert(jv->type == JV_MAP);

    // travel through the json code contents to create the code array
    struct json_value *jc = dict_lookup(jv->u.map, "code", 4);
    assert(jc->type == JV_LIST);
    global->code = code_init_parse(&global->values, jc);

    // Create an initial state
	uint64_t this = value_put_atom(&global->values, "this", 4);
    struct context *init_ctx = new_alloc(struct context);
    init_ctx->name = value_put_atom(&global->values, "__init__", 8);
    init_ctx->arg = VALUE_DICT;
    init_ctx->this = VALUE_DICT;
    init_ctx->vars = VALUE_DICT;
    init_ctx->atomic = 1;
    init_ctx->atomicFlag = true;
    value_ctx_push(&init_ctx, (CALLTYPE_PROCESS << VALUE_BITS) | VALUE_INT);
    value_ctx_push(&init_ctx, VALUE_DICT);
    struct state *state = new_alloc(struct state);
    state->vars = VALUE_DICT;
    state->seqs = VALUE_SET;
    uint64_t ictx = value_put_context(&global->values, init_ctx);
    state->ctxbag = value_dict_store(&global->values, VALUE_DICT, ictx, (1 << VALUE_BITS) | VALUE_INT);
    state->stopbag = VALUE_DICT;
    state->invariants = VALUE_SET;
    global->processes = new_alloc(uint64_t);
    *global->processes = ictx;
    global->nprocesses = 1;

    // Put the initial state in the visited map
    struct dict *visited = dict_new(0);
    struct node *node = new_alloc(struct node);
    node->state = state;
    node->after = ictx;
    graph_add(&global->graph, node);
    void **p = dict_insert(visited, state, sizeof(*state));
    assert(*p == NULL);
    *p = node;

    struct minheap *todo[2];
    todo[0] = minheap_create(node_cmp);
    todo[1] = minheap_create(node_cmp);

    struct minheap *results[2];
    results[0] = minheap_create(node_cmp);
    results[1] = minheap_create(node_cmp);

    // Determine how many worker threads to use
    int nworkers = sysconf(_SC_NPROCESSORS_ONLN);
	printf("nworkers = %d\n", nworkers);
    pthread_barrier_t start_barrier, end_barrier;
    pthread_barrier_init(&start_barrier, NULL, nworkers + 1);
    pthread_barrier_init(&end_barrier, NULL, nworkers + 1);

    // Allocate space for worker info
    struct worker *workers = calloc(nworkers, sizeof(*workers));
    for (int i = 0; i < nworkers; i++) {
        struct worker *w = &workers[i];
        w->global = global;
        w->timeout = timeout;
        w->start_barrier = &start_barrier;
        w->end_barrier = &end_barrier;
        w->index = i;
        w->todo = minheap_create(node_cmp);
        w->results[0] = minheap_create(node_cmp);
        w->results[1] = minheap_create(node_cmp);

        // Create a context for evaluating invariants
        w->inv_step.ctx = new_alloc(struct context);
        w->inv_step.ctx->name = value_put_atom(&global->values, "__invariant__", 13);
        w->inv_step.ctx->arg = VALUE_DICT;
        w->inv_step.ctx->this = VALUE_DICT;
        w->inv_step.ctx->vars = VALUE_DICT;
        w->inv_step.ctx->atomic = w->inv_step.ctx->readonly = 1;
        w->inv_step.ctx->interruptlevel = false;
    }

    // Start the workers, who'll wait on the start barrier
    for (int i = 0; i < nworkers; i++) {
        int r = pthread_create(
            &workers[i].tid, NULL,
            worker, &workers[i]
        );
    }

    // Give the initial state to worker 0
    minheap_insert(workers[0].todo, node);
    global->enqueued++;

    double before = gettime(), postproc = 0;
    while (minheap_empty(global->failures)) {
        // Put the value dictionaries in concurrent mode
        value_set_concurrent(&global->values, 1);

        // make the threads work
        pthread_barrier_wait(&start_barrier);
        pthread_barrier_wait(&end_barrier);

        double before_postproc = gettime();

        // Deal with the unstable values
        value_set_concurrent(&global->values, 0);

        // Collect the results of all the workers
        for (int i = 0; i < nworkers; i++) {
            struct worker *w = &workers[i];
            assert(minheap_empty(w->todo));

            for (int weight = 0; weight <= 1; weight++) {
                while (!minheap_empty(w->results[weight])) {
                    struct node *node = minheap_getmin(w->results[weight]);
                    minheap_insert(results[weight], node);
                }
            }
        }

        process_results(global, visited, todo, results[0]);

        if (minheap_empty(todo[0])) {
            struct minheap *tmp = results[0];
            results[0] = results[1];
            results[1] = tmp;
            process_results(global, visited, todo, results[0]);
        }

#ifdef XXX
        if (!minheap_empty(global->failures)) {
            break;
        }
#endif

        if (minheap_empty(todo[0])) {
            break;
        }

        // Distribute the work evenly among the workers
        assert(!minheap_empty(todo[0]));
        int distr = 0;
        while (!minheap_empty(todo[0])) {
            struct node *node = minheap_getmin(todo[0]);
            minheap_insert(workers[distr].todo, node);
            if (++distr == nworkers) {
                distr = 0;
            }
        }

        postproc += gettime() - before_postproc;
    }

    printf("#states %d (time %.3lf+%.3lf)\n", global->graph.size, gettime() - before - postproc, postproc);

    if (minheap_empty(global->failures)) {
        // find the strongly connected components
        int ncomponents = graph_find_scc(&global->graph);

        // mark the ones that are good
        struct component *components = calloc(ncomponents, sizeof(*components));
        for (int i = 0; i < global->graph.size; i++) {
            struct node *node = global->graph.nodes[i];
			assert(node->component < ncomponents);
            struct component *comp = &components[node->component];
            if (comp->size == 0) {
                comp->representative = i;
            }
            comp->size++;
            if (comp->good) {
                continue;
            }
            // TODO.  In case of ctxbag, all contexts should probably be blocked
            if (value_ctx_all_eternal(node->state->ctxbag) && value_ctx_all_eternal(node->state->stopbag)) {
                comp->good = true;
                continue;
            }
            for (struct edge *edge = node->fwd;
                            edge != NULL && !comp->good; edge = edge->next) {
                if (edge->node->component != node->component) {
                    comp->good = true;
                }
            }
        }

        // now count the nodes that are in bad components
        int nbad = 0;
        for (int i = 0; i < global->graph.size; i++) {
            struct node *node = global->graph.nodes[i];
            if (!components[node->component].good) {
                nbad++;
                struct failure *f = new_alloc(struct failure);
                f->type = FAIL_TERMINATION;
                f->choice = node->choice;
                f->node = node;
                minheap_insert(global->failures, f);
            }
        }

        if (nbad == 0 && !cflag) {
            for (int i = 0; i < global->graph.size; i++) {
				global->graph.nodes[i]->visited = false;
			}
            for (int i = 0; i < global->graph.size; i++) {
                struct node *node = global->graph.nodes[i];
                if (components[node->component].size > 1) {
                    detect_busywait(global->failures, node);
                }
            }
        }

        printf("%d components, %d bad states\n", ncomponents, nbad);
    }

    if (false) {
        FILE *df = fopen("charm.dump", "w");
        assert(df != NULL);
        for (int i = 0; i < global->graph.size; i++) {
            struct node *node = global->graph.nodes[i];
            assert(node->id == i);
            fprintf(df, "\nNode %d:\n", node->id);
            fprintf(df, "    component: %d\n", node->component);
            if (node->parent != NULL) {
                fprintf(df, "    parent: %d\n", node->parent->id);
            }
            fprintf(df, "    vars: %s\n", value_string(node->state->vars));
            fprintf(df, "    fwd:\n");
            int eno = 0;
            for (struct edge *edge = node->fwd; edge != NULL; edge = edge->next, eno++) {
                fprintf(df, "        %d:\n", eno);
                struct context *ctx = value_get(edge->ctx, NULL);
                fprintf(df, "            context: %s %s %d\n", value_string(ctx->name), value_string(ctx->arg), ctx->pc);
                fprintf(df, "            choice: %s\n", value_string(edge->choice));
                fprintf(df, "            node: %d (%d)\n", edge->node->id, edge->node->component);
            }
        }
        fclose(df);
    }

    if (false) {
        FILE *df = fopen("charm.dump", "w");
        assert(df != NULL);
        char **table = malloc(global->graph.size * sizeof(char*));
        for (int i = 0; i < global->graph.size; i++) {
            struct node *node = global->graph.nodes[i];
            table[i] = state_string(node->state);
            fprintf(df, "%s\n", table[i]);
        }
        fclose(df);
    }

    // Look for data races
    struct minheap *warnings = minheap_create(fail_cmp);
    if (minheap_empty(global->failures)) {
        for (int i = 0; i < global->graph.size; i++) {
            struct node *node = global->graph.nodes[i];
            graph_check_for_data_race(node, warnings, &global->values);
            if (!minheap_empty(warnings)) {
                break;
            }
        }
    }

    FILE *out = fopen(outfile, "w");
    if (out == NULL) {
        fprintf(stderr, "charm: can't create %s\n", outfile);
        exit(1);
    }
    fprintf(out, "{\n");

    bool no_issues = minheap_empty(global->failures) && minheap_empty(warnings);
    if (no_issues) {
        printf("No issues\n");
        fprintf(out, "  \"issue\": \"No issues\",\n");

        // Figure out how many extra "intermediate" states we need.
        int extra = 0;
        for (int i = 0; i < global->graph.size; i++) {
            struct node *node = global->graph.nodes[i];
            for (struct edge *edge = node->fwd; edge != NULL; edge = edge->next) {
                if (edge->nlog > 1) {
                    extra += edge->nlog - 1;
                }
            }
        }

        fprintf(out, "  \"nodes\": [\n");
        bool first = true;
        for (int i = 0; i < global->graph.size; i++) {
            struct node *node = global->graph.nodes[i];
            assert(node->id == i);
            if (first) {
                first = false;
            }
            else {
                fprintf(out, ",\n");
            }
            fprintf(out, "    {\n");
            fprintf(out, "      \"idx\": %d,\n", node->id);
            fprintf(out, "      \"component\": %d,\n", node->component);
            if (node->parent != NULL) {
                fprintf(out, "      \"parent\": %d,\n", node->parent->id);
            }
            fprintf(out, "      \"value\": \"%s:%d\",\n", json_escape_value(node->state->vars), node->state->choosing != 0);
            if (i == 0) {
                fprintf(out, "      \"type\": \"initial\"\n");
            }
            else if (node->state->ctxbag == VALUE_DICT) {
                fprintf(out, "      \"type\": \"terminal\"\n");
            }
            else {
                fprintf(out, "      \"type\": \"normal\"\n");
            }
            fprintf(out, "    }");
        }
        for (int i = 0; i < extra; i++) {
            if (first) {
                first = false;
            }
            else {
                fprintf(out, ",\n");
            }
            fprintf(out, "    {\n");
            fprintf(out, "      \"idx\": \"i%d\",\n", i);
            fprintf(out, "      \"type\": \"intermediate\"\n");
            fprintf(out, "    }");
        }
        fprintf(out, "\n");
        fprintf(out, "  ],\n");
        fprintf(out, "  \"edges\": [\n");
        first = true;
        for (int i = 0; i < global->graph.size; i++) {
            struct node *node = global->graph.nodes[i];
            for (struct edge *edge = node->fwd; edge != NULL; edge = edge->next) {
                if (first) {
                    first = false;
                }
                else {
                    fprintf(out, ",\n");
                }
                fprintf(out, "    {\n");
                fprintf(out, "      \"src\": %d,\n", node->id);
                fprintf(out, "      \"dst\": %d,\n", edge->node->id);
                fprintf(out, "      \"log\": [");
                bool first_log = true;
                for (int j = 0; j < edge->nlog; j++) {
                    if (first_log) {
                        first_log = false;
                        fprintf(out, "\n");
                    }
                    else {
                        fprintf(out, ",\n");
                    }
                    char *p = json_escape_value(edge->log[j]);
                    fprintf(out, "        \"%s\"", p);
                    free(p);
                }
                fprintf(out, "\n");
                fprintf(out, "      ]\n");
                fprintf(out, "    }");
            }
        }
        fprintf(out, "\n");
        fprintf(out, "  ],\n");
    }
    else {
        // Find shortest "bad" path
        struct failure *bad = NULL;
        if (minheap_empty(global->failures)) {
            bad = minheap_getmin(warnings);
        }
        else {
            bad = minheap_getmin(global->failures);
        }

        switch (bad->type) {
        case FAIL_SAFETY:
            printf("Safety Violation\n");
            fprintf(out, "  \"issue\": \"Safety violation\",\n");
            break;
        case FAIL_INVARIANT:
            printf("Invariant Violation\n");
            fprintf(out, "  \"issue\": \"Invariant violation\",\n");
            break;
        case FAIL_TERMINATION:
            printf("Non-terminating state\n");
            fprintf(out, "  \"issue\": \"Non-terminating state\",\n");
            break;
        case FAIL_BUSYWAIT:
            printf("Active busy waiting\n");
            fprintf(out, "  \"issue\": \"Active busy waiting\",\n");
            no_issues = true;       // to report possibly stuff
            break;
        case FAIL_RACE:
            assert(bad->address != VALUE_ADDRESS);
            char *addr = value_string(bad->address);
            char *json = json_string_encode(addr, strlen(addr));
            printf("Data race (%s)\n", json);
            fprintf(out, "  \"issue\": \"Data race (%s)\",\n", json);
            free(json);
            free(addr);
            no_issues = true;       // to report possibly stuff
            break;
        default:
            panic("main: bad fail type");
        }

        fprintf(out, "  \"macrosteps\": [");
        struct state oldstate;
        memset(&oldstate, 0, sizeof(oldstate));
        struct context *oldctx = calloc(1, sizeof(*oldctx));
        global->dumpfirst = true;
        path_dump(global, out, bad->node, bad->choice, &oldstate, &oldctx, false);
        fprintf(out, "\n");
        free(oldctx);
        fprintf(out, "  ],\n");
    }

    fprintf(out, "  \"code\": [\n");
    jc = dict_lookup(jv->u.map, "pretty", 6);
    assert(jc->type == JV_LIST);
    for (int i = 0; i < jc->u.list.nvals; i++) {
        struct json_value *next = jc->u.list.vals[i];
        assert(next->type == JV_LIST);
        assert(next->u.list.nvals == 2);
        struct json_value *codestr = next->u.list.vals[0];
        assert(codestr->type == JV_ATOM);
		char *v = json_escape(codestr->u.atom.base, codestr->u.atom.len);
        fprintf(out, "    \"%s\"", v);
		free(v);
        if (i < jc->u.list.nvals - 1) {
            fprintf(out, ",");
        }
        fprintf(out, "\n");
    }
    fprintf(out, "  ],\n");

    fprintf(out, "  \"explain\": [\n");
    for (int i = 0; i < jc->u.list.nvals; i++) {
        struct json_value *next = jc->u.list.vals[i];
        assert(next->type == JV_LIST);
        assert(next->u.list.nvals == 2);
        struct json_value *codestr = next->u.list.vals[1];
        assert(codestr->type == JV_ATOM);
		char *v = json_escape(codestr->u.atom.base, codestr->u.atom.len);
        fprintf(out, "    \"%s\"", v);
		free(v);
        if (i < jc->u.list.nvals - 1) {
            fprintf(out, ",");
        }
        fprintf(out, "\n");
    }
    fprintf(out, "  ],\n");

    fprintf(out, "  \"locations\": {");
    jc = dict_lookup(jv->u.map, "locations", 9);
    assert(jc->type == JV_MAP);
    struct enum_loc_env_t enum_loc_env;
    enum_loc_env.out = out;
    enum_loc_env.code_map = global->code.code_map;
    dict_iter(jc->u.map, enum_loc, &enum_loc_env);
    fprintf(out, "\n  }\n");

    fprintf(out, "}\n");
	fclose(out);

    if (no_issues) {
        for (int i = 0; i < global->code.len; i++) {
            if (strcmp(global->code.instrs[i].oi->name, "Possibly") == 0) {
                possibly_check(global->possibly_cnt, &global->code, i);
            }
        }
    }

    iface_write_spec_graph_to_file(global, "iface.gv");

    iface_write_spec_graph_to_json_file(global, "iface.json");

    free(global);
    return 0;
}
