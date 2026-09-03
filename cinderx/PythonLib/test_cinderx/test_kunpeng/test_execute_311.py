# Copyright (c) Meta Platforms, Inc. and affiliates.

"""The CPython 3.11 execute mode: the product auto-JIT (MR-11).

Every scenario runs in a child interpreter, because the mode is parsed
from the environment when the evaluator is installed.  The child programs
are real Python files under `execute311_programs/`; each prints a JOURNAL
line that the tests here assert on.  Each test names the regression it
pins in a short leading comment.
"""

import json
import os
import subprocess
import sys
import unittest
from pathlib import Path

PROGRAMS = Path(__file__).resolve().parent / "execute311_programs"

THRESHOLD = 30


def _clean_env():
    # Anything JIT-related inherited from the parent would silently change
    # the child's mode; the child sees exactly what the test sets.
    return {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(("PYTHONJIT", "CINDERX_", "PARALLEL_GC_"))
    }


def run_program(name, *, mode="execute", threshold=THRESHOLD, timeout=120, **env):
    child_env = _clean_env()
    if mode is not None:
        child_env["CINDERX_JIT_MODE"] = mode
    if threshold is not None:
        child_env["PYTHONJITAUTO"] = str(threshold)
    child_env.update(env)
    return subprocess.run(
        [sys.executable, str(PROGRAMS / (name + ".py"))],
        capture_output=True,
        text=True,
        env=child_env,
        timeout=timeout,
    )


def journal(proc):
    lines = [line for line in proc.stdout.splitlines() if line.startswith("JOURNAL ")]
    if not lines:
        raise AssertionError(
            "child wrote no JOURNAL line\nstdout:\n%s\nstderr:\n%s"
            % (proc.stdout[-2000:], proc.stderr[-2000:])
        )
    return json.loads(lines[-1][len("JOURNAL ") :])


@unittest.skipUnless(
    sys.version_info[:3] == (3, 11, 6),
    "the CPython 3.11 evaluator is pinned to 3.11.6",
)
class Execute311Test(unittest.TestCase):
    def run_ok(self, name, **kwargs):
        proc = run_program(name, **kwargs)
        self.assertEqual(proc.returncode, 0, proc.stderr[-2000:])
        return journal(proc)

    # -- the state machine ------------------------------------------------

    def test_mode_matrix_answers_one_hot_function(self):
        expected = {
            # mode: (enabled, results, entries, creations>0, shadow>0, cinderjit)
            "off": (False, [], 0, False, False, False),
            "observe": (True, ["CINDERX311_JIT_EXEC_DISABLED"], 0, False, False, False),
            "shadow": (True, ["compiled"], 0, False, True, False),
            "execute": (True, ["installed"], 10, True, False, True),
        }
        for mode, (enabled, results, entries, made, shadow, has_cinderjit) in expected.items():
            with self.subTest(mode=mode):
                payload = self.run_ok("mode_matrix", mode=mode)
                self.assertTrue(payload["values_ok"])
                self.assertEqual(payload["enabled"], enabled)
                self.assertEqual(payload["mode"], mode)
                self.assertEqual(payload["results"], results)
                self.assertEqual(payload["entries"], entries)
                self.assertEqual(payload["creations"] > 0, made)
                self.assertEqual(payload["allocs"] > 0, made)
                self.assertEqual(payload["shadow"] > 0, shadow)
                self.assertEqual(payload["cinderjit"], has_cinderjit)
                self.assertEqual(payload["compiled"], mode == "execute")

    def test_bare_threshold_without_the_mode_executes_nothing(self):
        # The product JIT is opt-in by mode: a threshold alone is the
        # interpreter-only default, with no cinderjit module.
        payload = self.run_ok("bare_threshold", mode=None)
        self.assertFalse(payload["stats"]["enabled"])
        self.assertEqual(payload["stats"]["mode"], "off")
        self.assertEqual(payload["stats"]["events"], [])
        self.assertFalse(payload["cinderjit"])
        self.assertTrue(all(v == 0 for v in payload["trigger"].values()))

    # -- one attempt per code object ----------------------------------------

    def test_one_scheduling_attempt_per_code_no_compilation_storm(self):
        # A function re-created per call is compiled once; later instances
        # attach within the budget and the rest run interpreted.
        payload = self.run_ok("one_attempt_storm")
        self.assertEqual(len(payload["events"]), 1)
        self.assertEqual(payload["events"][0]["result"], "installed")
        # The closure's code and the (hot) factory itself: two
        # compilations in two hundred instances.
        self.assertEqual(payload["creations"], 2)
        self.assertEqual(payload["attachments"], 8)
        # The compiled instance, the eight attached ones, then interpreted.
        self.assertEqual(payload["compiled_flags"][:9], [True] * 9)
        self.assertEqual(payload["compiled_flags"][9:], [False] * 191)
        # Only the factory is still compiled; every instance is gone.
        self.assertEqual(payload["live"], 1)

    def test_failed_automatic_attempt_disables_the_code_object(self):
        # A refused attempt is a permanent verdict on the code object;
        # force_compile() stays available.
        payload = self.run_ok("failed_attempt_disables")
        self.assertEqual(len(payload["after_first"]), 1)
        self.assertEqual(
            payload["after_first"][0]["result"], "REFUSE_SHAPE_JIT_SUPPRESSED"
        )
        self.assertEqual(payload["after_second"], payload["after_first"])
        self.assertEqual(payload["attachments"], 0)
        self.assertEqual(payload["disabled"], 1)
        self.assertEqual(payload["disabled_second"], 0)
        self.assertFalse(payload["first_compiled"])
        self.assertTrue(payload["forced"])
        self.assertEqual(payload["forced_entered"], 1)

    def test_generator_auto_default_is_off_but_explicit_compile_works(self):
        payload = self.run_ok("generator_auto_off")
        self.assertEqual(len(payload["events"]), 1)
        self.assertEqual(
            payload["events"][0]["result"], "REFUSE_SHAPE_GENERATOR_AUTO_DISABLED"
        )
        self.assertEqual(payload["disabled"], 1)
        self.assertTrue(payload["forced"])
        self.assertGreater(payload["entered"], 0)

    # -- fresh function objects ----------------------------------------------

    def test_fresh_instances_attach_within_the_budget(self):
        for budget in (8, 3, 0):
            with self.subTest(budget=budget):
                env = {}
                if budget != 8:
                    env["PYTHONJITFRESHATTACHBUDGET"] = str(budget)
                payload = self.run_ok("fresh_attach_budget", **env)
                self.assertTrue(payload["compiled_first"])
                self.assertEqual(payload["creations"], 1)
                self.assertEqual(payload["attachments"], budget)
                self.assertEqual(payload["later"][:budget], [True] * budget)
                self.assertEqual(payload["later"][budget:], [False] * (13 - budget))
                self.assertEqual(payload["entered"], 1 + budget)
                self.assertEqual(payload["same_artifact"], 1)

    def test_attachment_survives_the_death_of_the_compiled_instance(self):
        # The artifact is anchored by the outer function: the compiled
        # instance can die, a later instance still attaches, and dropping
        # the outer releases the machine code.
        payload = self.run_ok("attach_survives_death")
        self.assertTrue(payload["anchored"])
        self.assertEqual(payload["resident_after_death"], 1)
        self.assertTrue(payload["second_compiled"])
        self.assertTrue(payload["same"])
        self.assertEqual(payload["resident_after_second"], 1)
        self.assertEqual(payload["creations"], 1)
        self.assertEqual(payload["attachments"], 1)
        # Dropping the outer function released the machine code.
        self.assertEqual(payload["resident"], 0)

    def test_outer_found_through_the_caller_chain(self):
        # A nested function bound nowhere -- the outer is itself local --
        # is anchored on the outermost containing caller.
        payload = self.run_ok("outer_caller_chain")
        self.assertEqual(payload["flags"], [True] * 6)
        self.assertTrue(payload["anchored"])
        self.assertEqual(payload["creations"], 1)
        self.assertEqual(payload["attachments"], 5)

    def test_namespace_twin_never_attaches(self):
        payload = self.run_ok("namespace_twin")
        self.assertFalse(payload["twin_compiled"])
        self.assertIn("CANNOT_SPECIALIZE", payload["refused"])
        self.assertEqual(payload["attachments"], 0)
        self.assertEqual(payload["entered"], 0)
        self.assertEqual(payload["creations"], 1)

    def test_force_compile_attaches_beyond_the_budget(self):
        # Explicit compilation of a fresh function is never budgeted, and
        # it attaches rather than compiling again.
        payload = self.run_ok("force_beyond_budget", PYTHONJITFRESHATTACHBUDGET="0")
        self.assertFalse(payload["auto_attached"])
        self.assertTrue(payload["forced"])
        self.assertTrue(payload["compiled"])
        self.assertEqual(payload["entered"], 1)
        self.assertEqual(payload["creations"], 1)
        self.assertEqual(payload["attachments"], 0)

    def test_paused_jit_and_tracing_defer_attachment(self):
        payload = self.run_ok("paused_tracing_attach")
        self.assertFalse(payload["paused_attached"])
        self.assertTrue(payload["resumed_attached"])
        self.assertFalse(payload["traced_attached"])
        self.assertTrue(payload["after_trace_attached"])
        self.assertEqual(payload["attachments"], 2)
        self.assertEqual(payload["creations"], 1)

    def test_a_foreign_twin_compiles_once_the_owner_retires(self):
        # The twin refusal lasts exactly as long as the artifact has an
        # owner: retiring the owner makes the same twin compilable.
        payload = self.run_ok("foreign_twin_owner_retires")
        self.assertIn("CANNOT_SPECIALIZE", payload["refused"])
        self.assertTrue(payload["forced"])
        self.assertTrue(payload["compiled"])
        self.assertEqual(payload["entered"], 1)

    def test_uncompiling_one_member_releases_the_shared_code_buffer(self):
        # Uncompiling one member retires the artifact for every member,
        # anchors included; a leftover anchor would hold the code buffer
        # resident with nothing able to release it.
        payload = self.run_ok("uncompile_member_releases")
        # One artifact, three owning members.
        self.assertEqual(payload["creations"], 1)
        self.assertEqual(payload["anchored_before"], [True] * 3)
        self.assertEqual(payload["resident_compiled"], 1)
        # Retirement reaches every member, anchors included.
        self.assertEqual(payload["compiled_after"], [False] * 3)
        self.assertEqual(payload["anchored_after"], [False] * 3)
        self.assertEqual(payload["resident_after"], 0)
        # ...and the members still compute the same answers interpreted.
        self.assertTrue(payload["values_ok"])

    def test_a_twin_refusal_spends_the_dispatch_but_not_the_code(self):
        # The twin spends the dispatch (nothing reschedules it), but the
        # refusal is about the function, not the code -- so no verdict is
        # recorded against the code object.
        payload = self.run_ok("twin_refusal_spends_dispatch")
        self.assertEqual(
            payload["refused"], ["REFUSE_SHAPE_CODE_ARTIFACT_ALREADY_PUBLISHED"]
        )
        # Exactly one attempt, ever.
        self.assertEqual(payload["after"], payload["refused"])
        self.assertEqual(payload["disabled"], 0)
        self.assertFalse(payload["compiled"])
        self.assertEqual(payload["entered"], 0)

    def test_transient_conditions_never_reach_the_attempt(self):
        # Tracing and a paused JIT say nothing about the code object, so
        # neither may spend its one attempt: counting continues, nothing is
        # recorded, and the first clean frame past the threshold installs.
        for case in ("trace", "pause"):
            with self.subTest(case=case):
                payload = self.run_ok("transient_conditions", TRANSIENT_CASE=case)
                self.assertEqual(payload["held"], [])
                self.assertEqual(payload["disabled_during"], 0)
                self.assertEqual(payload["after_results"], ["installed"])
                # Counting continued while the condition held: the crossing
                # is recorded at the count the dispatch happened at.
                self.assertEqual(payload["after_count"], 3 * THRESHOLD + 1)
                self.assertTrue(payload["compiled"])
                self.assertEqual(payload["entered"], 1)

    def test_staticmethod_and_classmethod_factories_anchor_their_closures(self):
        # staticmethod/classmethod factories are wrappers, not functions;
        # without unwrapping, the closure's artifact would have no outer to
        # live on and would die with the first instance.
        for kind in ("staticmethod", "classmethod"):
            with self.subTest(kind=kind):
                payload = self.run_ok("wrapper_factories", FACTORY_KIND=kind)
                self.assertTrue(payload["first_compiled"])
                # The artifact outlived the instance that was compiled.
                self.assertEqual(payload["resident_after_death"], 1)
                self.assertTrue(payload["second_compiled"])
                self.assertEqual(payload["creations"], 1)
                self.assertTrue(payload["values_ok"])

    def test_threshold_rejects_negative_and_overflowing_values(self):
        # strtoull() accepts a sign: an unchecked "-1" becomes a threshold
        # no program reaches.  Bad values must be refused outright.
        for value in ("-1", "99999999999999999999999999", "1.5", " 5"):
            with self.subTest(value=value):
                proc = run_program("init_only", threshold=value, timeout=60)
                self.assertNotEqual(proc.returncode, 0, value)
                self.assertIn("non-negative integer", proc.stderr)

    def test_attach_budget_above_the_counter_width_is_clamped(self):
        # The 16-bit attach counter saturates; an unreachable budget would
        # be no cap at all, so the configuration is clamped.
        payload = self.run_ok(
            "budget_clamp", threshold=1, PYTHONJITFRESHATTACHBUDGET="70000"
        )
        # Clamped to the counter width, so attachment still terminates.
        self.assertLessEqual(payload["attachments"], 0xFFFF)
        self.assertTrue(any(payload["kept"]))

    # -- CALL specialization and the evaluator ------------------------------

    def test_interpreter_call_keeps_legal_specialization_and_enters_callee(self):
        # The interpreted caller keeps stock CALL specialization, and every
        # call of the compiled callee enters machine code through its
        # vectorcall entry.
        payload = self.run_ok("call_specialization", threshold=1000000)
        self.assertEqual(payload["value"], sum(i * 3 + 3 for i in range(200)))
        self.assertEqual(payload["entered"], 200)
        self.assertFalse(payload["caller_compiled"])
        self.assertIn("PRECALL_NO_KW_LEN", payload["ops"])
        self.assertIn("CALL_PY_EXACT_ARGS", payload["ops"])

    def test_third_party_evaluator_degrades_the_jit_safely(self):
        payload = self.run_ok("third_party_evaluator")
        self.assertEqual(payload["entered_ours"], 1)
        self.assertFalse(payload["installed_foreign"])
        self.assertFalse(payload["compiled_foreign"])
        self.assertTrue(payload["values_ok"])
        self.assertEqual(payload["entered_foreign"], 0)
        # No frame of ours ran, so nothing was counted or scheduled.
        self.assertEqual(payload["foreign_events"], 0)
        self.assertIn("another component replaced", payload["removal"])
        self.assertTrue(payload["installed_again"])
        self.assertTrue(payload["compiled_again"])
        self.assertEqual(payload["entered_again"], 1)

    # -- thresholds and shutdown ---------------------------------------------

    def test_threshold_matrix(self):
        # (PYTHONJITAUTO, effective threshold); None = the default of 50,
        # and the very high value is the armed-but-interpreted control arm.
        cases = [
            ("0", 0),
            ("1", 1),
            ("2", 2),
            ("4", 4),
            (None, 50),
            ("1000000000", None),
        ]
        for raw, effective in cases:
            with self.subTest(threshold=raw):
                payload = self.run_ok("threshold_matrix", threshold=raw)
                self.assertTrue(payload["values_ok"])
                self.assertTrue(payload["installed"])
                self.assertEqual(payload["mode"], "execute")
                if effective is None:
                    # Armed but never compiling: the interpreted control arm.
                    self.assertEqual(payload["events"], [])
                    self.assertEqual(payload["entries"], 0)
                    self.assertFalse(payload["compiled"])
                    continue
                self.assertEqual(payload["threshold"], effective)
                self.assertEqual(len(payload["events"]), 1)
                # Threshold zero schedules on the first observable call;
                # there is no call-count event before that frame exists.
                self.assertEqual(payload["events"][0]["count"], max(effective, 1))
                self.assertEqual(payload["events"][0]["result"], "installed")
                # hot's own calls after the crossing; the driving
                # comprehension itself never enters machine code.
                # The threshold-crossing frame is already executing in the
                # interpreter; only later calls enter the published artifact.
                self.assertEqual(payload["entries"], 200 - max(effective, 1))
                self.assertTrue(payload["compiled"])

    def test_zero_threshold_startup_sources_are_consistent(self):
        cases = (
            {"threshold": "0"},
            {"threshold": None, "PYTHONJITALL": "1"},
        )
        for env in cases:
            with self.subTest(env=env):
                payload = self.run_ok("threshold_matrix", **env)
                self.assertEqual(payload["threshold"], 0)
                self.assertEqual(len(payload["events"]), 1)
                self.assertEqual(payload["events"][0]["count"], 1)
                self.assertEqual(payload["events"][0]["result"], "installed")
                self.assertEqual(payload["entries"], 199)
                self.assertTrue(payload["compiled"])

    def test_shutdown_repeats_clean_with_live_state(self):
        # Compiled functions, attached fresh instances, a suspended
        # compiled generator and a parked function are all left alive at
        # exit.
        for repetition in range(10):
            with self.subTest(repetition=repetition):
                proc = run_program("shutdown_live_state")
                self.assertEqual(proc.returncode, 0, proc.stderr[-2000:])
                self.assertEqual(proc.stderr, "")
                payload = journal(proc)
                # Six closure instances and the generator.
                self.assertEqual(payload["live"], 7)
                self.assertEqual(payload["attachments"], 5)

    # -- observation stays invisible ----------------------------------------

    def test_observation_leaves_user_weak_references_as_stock_leaves_them(self):
        # Observation must be invisible to gc.get_objects() and to
        # weakref.getweakrefs() -- test_descr counts both.  The JIT-off arm
        # is the oracle, and the two must agree.
        off = self.run_ok("weakref_parity", mode="off")
        execute = self.run_ok("weakref_parity", mode="execute")
        # A user weak reference is an ordinary tracked object either way.
        self.assertTrue(off["tracked"])
        self.assertTrue(off["censused"])
        self.assertTrue(off["cached"])
        # Stock puts exactly the one the test made on the list.
        self.assertEqual(off["listed"], 1)
        for key in ("listed", "tracked", "censused", "cached"):
            self.assertEqual(execute[key], off[key], key)
        # The only code weakref either arm can see is the user's own.
        self.assertEqual(off["code_refs"], 1)
        self.assertEqual(execute["code_refs"], off["code_refs"])
        # The observer was watching -- otherwise the assertions above
        # would pass vacuously.
        self.assertGreater(execute["observed"], 1)
        self.assertEqual(off["observed"], 0)

    def test_watching_a_code_object_ends_when_the_code_object_dies(self):
        # Entries are retired by the death notice; a missed notice would
        # key a used-up attempt to an address a later code object can be
        # allocated at.
        payload = self.run_ok("code_death_retires_watch")
        watched = [row[0] for row in payload["rounds"]]
        capacity = [row[1] for row in payload["rounds"]]
        seen = [row[2] for row in payload["rounds"]]
        # 800 more code objects were observed across the four rounds ...
        self.assertGreater(seen[-1] - payload["base"][2], 700)
        # ... and not one of them is still being watched: every entry was
        # retired by its death notice.
        self.assertEqual(watched, [payload["base"][0]] * len(watched))
        # The table therefore never had to grow to hold them.
        self.assertEqual(capacity, [payload["base"][1]] * len(capacity))

    def test_the_observer_table_does_not_ratchet_across_churn_cycles(self):
        # Dead slots keep their keys (tombstones), so churn trips the load
        # factor with little alive; growth must compact, not double, or the
        # table ratchets one octave per churn cycle.
        payload = self.run_ok("table_no_ratchet")
        rounds = payload["rounds"]
        peak_watched = [r[0] for r in rounds]
        capacity = [r[1] for r in rounds]
        resting = [r[2] for r in rounds]
        # The workload is identical every round: same live population at
        # the peak, same population left behind.
        self.assertEqual(peak_watched, [peak_watched[0]] * len(peak_watched))
        self.assertEqual(resting, [resting[0]] * len(resting))
        self.assertGreater(peak_watched[0], 600)
        # 4200 code objects were observed across the six rounds ...
        self.assertGreater(rounds[-1][4], 4000)
        # ... and the table never grew past what one round's live
        # population actually needs.
        self.assertEqual(capacity, [capacity[0]] * len(capacity))

    def test_a_foreign_code_extra_slot_below_ours_refuses_the_mode(self):
        # code_dealloc walks co_extra slots 0..ce_size, foreign slots
        # included; a dead foreign freefunc below ours turns teardown into a
        # jump through a freed trampoline.  Registration order is a
        # load-order accident, so the mode refuses unless we hold slot 0.
        for mode in ("observe", "shadow", "execute"):
            with self.subTest(mode=mode):
                proc = run_program("foreign_slot_refuses", mode=mode)
                self.assertEqual(proc.returncode, 0, proc.stderr[-2000:])
                payload = journal(proc)
                # The foreign component really did land below us.
                self.assertEqual(payload["foreign"], 0)
                # And the mode refused rather than running.
                self.assertIsNotNone(payload["refused"])
                self.assertIn("code-extra slot", payload["refused"])
                # Nothing was watched, so nothing forced the walk to reach
                # the foreign slot.
                self.assertEqual(payload["foreign_calls"], 0)

    def test_a_suspended_frame_does_not_schedule_its_functions_new_code(self):
        # A suspended generator resumes the frame it already has: the OLD
        # code runs while the function holds a new one.  Counts and the
        # single attempt belong to the code that actually runs.
        payload = self.run_ok("suspended_frame_no_schedule")
        # No resume of the old frame produced an event for either code
        # object the function was pointed at.
        self.assertEqual(payload["installable"], [])
        self.assertEqual(payload["refusable"], [])
        # In particular the compilable one was not compiled from work it
        # never did.
        self.assertFalse(payload["compiled_from_resumes"])
        # And once it really runs, it is scheduled normally -- the attempt
        # was still there, and the code reached it on its own frames.
        self.assertEqual(payload["events_after"], ["installed"])
        self.assertTrue(payload["compiled_after_real_calls"])
        self.assertGreater(payload["entered"], 0)

    def test_reporting_the_event_ledger_excludes_the_observer(self):
        # Building the report allocates, an allocation can run a finalizer,
        # and a finalizer can append to the very array the report is
        # walking -- realloc() then frees the element the walk holds.
        payload = self.run_ok("ledger_reentrancy", mode="observe", threshold=2)
        # The ledger was full, so an append during the report would have
        # moved it.
        self.assertEqual(payload["before"], 1024)
        # Every record read back is intact.
        self.assertEqual(payload["malformed"], 0)
        # And the report is a consistent snapshot: the finalizer's own
        # frames were not counted, exactly as frames entered while the
        # observer is bookkeeping never are.
        self.assertEqual(payload["reported"], 1024)
        self.assertFalse(payload["bomb_ran"])

    def test_the_event_ledger_does_not_hold_the_observed_program_s_strings(self):
        # The ledger keeps copies, not references: holding co_filename or
        # co_qualname would be visible in sys.getrefcount() from the
        # observed program.
        off = self.run_ok("ledger_string_copies", mode="off")
        for mode in ("observe", "shadow", "execute"):
            with self.subTest(mode=mode):
                watched = self.run_ok("ledger_string_copies", mode=mode)
                # Observation is invisible in the refcounts of the program
                # being observed.
                self.assertEqual(watched["delta"], off["delta"])
                self.assertEqual(watched["delta"], [0, 0])
        # And the ledger really did record the names.
        recorded = self.run_ok("ledger_string_copies", mode="execute")["recorded"]
        self.assertEqual(recorded, [["probe_fn", "<ledger-probe>"]])

    # -- import/setup suppression -------------------------------------------

    def test_scheduling_is_suppressed_inside_import_and_setup_scopes(self):
        # Inside import/setup scopes nothing dispatches and nothing is
        # spent; counting continues and the first frame after the scope
        # closes dispatches.
        for scope in ("import", "setup"):
            with self.subTest(scope=scope):
                payload = self.run_ok("suppress_scopes", SUPPRESS_SCOPE=scope)
                # Nothing was scheduled and nothing was spent inside.
                self.assertEqual(payload["inside_events"], [])
                self.assertFalse(payload["inside_compiled"])
                self.assertEqual(payload["spent"], 0)
                # The attempt was still there when the scope closed.
                self.assertEqual(payload["after_events"], ["installed"])
                self.assertTrue(payload["compiled"])
                self.assertEqual(payload["entered"], 1)

    def test_suppression_covers_fresh_attachment_not_only_the_first_dispatch(self):
        # Fresh attachment is its own scheduling door, reached before the
        # threshold path; suppression has to cover it too or an instance
        # created inside a scope attaches and spends the budget there.
        for scope in ("import", "setup"):
            with self.subTest(scope=scope):
                payload = self.run_ok("suppress_covers_attach", SUPPRESS_SCOPE=scope)
                # Nothing attached, ran or was charged inside the scope.
                self.assertFalse(payload["inside_attached"])
                self.assertEqual(payload["inside_ran"], 0)
                self.assertEqual(payload["inside_charged"], 0)
                # And the attachment was withheld, not lost.
                self.assertTrue(payload["after_attached"])
                self.assertEqual(payload["after_ran"], 10)

    def test_a_pre_existing_artifact_still_obeys_the_attachment_budget(self):
        # A later instance over an artifact the scheduler never dispatched
        # reaches the compile entry point, not the frame-entry attachment;
        # that door must consult the same per-code budget.
        zero = self.run_ok(
            "preexisting_artifact_budget", PYTHONJITFRESHATTACHBUDGET="0"
        )
        # A budget of zero turns automatic attachment off, by every door.
        self.assertFalse(zero["attached"])
        self.assertEqual(zero["ran"], 0)
        # A budget that allows one still allows exactly this one.
        one = self.run_ok(
            "preexisting_artifact_budget", PYTHONJITFRESHATTACHBUDGET="1"
        )
        self.assertTrue(one["attached"])
        self.assertEqual(one["ran"], 10)

    def test_a_namespace_twin_does_not_disable_the_code_for_its_own_namespace(self):
        # The twin's refusal is about the function (foreign globals), not
        # the code; recording it as the code's verdict would lock out the
        # instances that DO share the artifact's namespace.
        payload = self.run_ok("twin_does_not_disable")
        # The twin was refused, and for the reason that is about it.
        self.assertEqual(
            payload["twin_verdicts"],
            ["REFUSE_SHAPE_CODE_ARTIFACT_ALREADY_PUBLISHED"],
        )
        self.assertFalse(payload["twin_compiled"])
        # And the refusal cost the code object nothing: an instance in the
        # artifact's own namespace still attaches and runs machine code.
        self.assertTrue(payload["sibling_attached"])
        self.assertEqual(payload["sibling_ran"], 10)

    def test_the_product_configuration_installs_the_import_provider(self):
        # Something has to raise the import depth the scheduler reads, and
        # on 3.11 that must not be keyed off the 3.12 auto[:N] classifier.
        # Driven end to end: a real import raises the depth.
        execute = self.run_ok("import_provider", mode="execute")
        # The product configuration asks for both providers ...
        self.assertEqual(execute["import_provider"], "find_and_load")
        self.assertTrue(execute["setup_provider"])
        self.assertNotEqual(execute["setup_provider"], "off")
        # ... and a real import actually raises the depth the scheduler
        # reads, then puts it back.
        self.assertGreater(execute["depth_during_import"], 0)
        self.assertEqual(execute["depth_after"], 0)
        # With no scheduling mode configured, nothing is installed and no
        # import pays for a wrapper it does not need.
        off = self.run_ok("import_provider", mode="off")
        self.assertEqual(off["import_provider"], "off")
        self.assertEqual(off["setup_provider"], "off")
        self.assertEqual(off["depth_during_import"], 0)

    # -- publication racing a control-plane decision ----------------------

    def test_a_code_move_inside_the_attempt_withholds_it_and_never_burns_it(self):
        # The attempt's subject is fixed when it begins; the attempt runs
        # arbitrary Python, so __code__ can move at any allocation inside
        # it.  A moved function must neither be compiled on a threshold the
        # new code did not earn nor have the move recorded as a verdict.
        payload = self.run_ok("sweep_code_move")
        # A move really did land inside the attempt.
        self.assertTrue(payload["moved"])
        # No never-executed code object was compiled ...
        self.assertEqual(payload["ran"], [])
        # ... and no code object lost its attempt to one.
        self.assertEqual(payload["burned"], [])

    def test_disable_across_the_dispatch_never_spends_the_attempt(self):
        # A disable() landing inside the attempt describes the process,
        # not the code object; no landing point may spend the attempt.
        payload = self.run_ok("sweep_disable_dispatch")
        # No landing point cost a code object its attempt.
        self.assertEqual(payload["lost"], [])
        # And landing points inside the attempt exist and were reached --
        # without this the assertion above would pass vacuously.
        self.assertTrue(payload["deferred"])

    def test_disable_across_a_nested_dispatch_never_spends_the_attempt(self):
        # The same sweep over the nested shape: arming the outer's death
        # watch allocates well before the compile entry point is reached.
        payload = self.run_ok("sweep_disable_nested")
        self.assertEqual(payload["lost"], [])
        self.assertTrue(payload["deferred"])

    def test_disable_across_the_attach_still_charges_the_budget(self):
        # A publication a disable() lands inside still associates the
        # member, so it must still charge the budget.
        payload = self.run_ok(
            "sweep_disable_attach_budget", PYTHONJITFRESHATTACHBUDGET="1"
        )
        # The budget was never exceeded ...
        self.assertEqual(payload["over"], [])
        # ... and a disable() really did land inside a publication, so the
        # assertion above is about the window it is named for.
        self.assertTrue(payload["landed"])

    def test_disable_across_the_attach_keeps_machine_code_shut(self):
        # A disable() can land between the registry sweep and the new
        # entry point going in; entering machine code behind it is the
        # failure.  The entry predicate answers on the state NOW.
        payload = self.run_ok("sweep_disable_attach_entry")
        # The disable really landed inside some publication, and fresh
        # attachment really ran -- neither assertion below is vacuous.
        self.assertTrue(payload["landed"])
        self.assertGreaterEqual(payload["attached"], 1)
        # No frame entered machine code while the JIT was unusable.
        self.assertEqual(payload["entered"], [])
        # And attachment still works once it is usable again.
        self.assertGreater(payload["resumed"], 0)


if __name__ == "__main__":
    unittest.main()
