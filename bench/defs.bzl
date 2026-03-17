load("@rules_cc//cc:cc_binary.bzl", "cc_binary")

def _shell_quote(value):
    return "'" + value.replace("'", "'\"'\"'") + "'"

def _sleep_seconds(milliseconds):
    seconds = milliseconds // 1000
    remainder = milliseconds % 1000
    if remainder == 0:
        return str(seconds)

    padded = str(remainder)
    for _ in range(3 - len(padded)):
        padded = "0" + padded

    return "{}.{}".format(seconds, padded)

def _runfiles_prologue():
    return """set +e
f="bazel_tools/tools/bash/runfiles/runfiles.bash"
source "${RUNFILES_DIR:-/dev/null}/$f" 2>/dev/null || \
  source "$(grep -sm1 "^$f " "${RUNFILES_MANIFEST_FILE:-/dev/null}" | cut -d ' ' -f2-)" 2>/dev/null || \
  source "$0.runfiles/$f" 2>/dev/null || \
  source "$(grep -sm1 "^$f " "$0.runfiles_manifest" 2>/dev/null | cut -d ' ' -f2-)" 2>/dev/null || \
  {
    echo "ERROR: cannot find $f" >&2
    exit 1
  }
set -e
"""

def _benchmark_case_impl(ctx):
    script = ctx.actions.declare_file(ctx.label.name)
    benchmark = ctx.attr.benchmark[DefaultInfo].files_to_run.executable
    if benchmark == None:
        fail("{} is not executable".format(ctx.attr.benchmark.label))

    script_content = """#!/usr/bin/env bash
set -euo pipefail

{runfiles}

WORKSPACE_ROOT="${{BUILD_WORKSPACE_DIRECTORY:-$PWD}}"
OUT_ROOT="${{UNLOG_BENCH_OUTPUT_DIR:-$WORKSPACE_ROOT/bench-results/$(date +%Y%m%d-%H%M%S)}}"
mkdir -p "$OUT_ROOT"

current_sink_out=""

cleanup() {{
    if [[ -n "$current_sink_out" ]]; then
        rm -rf "$current_sink_out"
    fi
}}

trap cleanup EXIT

result_path="$OUT_ROOT/{name}.json"
current_sink_out="$(mktemp -d "${{TMPDIR:-/tmp}}/unlog-bench-{name}.XXXXXX")"

"$(rlocation "{workspace}/{relpath}")" \
    --bench_sink={sink} \
    --bench_output_dir="$current_sink_out" \
    --benchmark_out="$result_path" \
    --benchmark_out_format=json \
    "$@"

rm -rf "$current_sink_out"
current_sink_out=""
""".format(
        name = ctx.label.name,
        relpath = benchmark.short_path,
        runfiles = _runfiles_prologue(),
        sink = _shell_quote(ctx.attr.sink),
        workspace = ctx.workspace_name,
    )

    ctx.actions.write(script, script_content, is_executable = True)

    return DefaultInfo(
        executable = script,
        runfiles = ctx.runfiles(files = ctx.files._runfiles_lib)
            .merge(ctx.attr.benchmark[DefaultInfo].default_runfiles)
            .merge(ctx.attr._runfiles_lib[DefaultInfo].default_runfiles),
    )

def _benchmark_suite_impl(ctx):
    script = ctx.actions.declare_file(ctx.label.name)
    benchmark_entries = []
    runfiles = ctx.runfiles()

    for target in ctx.attr.benchmarks:
        executable = target[DefaultInfo].files_to_run.executable
        if executable == None:
            fail("{} is not executable".format(target.label))

        benchmark_entries.append((target.label.name, executable.short_path))
        runfiles = runfiles.merge(target[DefaultInfo].default_runfiles)

    benchmark_lines = "\n".join([
        "    {name}:{path}".format(
            name = _shell_quote(name),
            path = _shell_quote(path),
        )
        for name, path in benchmark_entries
    ])

    script_content = """#!/usr/bin/env bash
set -euo pipefail

{runfiles}

WORKSPACE_ROOT="${{BUILD_WORKSPACE_DIRECTORY:-$PWD}}"
OUT_ROOT="${{UNLOG_BENCH_OUTPUT_DIR:-$WORKSPACE_ROOT/bench-results/$(date +%Y%m%d-%H%M%S)}}"
mkdir -p "$OUT_ROOT"

current_sink_out=""

cleanup() {{
    if [[ -n "$current_sink_out" ]]; then
        rm -rf "$current_sink_out"
    fi
}}

trap cleanup EXIT

benchmarks=(
{benchmark_lines}
)

for entry in "${{benchmarks[@]}}"; do
    name="${{entry%%:*}}"
    relpath="${{entry#*:}}"
    result_path="$OUT_ROOT/$name.json"
    current_sink_out="$(mktemp -d "${{TMPDIR:-/tmp}}/unlog-bench-${{name}}.XXXXXX")"

    "$(rlocation "{workspace}/$relpath")" \
        --bench_output_dir="$current_sink_out" \
        --benchmark_out="$result_path" \
        --benchmark_out_format=json \
        "$@"
    rm -rf "$current_sink_out"
    current_sink_out=""

    sleep {cooloff_seconds}
done
""".format(
        benchmark_lines = benchmark_lines,
        cooloff_seconds = _sleep_seconds(ctx.attr.cooloff_ms),
        runfiles = _runfiles_prologue(),
        workspace = ctx.workspace_name,
    )

    ctx.actions.write(script, script_content, is_executable = True)

    return DefaultInfo(
        executable = script,
        runfiles = runfiles
            .merge(ctx.runfiles(files = ctx.files._runfiles_lib))
            .merge(ctx.attr._runfiles_lib[DefaultInfo].default_runfiles),
    )

benchmark_suite = rule(
    implementation = _benchmark_suite_impl,
    executable = True,
    attrs = {
        "benchmarks": attr.label_list(
            cfg = "target",
            allow_files = False,
        ),
        "cooloff_ms": attr.int(default = 5000),
        "_runfiles_lib": attr.label(
            default = Label("@bazel_tools//tools/bash/runfiles"),
        ),
    },
)

_benchmark_case = rule(
    implementation = _benchmark_case_impl,
    executable = True,
    attrs = {
        "benchmark": attr.label(
            cfg = "target",
            executable = True,
            allow_files = False,
        ),
        "sink": attr.string(
            values = [
                "fd",
                "file",
                "stdout",
            ],
        ),
        "_runfiles_lib": attr.label(
            default = Label("@bazel_tools//tools/bash/runfiles"),
        ),
    },
)

def provider_benchmark(name, src, sink, deps = []):
    cc_binary(
        name = name + "_bench_bin",
        srcs = [src],
        copts = ["-std=c++23"],
        deps = [
            "//bench:common",
            "@benchmark",
        ] + deps,
        linkopts = ["-pthread"],
        visibility = ["//visibility:public"],
    )

    _benchmark_case(
        name = name,
        benchmark = ":" + name + "_bench_bin",
        sink = sink,
    )
