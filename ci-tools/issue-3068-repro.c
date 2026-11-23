/*
 * Measures what each jemalloc arena costs in committed memory on Windows.
 *
 *   arena-repro.exe arenas [N]          create N manual arenas (default 64)
 *   arena-repro.exe threads [T] [conf]  start T threads (default: jemalloc's
 *                                       narenas) that allocate once each, which
 *                                       binds them to automatic arenas. conf is
 *                                       the jemalloc config, "tcache:false" by
 *                                       default so that thread caches stay out
 *                                       of the measure.
 *
 * Built against jemalloc 5.4.0 with its default "je_" prefix, so the process's
 * own malloc stays the CRT's and only the je_ calls below touch jemalloc.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jemalloc/jemalloc.h>

#define MIB (1024.0 * 1024.0)

typedef struct {
	double private_usage;
	double mapped;
	double metadata;
	unsigned arenas;
} snapshot_t;

static void fail(const char *what) {
	fprintf(stderr, "%s failed\n", what);
	exit(1);
}

static double private_usage(void) {
	PROCESS_MEMORY_COUNTERS_EX counters = {0};
	counters.cb = sizeof(counters);
	if (!GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&counters, sizeof(counters))) {
		fail("GetProcessMemoryInfo");
	}
	return (double)counters.PrivateUsage;
}

static void read_ctl(const char *name, void *value, size_t len) {
	size_t expected = len;
	if (je_mallctl(name, value, &len, NULL, 0) != 0 || len != expected) {
		fail(name);
	}
}

static size_t read_size(const char *name) {
	size_t value;
	read_ctl(name, &value, sizeof(value));
	return value;
}

static unsigned read_unsigned(const char *name) {
	unsigned value;
	read_ctl(name, &value, sizeof(value));
	return value;
}

static const char *read_string(const char *name) {
	const char *value;
	read_ctl(name, &value, sizeof(value));
	return value;
}

/* jemalloc only refreshes its statistics when the epoch advances. */
static void refresh_stats(void) {
	uint64_t epoch = 1;
	size_t len = sizeof(epoch);
	if (je_mallctl("epoch", &epoch, &len, &epoch, len) != 0) {
		fail("epoch");
	}
}

static unsigned initialized_arenas(void) {
	size_t mib[3];
	size_t miblen = 3;
	if (je_mallctlnametomib("arena.0.initialized", mib, &miblen) != 0) {
		fail("arena.0.initialized");
	}
	unsigned count = 0;
	unsigned narenas = read_unsigned("arenas.narenas");
	for (unsigned i = 0; i < narenas; i++) {
		bool initialized = false;
		size_t len = sizeof(initialized);
		mib[1] = i;
		if (je_mallctlbymib(mib, miblen, &initialized, &len, NULL, 0) == 0 && initialized) {
			count++;
		}
	}
	return count;
}

static snapshot_t snapshot(void) {
	refresh_stats();
	snapshot_t s;
	s.private_usage = private_usage();
	s.mapped = (double)read_size("stats.mapped");
	s.metadata = (double)read_size("stats.metadata");
	s.arenas = initialized_arenas();
	return s;
}

static void print_header(void) {
	printf("%8s  %22s  %15s  %22s  %22s\n", "arenas", "private usage (MiB)", "per arena", "jemalloc mapped (MiB)",
	    "metadata in use (MiB)");
}

static void print_row(snapshot_t before, snapshot_t after) {
	unsigned added = after.arenas - before.arenas;
	double committed = (after.private_usage - before.private_usage) / MIB;
	printf("%8u  %+22.1f  %11.2f MiB  %+22.1f  %+22.2f\n", added, committed, added ? committed / added : 0.0,
	    (after.mapped - before.mapped) / MIB, (after.metadata - before.metadata) / MIB);
}

static void print_context(const char *conf) {
	printf("jemalloc %s\n", read_string("version"));
	/* jemalloc counts processors with GetSystemInfo, which only sees the process's group. */
	SYSTEM_INFO info;
	GetSystemInfo(&info);
	printf("logical processors: %lu in %u group(s), %lu seen by jemalloc\n",
	    GetActiveProcessorCount(ALL_PROCESSOR_GROUPS), GetActiveProcessorGroupCount(), info.dwNumberOfProcessors);
	DWORD_PTR process_mask, system_mask;
	if (GetProcessAffinityMask(GetCurrentProcess(), &process_mask, &system_mask)) {
		printf("affinity: process 0x%llx, system 0x%llx\n", (unsigned long long)process_mask,
		    (unsigned long long)system_mask);
	}
	printf("config: \"%s\", opt.narenas %u, opt.percpu_arena %s, opt.metadata_thp %s\n", conf ? conf : "",
	    read_unsigned("opt.narenas"), read_string("opt.percpu_arena"), read_string("opt.metadata_thp"));
}

static int run_arenas(unsigned count) {
	print_context(NULL);
	snapshot_t before = snapshot();
	printf("\ncreating %u manual arenas (arenas.create), %u arena(s) already initialized\n", count, before.arenas);
	print_header();
	unsigned checkpoint = 1;
	for (unsigned i = 1; i <= count; i++) {
		unsigned index;
		size_t len = sizeof(index);
		if (je_mallctl("arenas.create", &index, &len, NULL, 0) != 0) {
			fail("arenas.create");
		}
		if (i == checkpoint || i == count) {
			print_row(before, snapshot());
			checkpoint *= 2;
		}
	}
	return 0;
}

static HANDLE start_event;
static HANDLE stop_event;
static volatile LONG ready;
static volatile LONG done;

/* Where each thread ran when it allocated, and the arena it got. */
static unsigned *thread_cpus;
static unsigned *thread_arenas;

static DWORD WINAPI worker(LPVOID index) {
	/*
	 * On an idle machine, Windows keeps such short-lived threads on one CPU,
	 * so spread them explicitly for percpu_arena to see every CPU.
	 */
	SYSTEM_INFO info;
	GetSystemInfo(&info);
	SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << ((size_t)index % info.dwNumberOfProcessors));
	Sleep(0);
	InterlockedIncrement(&ready);
	WaitForSingleObject(start_event, INFINITE);
	thread_cpus[(size_t)index] = (unsigned)GetCurrentProcessorNumber();
	/* The first allocation binds the thread to an arena, creating it if needed. */
	je_free(je_malloc(64));
	read_ctl("thread.arena", &thread_arenas[(size_t)index], sizeof(unsigned));
	InterlockedIncrement(&done);
	WaitForSingleObject(stop_event, INFINITE);
	return 0;
}

static void print_histogram(const char *what, const unsigned *values, unsigned count) {
	printf("%s:", what);
	for (unsigned v = 0; v < 4096; v++) {
		unsigned n = 0;
		for (unsigned i = 0; i < count; i++) {
			n += values[i] == v;
		}
		if (n) {
			printf(" %u (x%u)", v, n);
		}
	}
	printf("\n");
}

static void wait_for(volatile LONG *counter, LONG target) {
	while (InterlockedCompareExchange(counter, 0, 0) < target) {
		Sleep(1);
	}
}

static int run_threads(unsigned threads, const char *conf) {
	print_context(conf);
	if (threads == 0) {
		threads = read_unsigned("opt.narenas");
	}
	start_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
	thread_cpus = calloc(threads, sizeof(*thread_cpus));
	thread_arenas = calloc(threads, sizeof(*thread_arenas));
	if (!start_event || !stop_event || !thread_cpus || !thread_arenas) {
		fail("setup");
	}
	for (unsigned i = 0; i < threads; i++) {
		HANDLE thread = CreateThread(
		    NULL, 64 * 1024, worker, (LPVOID)(size_t)i, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
		if (!thread) {
			fail("CreateThread");
		}
		CloseHandle(thread);
	}
	/* Measure once every thread exists, so that their stacks are not counted. */
	wait_for(&ready, (LONG)threads);
	snapshot_t before = snapshot();
	printf("\n%u threads allocating once each, %u arena(s) already initialized\n", threads, before.arenas);
	SetEvent(start_event);
	wait_for(&done, (LONG)threads);
	print_header();
	print_row(before, snapshot());
	printf("\n");
	print_histogram("CPU of each thread when allocating", thread_cpus, threads);
	print_histogram("arena each thread got", thread_arenas, threads);
	SetEvent(stop_event);
	return 0;
}

static unsigned parse_count(const char *arg) {
	char *end;
	unsigned long value = strtoul(arg, &end, 10);
	if (*end != '\0' || value > 100000) {
		fprintf(stderr, "invalid count: %s\n", arg);
		exit(2);
	}
	return (unsigned)value;
}

int main(int argc, char **argv) {
	const char *mode = argc > 1 ? argv[1] : "arenas";
	if (strcmp(mode, "arenas") == 0) {
		unsigned count = argc > 2 ? parse_count(argv[2]) : 64;
		je_free(je_malloc(1));
		return run_arenas(count);
	}
	if (strcmp(mode, "threads") == 0) {
		unsigned threads = argc > 2 ? parse_count(argv[2]) : 0;
		const char *conf = argc > 3 ? argv[3] : "tcache:false";
		/* jemalloc reads its config once, on its first allocation. */
		if (_putenv_s("JE_MALLOC_CONF", conf) != 0) {
			fail("_putenv_s");
		}
		je_free(je_malloc(1));
		return run_threads(threads, conf);
	}
	fprintf(stderr, "usage: %s arenas [N] | threads [T] [jemalloc conf]\n", argv[0]);
	return 2;
}
