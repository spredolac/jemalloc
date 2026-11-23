#!/usr/bin/env bash
# Tests only: run from the workflow checkout with a detached ci-build worktree.
set -euo pipefail

root=$PWD
evidence=$root/evidence
mkdir -p "$evidence"
cd ci-build
umask 022

# MSYS2 normally prepends /usr/bin (which has an unrelated link.exe). Put
# the Microsoft tools before it without changing the paths to POSIX tools.
if [[ $CC != gcc ]]; then
  export AR=lib.exe NM=dumpbin.exe RANLIB=:
  : "${VCToolsInstallDir:?Visual C++ developer environment is missing}"
  if [[ $EXPECT_ARCH == x86 ]]; then
    msvc_path=$(cygpath -u "${VCToolsInstallDir}bin\\Hostx64\\x86")
    if [[ ! -x $msvc_path/cl.exe ]]; then
      msvc_path=$(cygpath -u "${VCToolsInstallDir}bin\\Hostx86\\x86")
    fi
  else
    msvc_path=$(cygpath -u "${VCToolsInstallDir}bin\\Hostx64\\x64")
  fi
  test -x "$msvc_path/link.exe"
  export PATH="$msvc_path:$PATH"
else
  export EXTRA_CFLAGS=-fcommon
fi

run_logged() {
  local name=$1
  shift
  printf '\n--- %s ---\n' "$name"
  "$@" 2>&1 | tee "$evidence/$name.log"
}

record_rc() {
  printf '%s=%s\n' "$1" "$2" | tee -a "$evidence/status.env"
}

run_focused() {
  local rc=0 arch_report addrsize
  unset JE_MALLOC_CONF MALLOC_CONF MALLOC_CONF_TEST
  if [[ -n ${EXPECT_LG_PAGE:-} ]]; then
    export JE_MALLOC_CONF="lg_page:$EXPECT_LG_PAGE"
  fi
  run_logged focused bash test/test.sh test/unit/base || rc=$?
  record_rc focused_rc "$rc"
  arch_report=$(objdump -f test/unit/base.exe)
  printf '%s\n' "$arch_report" | tee "$evidence/executable-architecture.log"
  if [[ $EXPECT_ARCH == x64 ]]; then addrsize=pei-x86-64; else addrsize=pei-i386; fi
  if ! grep -Fq "$addrsize" <<<"$arch_report"; then
    echo '::error::Built executable has an unexpected architecture'
    return 1
  fi
  # A passing harness that skipped every case isn't proof of a regression fix.
  if [[ $SIDE == fixed ]]; then
    if (( rc != 0 )) || ! grep -Fq 'pass: 1/1, skip: 0/1, fail: 0/1' "$evidence/focused.log" ||
      ! grep -Eq '^test_base_block_size \(non-reentrant\): pass' "$evidence/focused.log"; then
      echo '::error::Fixed focused base suite or targeted regression did not pass'
      return 1
    fi
  else
    if (( rc == 0 )) || ! grep -Fq 'pass: 0/1, skip: 0/1, fail: 1/1' "$evidence/focused.log" ||
      ! grep -Eq '^test_base_block_size \(non-reentrant\): fail' "$evidence/focused.log" ||
      ! grep -Fq 'Initial base block should not reserve unnecessary metadata space' "$evidence/focused.log"; then
      echo '::error::Baseline did not fail the new initial-block assertion as expected'
      return 1
    fi
  fi
}

run_repro() {
  cp "$root/ci-tools/issue-3068-repro.c" arena-repro.c
  # Build just the static library: jemalloc_s.lib is distinct from the DLL
  # import library even when both were built by the full-suite job later.
  if ! run_logged repro-build mingw32-make -j3 lib/jemalloc_s.lib; then
    return 1
  fi
  if [[ $CC == gcc ]]; then
    if ! run_logged repro-link gcc -static -pthread -DJEMALLOC_EXPORT= \
      -Iinclude arena-repro.c lib/jemalloc_s.lib -lpsapi -lstdc++ -o arena-repro.exe; then
      return 1
    fi
  else
    if ! run_logged repro-link "$CC" -nologo -MT -DJEMALLOC_EXPORT= \
      -Iinclude arena-repro.c lib/jemalloc_s.lib psapi.lib -Fearena-repro.exe; then
      return 1
    fi
  fi
  local errors=0 rc=0
  run_logged repro-arenas ./arena-repro.exe arenas 64 || { rc=$?; errors=1; }
  record_rc repro_arenas_rc "$rc"
  rc=0
  run_logged repro-threads ./arena-repro.exe threads || { rc=$?; errors=1; }
  record_rc repro_threads_rc "$rc"
  rc=0
  run_logged repro-percpu ./arena-repro.exe threads 64 'percpu_arena:percpu,tcache:false' || { rc=$?; errors=1; }
  record_rc repro_percpu_rc "$rc"
  return "$errors"
}

case ${1:?expected focused or suite} in
  focused)
    {
      command -v "$CC"
      if [[ $CC == cl.exe ]]; then
        # cl with no inputs reports its version and returns nonzero.
        "$CC" 2>&1 || :
        command -v link.exe
      else
        "$CC" --version
        if [[ $CC == clang-cl.exe ]]; then command -v link.exe; fi
      fi
    } | tee -a "$evidence/environment.log"
    run_logged autoconf autoconf
    read -r -a flags <<<"${CONFIGURE_FLAGS:-}"
    run_logged configure ./configure "${flags[@]}"
    printf 'configured=true\n' >> "$GITHUB_OUTPUT"
    cp config.log "$evidence/config.log"
    run_logged focused-build mingw32-make -j3 test/unit/base.exe
    focused_rc=0
    run_focused || focused_rc=$?
    repro_rc=0
    if [[ $RUN_REPRO == true ]]; then
      unset JE_MALLOC_CONF MALLOC_CONF MALLOC_CONF_TEST
      run_repro || repro_rc=$?
    fi
    if (( focused_rc != 0 || repro_rc != 0 )); then exit 1; fi
    ;;
  suite)
    if [[ $SIDE == baseline ]]; then
      git restore -- test/unit/base.c
    fi
    unset JE_MALLOC_CONF MALLOC_CONF MALLOC_CONF_TEST
    run_logged suite-build mingw32-make -j3
    # Run test link steps sequentially to avoid MSVC PDB contention.
    run_logged suite-tests-build mingw32-make tests
    suite_rc=0
    run_logged suite mingw32-make -k check || suite_rc=$?
    record_rc suite_rc "$suite_rc"
    # Do not suppress a red full suite; the evidence distinguishes pre-existing
    # failures on baseline from any divergence on the fixed build.
    exit "$suite_rc"
    ;;
  *) exit 2 ;;
esac
