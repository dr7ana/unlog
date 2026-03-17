from __future__ import annotations

import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path

from plotly import graph_objects as go
from plotly.subplots import make_subplots


THREAD_COUNTS = [1, 2, 4, 8, 16]
AGGREGATES = {"mean", "median", "stddev", "cv"}
PROVIDER_ORDER = ["unlog", "spdlog", "quill", "fmtlog"]
PROVIDER_COLORS = {
    "unlog": "#1f77b4",
    "spdlog": "#ff7f0e",
    "quill": "#2ca02c",
    "fmtlog": "#d62728",
}
REPEATED_PATTERN = re.compile(
    r"^(?P<name>[^/]+)/repeated/iterations:(?P<iterations>\d+)/repeats:(?P<repeats>\d+)/threads:(?P<threads>\d+?)(?:_(?P<aggregate>mean|median|stddev|cv))?$"
)
MIN_TIME_PATTERN = re.compile(
    r"^(?P<name>[^/]+)/min_time/min_time:(?P<min_time>\d+(?:\.\d+)?)/threads:(?P<threads>\d+)$"
)


@dataclass(frozen=True)
class ResultRow:
    provider: str
    sink: str
    phase: str
    aggregate: str
    threads: int
    cpu_ns: float
    real_ns: float


def parse_provider_sink(path: Path) -> tuple[str, str]:
    stem = path.stem
    if stem.endswith("_stdout"):
        return stem.removesuffix("_stdout"), "stdout"

    if stem.endswith("_file"):
        return stem.removesuffix("_file"), "file"

    if stem.endswith("_fd"):
        return stem.removesuffix("_fd"), "fd"

    raise ValueError(f"unsupported result filename: {path.name}")


def parse_json(path: Path) -> list[ResultRow]:
    provider, sink = parse_provider_sink(path)
    payload = json.loads(path.read_text())
    rows: list[ResultRow] = []

    for benchmark in payload.get("benchmarks", []):
        name = benchmark["name"]

        repeated = REPEATED_PATTERN.match(name)
        if repeated:
            aggregate = repeated.group("aggregate") or "sample"
            rows.append(
                ResultRow(
                    provider=provider,
                    sink=sink,
                    phase="repeated",
                    aggregate=aggregate,
                    threads=int(repeated.group("threads")),
                    cpu_ns=float(benchmark["cpu_time"]),
                    real_ns=float(benchmark["real_time"]),
                )
            )
            continue

        min_time = MIN_TIME_PATTERN.match(name)
        if min_time:
            rows.append(
                ResultRow(
                    provider=provider,
                    sink=sink,
                    phase="min_time",
                    aggregate="sample",
                    threads=int(min_time.group("threads")),
                    cpu_ns=float(benchmark["cpu_time"]),
                    real_ns=float(benchmark["real_time"]),
                )
            )
            continue

    return rows


def load_rows(run_dir: Path) -> list[ResultRow]:
    json_paths = sorted(run_dir.glob("*.json"))
    if not json_paths:
        raise SystemExit(f"no benchmark json files found in {run_dir}")

    rows: list[ResultRow] = []
    for path in json_paths:
        rows.extend(parse_json(path))

    if not rows:
        raise SystemExit(f"no benchmark rows parsed from {run_dir}")

    return rows


def sinks(rows: list[ResultRow]) -> list[str]:
    order = ["file", "stdout", "fd"]
    present = {row.sink for row in rows}
    return [sink for sink in order if sink in present]


def providers(rows: list[ResultRow], sink: str) -> list[str]:
    present = {row.provider for row in rows if row.sink == sink}
    return [provider for provider in PROVIDER_ORDER if provider in present]


def select_aggregate(
    rows: list[ResultRow], sink: str, provider: str, aggregate: str
) -> list[tuple[int, float]]:
    selected = [
        (row.threads, row.cpu_ns)
        for row in rows
        if row.sink == sink
        and row.provider == provider
        and row.phase == "repeated"
        and row.aggregate == aggregate
    ]
    return sorted(selected)


def select_min_time(
    rows: list[ResultRow], sink: str, provider: str
) -> list[tuple[int, float]]:
    selected = [
        (row.threads, row.cpu_ns)
        for row in rows
        if row.sink == sink and row.provider == provider and row.phase == "min_time"
    ]
    return sorted(selected)


def render(rows: list[ResultRow], run_dir: Path) -> None:
    sink_list = sinks(rows)
    fig = make_subplots(
        rows=len(sink_list),
        cols=3,
        column_titles=[
            "<b>Repeated Mean CPU</b>",
            "<b>Repeated Median CPU</b>",
            "<b>Min Time CPU</b>",
        ],
    )
    legend_seen: set[str] = set()

    for row_index, sink in enumerate(sink_list, start=1):
        for provider in providers(rows, sink):
            mean_points = select_aggregate(rows, sink, provider, "mean")
            median_points = select_aggregate(rows, sink, provider, "median")
            min_time_points = select_min_time(rows, sink, provider)
            color = PROVIDER_COLORS[provider]
            show_legend = provider not in legend_seen

            if mean_points:
                fig.add_trace(
                    go.Scatter(
                        x=[threads for threads, _ in mean_points],
                        y=[value for _, value in mean_points],
                        mode="lines+markers",
                        name=provider,
                        legendgroup=provider,
                        showlegend=show_legend,
                        line={"color": color},
                        marker={"color": color},
                    ),
                    row=row_index,
                    col=1,
                )
                legend_seen.add(provider)

            if median_points:
                fig.add_trace(
                    go.Scatter(
                        x=[threads for threads, _ in median_points],
                        y=[value for _, value in median_points],
                        mode="lines+markers",
                        name=provider,
                        legendgroup=provider,
                        showlegend=False,
                        line={"color": color},
                        marker={"color": color},
                    ),
                    row=row_index,
                    col=2,
                )

            if min_time_points:
                fig.add_trace(
                    go.Scatter(
                        x=[threads for threads, _ in min_time_points],
                        y=[value for _, value in min_time_points],
                        mode="lines+markers",
                        name=provider,
                        legendgroup=provider,
                        showlegend=False,
                        line={"color": color},
                        marker={"color": color},
                    ),
                    row=row_index,
                    col=3,
                )

        for col_index in (1, 2, 3):
            fig.update_xaxes(
                row=row_index,
                col=col_index,
                type="category",
                categoryorder="array",
                categoryarray=THREAD_COUNTS,
                title_text="threads",
            )
            fig.update_yaxes(
                row=row_index,
                col=col_index,
                title_text="cpu ns",
            )

    fig.update_layout(
        title=f"<b>Benchmark Results:</b> {run_dir.name}<br><br><br>",
        template="plotly_dark",
        height=420 * max(1, len(sink_list)),
        autosize=True,
        margin={"l": 175, "r": 40, "t": 145, "b": 40},
    )

    for annotation in fig.layout.annotations[:3]:
        annotation.y += 0.03

    for row_index, sink in enumerate(sink_list, start=1):
        axis_index = (row_index - 1) * 3 + 1
        yaxis_name = "yaxis" if axis_index == 1 else f"yaxis{axis_index}"
        domain = fig.layout[yaxis_name].domain
        fig.add_annotation(
            x=-0.08,
            y=(domain[0] + domain[1]) / 2,
            xref="paper",
            yref="paper",
            text=f"<b>{sink}</b>",
            textangle=0,
            showarrow=False,
            font={"size": 15},
            xanchor="center",
            yanchor="middle",
        )

    report_path = run_dir / "report.html"
    fig.write_html(
        report_path,
        include_plotlyjs=True,
        full_html=True,
        config={"responsive": True},
    )
    print(report_path)


def latest_run_dir(results_root: Path) -> Path:
    candidates = sorted(path for path in results_root.iterdir() if path.is_dir())
    if not candidates:
        raise SystemExit(f"no benchmark run directories found in {results_root}")

    return candidates[-1]


def main() -> int:
    if len(sys.argv) > 2:
        raise SystemExit("usage: render_report.py [bench-results/timestamp-dir]")

    if len(sys.argv) == 2:
        run_dir = Path(sys.argv[1]).resolve()
    else:
        workspace_root = Path(__file__).resolve().parents[2]
        run_dir = latest_run_dir(workspace_root / "bench-results")

    if not run_dir.is_dir():
        raise SystemExit(f"not a directory: {run_dir}")

    rows = load_rows(run_dir)
    render(rows, run_dir)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
