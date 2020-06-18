#include "memory.h"
#include "fiber.h"
#include "unit.h"
#include "trivia/util.h"
#include "errinj.h"

static struct fiber_attr default_attr;

static int
noop_f(va_list ap)
{
	return 0;
}

static int
main_f(va_list ap)
{
	struct slab_cache *slabc = &cord()->slabc;
	size_t used_before, used_after;
	struct errinj *inj;
	struct fiber *fiber;
	int rc = 0;

	header();
	plan(6);

	/*
	 * Set non-default stack size to prevent reusing of an
	 * existing fiber.
	 */
	struct fiber_attr *fiber_attr = fiber_attr_new();
	fiber_attr_setstacksize(fiber_attr, default_attr.stack_size * 2);

	/*
	 * Clear the fiber's diagnostics area to check that failed
	 * fiber_new() sets an error.
	 */
	diag_clear(diag_get());

	/*
	 * Check guard page setup via mprotect. We can't test the fiber
	 * destroy path since it clears fiber's diag.
	 */
	inj = errinj(ERRINJ_FIBER_MPROTECT, ERRINJ_INT);
	inj->iparam = PROT_NONE;
	fiber = fiber_new_ex("test_mprotect", fiber_attr, noop_f);
	inj->iparam = -1;

	ok(fiber == NULL, "mprotect: failed to setup fiber guard page");
	ok(diag_get() != NULL, "mprotect: diag is armed after error");

	/*
	 * Check madvise error on fiber creation.
	 */
	diag_clear(diag_get());
	inj = errinj(ERRINJ_FIBER_MADVISE, ERRINJ_BOOL);
	inj->bparam = true;
	fiber = fiber_new_ex("test_madvise", fiber_attr, noop_f);
	inj->bparam = false;

	ok(fiber != NULL, "madvise: non critical error on madvise hint");
	ok(diag_get() != NULL, "madvise: diag is armed after error");

	/*
	 * Check if we leak on fiber destruction.
	 * We will print an error and result get
	 * compared by testing engine.
	 */
	fiber_attr_delete(fiber_attr);
	fiber_attr = fiber_attr_new();
	fiber_attr->flags |= FIBER_CUSTOM_STACK;
	fiber_attr->stack_size = 64 << 10;

	diag_clear(diag_get());

	used_before = slabc->allocated.stats.used;

	fiber = fiber_new_ex("test_madvise", fiber_attr, noop_f);
	ok(fiber != NULL, "fiber with custom stack");
	fiber_set_joinable(fiber, true);

	inj = errinj(ERRINJ_FIBER_MPROTECT, ERRINJ_INT);
	inj->iparam = PROT_READ | PROT_WRITE;

	fiber_start(fiber);
	rc = fiber_join(fiber);
	fail_if(rc);
	inj->iparam = -1;

	used_after = slabc->allocated.stats.used;
	ok(used_after > used_before, "expected leak detected");

	fiber_attr_delete(fiber_attr);
	footer();

	ev_break(loop(), EVBREAK_ALL);
	return check_plan();
}

int main()
{
	plan(0);
	memory_init();
	fiber_init(fiber_c_invoke);
	fiber_attr_create(&default_attr);
	struct fiber *f = fiber_new("main", main_f);
	fail_if(!f);
	fiber_wakeup(f);
	ev_run(loop(), 0);
	fiber_free();
	memory_free();
	check_plan();

	return 0;
}
