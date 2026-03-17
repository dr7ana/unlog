#pragma once

#include "common.hpp"

#include <benchmark/benchmark.h>

#include <string>
#include <string_view>

namespace unlog_bench {

    template <typename Provider>
    concept has_prepare_benchmark = requires { Provider::prepare_benchmark(); };

    template <typename Provider>
    void benchmark_setup(const benchmark::State&) {
        Provider::initialize(options());
        if constexpr (has_prepare_benchmark<Provider>) {
            Provider::prepare_benchmark();
        }
        Provider::prepare_thread();
    }

    template <typename Provider>
    void benchmark_teardown(const benchmark::State& state) {
        if (state.thread_index() == 0) {
            Provider::reset_state();
        }
    }

    template <typename Provider>
    void benchmark_body(benchmark::State& state) {
        static thread_local std::size_t index = 0;
        auto& messages = dataset();

        for (auto _ : state) {
            Provider::log(messages[index]);
            index = (index + 1) % messages.size();
        }
    }

    template <typename Provider>
    void register_benchmarks() {
        for (const int threads : thread_counts) {
            const auto repeated_name = benchmark_name(Provider::name(), "repeated");
            const auto min_time_name = benchmark_name(Provider::name(), "min_time");

            benchmark::RegisterBenchmark(repeated_name.c_str(), &benchmark_body<Provider>)
                    ->Threads(threads)
                    ->Iterations(repeated_iterations)
                    ->Repetitions(repeated_repetitions)
                    ->Unit(benchmark::kNanosecond)
                    ->Setup(&benchmark_setup<Provider>)
                    ->Teardown(&benchmark_teardown<Provider>);

            benchmark::RegisterBenchmark(min_time_name.c_str(), &benchmark_body<Provider>)
                    ->Threads(threads)
                    ->MinTime(min_time_seconds)
                    ->Unit(benchmark::kNanosecond)
                    ->Setup(&benchmark_setup<Provider>)
                    ->Teardown(&benchmark_teardown<Provider>);
        }
    }

    template <typename Provider>
    int run_benchmarks(int argc, char** argv) {
        parse_options(&argc, argv);
        benchmark::Initialize(&argc, argv);

        if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
            return 1;
        }

        benchmark::AddCustomContext("provider", std::string{Provider::name()});
        benchmark::AddCustomContext("sink", std::string{sink_name(options().sink)});
        benchmark::AddCustomContext("dataset_size", std::to_string(options().dataset_size));

        register_benchmarks<Provider>();
        benchmark::RunSpecifiedBenchmarks();
        benchmark::Shutdown();
        return 0;
    }

}  // namespace unlog_bench
