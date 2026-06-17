# SPDX-License-Identifier: GPL-2.0
# Triade kselftest harness.
#
# In-tree (upstream) layout: drop this directory under
# tools/testing/selftests/net/triade/ and the toplevel kselftest Makefile
# picks it up. Out-of-tree: run `make -C tools/testing/selftests/net/triade
# run_tests` directly from the repo root.

TEST_PROGS := \
	triade_smoke.sh \
	triade_ring_3node.sh \
	triade_open_close.sh \
	triade_state_machine.sh \
	triade_dedup.sh \
	triade_sched_modes.sh \
	triade_mtu_coherence.sh

TEST_FILES := lib.sh

# Standalone runner so the tests work without the kernel-tree kselftest
# infrastructure. Each script is expected to print TAP-style results and
# exit 0 on success / 1 on failure.
run_tests:
	@total=0; pass=0; fail=0; \
	for t in $(TEST_PROGS); do \
		total=$$((total+1)); \
		printf '\n=== %s ===\n' $$t; \
		if ./$$t; then pass=$$((pass+1)); \
		else fail=$$((fail+1)); fi; \
	done; \
	printf '\n--- summary: %d/%d passed (%d failed) ---\n' $$pass $$total $$fail; \
	[ $$fail -eq 0 ]

clean:

.PHONY: run_tests clean
