#include "utils.hpp"

#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace un::log::test {
    namespace detail {
        static void write_record(
                backend::producer& producer, int32_t source_line, std::string_view msg, log_level level) {
            auto& ring = producer.ring();
            auto seq = producer.next_sequence();
            auto reservation = ring.try_reserve(
                    msg.size(),
                    "drain-test",
                    "004.cpp",
                    "write_record",
                    source_line,
                    level,
                    static_cast<uint64_t>(1000 + seq),
                    producer.thread_id(),
                    seq);

            REQUIRE(reservation.has_value());
            if (!msg.empty())
                std::memcpy(reservation->payload.data(), msg.data(), msg.size());
            ring.commit(*reservation);
        }

    }  // namespace detail

    TEST_CASE("004 - record header commit publication", "[004][record]") {
        runtime_state_guard guard;

        backend::record_header header{};
        header.total_size = backend::align_record_size(sizeof(backend::record_header) + 5u);

        backend::clear_commit(header);
        CHECK(backend::committed_size_acquire(header) == 0u);

        backend::publish_commit(header);
        CHECK(backend::committed_size_acquire(header) == header.total_size);

        CHECK(backend::decode_level(backend::encode_level(log_level::warn)) == log_level::warn);
    }

    TEST_CASE("004 - record view validation", "[004][record]") {
        runtime_state_guard guard;

        constexpr size_t payload_size = 3;
        auto total_size = backend::align_record_size(sizeof(backend::record_header) + payload_size);

        std::vector<uint64_t> words((total_size + sizeof(uint64_t) - 1u) / sizeof(uint64_t));
        auto bytes = std::span<std::byte>{reinterpret_cast<std::byte*>(words.data()), total_size};

        auto* header = reinterpret_cast<backend::record_header*>(bytes.data());
        *header = {};
        header->version = backend::record_abi_version;
        header->kind = backend::record_kind::data;
        header->header_size = static_cast<uint8_t>(sizeof(backend::record_header));
        header->total_size = total_size;
        header->payload_size = payload_size;
        header->logger_name = "unit-test";
        header->source_file = "004.cpp";
        header->source_function = "record_view_validation";
        header->source_line = 42;
        header->thread_id = 7;
        header->sequence = 9;
        header->level = backend::encode_level(log_level::debug);
        header->timestamp = 12345;

        backend::clear_commit(*header);
        bytes[sizeof(backend::record_header) + 0] = std::byte{'a'};
        bytes[sizeof(backend::record_header) + 1] = std::byte{'b'};
        bytes[sizeof(backend::record_header) + 2] = std::byte{'c'};
        backend::publish_commit(*header);

        auto view = backend::try_make_record_view(std::span<const std::byte>{bytes});
        REQUIRE(view.has_value());
        REQUIRE(view->has_header());
        CHECK(view->committed());
        CHECK_FALSE(view->is_padding());
        CHECK(std::string_view{view->header->logger_name} == "unit-test");
        CHECK(std::string_view{view->header->source_file} == "004.cpp");
        CHECK(std::string_view{view->header->source_function} == "record_view_validation");
        CHECK(view->header->source_line == 42);
        CHECK(view->header->thread_id == 7u);
        CHECK(view->header->sequence == 9u);
        CHECK(view->level() == log_level::debug);
        auto expected_payload_size = total_size - sizeof(backend::record_header);
        REQUIRE(view->payload.size() == expected_payload_size);
        CHECK(view->payload[0] == std::byte{'a'});
        CHECK(view->payload[1] == std::byte{'b'});
        CHECK(view->payload[2] == std::byte{'c'});
        for (size_t i = payload_size; i < view->payload.size(); ++i)
            CHECK(view->payload[i] == std::byte{0});

        auto misaligned = std::span<const std::byte>{bytes.data() + 1, bytes.size() - 1};
        CHECK_FALSE(backend::try_make_record_view(misaligned).has_value());

        header->version = 99;
        CHECK_FALSE(backend::try_make_record_view(std::span<const std::byte>{bytes}).has_value());
    }

    TEST_CASE("004 - ring reserve commit and consume", "[004][ring]") {
        runtime_state_guard guard;

        backend::ring_buffer ring;
        constexpr std::string_view payload = "hello";

        auto reservation = ring.try_reserve(
                payload.size(), "ring-test", "004.cpp", "ring_reserve", 11, log_level::info, 1111, 22, 33);
        REQUIRE(reservation.has_value());
        std::memcpy(reservation->payload.data(), payload.data(), payload.size());
        ring.commit(*reservation);

        auto view = ring.try_peek();
        REQUIRE(view.has_value());
        CHECK_FALSE(view->is_padding());
        CHECK(std::string_view{view->header->logger_name} == "ring-test");
        CHECK(std::string_view{view->header->source_file} == "004.cpp");
        CHECK(std::string_view{view->header->source_function} == "ring_reserve");
        CHECK(view->header->source_line == 11);
        CHECK(view->header->thread_id == 22u);
        CHECK(view->header->sequence == 33u);
        CHECK(view->level() == log_level::info);
        auto expected_payload_size = backend::align_record_size(sizeof(backend::record_header) + payload.size()) -
                                     sizeof(backend::record_header);
        CHECK(view->payload.size() == expected_payload_size);

        auto read = std::string{reinterpret_cast<const char*>(view->payload.data()), payload.size()};
        CHECK(read == payload);

        CHECK(ring.consume_peeked(*view));
        CHECK_FALSE(ring.try_peek().has_value());
    }

    TEST_CASE("004 - ring wrap emits padding record", "[004][ring]") {
        runtime_state_guard guard;

        backend::ring_buffer ring;
        auto header_size = sizeof(backend::record_header);
        auto cap = ring.capacity();
        REQUIRE(cap == backend::thread_ring_capacity);

        auto payload1 = cap - (2u * header_size);
        auto first = ring.try_reserve(payload1, "wrap-test", "004.cpp", "ring_wrap", 1, log_level::info, 10, 1, 1);
        REQUIRE(first.has_value());
        ring.commit(*first);

        auto first_view = ring.try_peek();
        REQUIRE(first_view.has_value());
        CHECK_FALSE(first_view->is_padding());
        REQUIRE(ring.consume_peeked(*first_view));

        auto second = ring.try_reserve(8, "wrap-test", "004.cpp", "ring_wrap", 2, log_level::warn, 20, 1, 2);
        REQUIRE(second.has_value());
        ring.commit(*second);

        auto pad_view = ring.try_peek();
        REQUIRE(pad_view.has_value());
        CHECK(pad_view->is_padding());
        CHECK(pad_view->header->total_size == header_size);
        REQUIRE(ring.consume_peeked(*pad_view));

        auto second_view = ring.try_peek();
        REQUIRE(second_view.has_value());
        CHECK_FALSE(second_view->is_padding());
        CHECK(second_view->header->source_line == 2);
        CHECK(second_view->level() == log_level::warn);
        REQUIRE(ring.consume_peeked(*second_view));
    }

    TEST_CASE("004 - ring skip wrap gap smaller than header", "[004][ring]") {
        runtime_state_guard guard;

        backend::ring_buffer ring;
        auto header_size = sizeof(backend::record_header);
        REQUIRE(header_size == sizeof(backend::record_header));

        auto cap = ring.capacity();
        auto gap = header_size - 8u;
        auto payload1 = cap - header_size - gap;

        auto first = ring.try_reserve(payload1, "gap-test", "004.cpp", "ring_gap", 10, log_level::info, 10, 2, 1);
        REQUIRE(first.has_value());
        ring.commit(*first);

        auto first_view = ring.try_peek();
        REQUIRE(first_view.has_value());
        REQUIRE(ring.consume_peeked(*first_view));

        auto second = ring.try_reserve(8, "gap-test", "004.cpp", "ring_gap", 20, log_level::err, 20, 2, 2);
        REQUIRE(second.has_value());
        ring.commit(*second);

        CHECK_FALSE(ring.try_peek().has_value());
        CHECK(ring.skip_wrap_gap());

        auto second_view = ring.try_peek();
        REQUIRE(second_view.has_value());
        CHECK_FALSE(second_view->is_padding());
        CHECK(second_view->header->source_line == 20);
        CHECK(second_view->level() == log_level::err);
        REQUIRE(ring.consume_peeked(*second_view));
    }

    TEST_CASE("004 - producer tls identity and sequencing", "[004][producer]") {
        runtime_state_guard guard;

        auto& p1 = backend::get_thread_producer();
        auto& p2 = backend::get_thread_producer();

        CHECK(&p1 == &p2);

        auto seq0 = p1.next_sequence();
        auto seq1 = p1.next_sequence();
        CHECK(seq1 == (seq0 + 1u));

        CHECK(backend::active_producer_count() == backend::producer_snapshot().size());
    }

    TEST_CASE("004 - producer drain batch honors max limit", "[004][producer]") {
        runtime_state_guard guard;

        auto& p = backend::get_thread_producer();

        test_helper::drain_all_records();
        detail::write_record(p, 101, "one", log_level::info);
        detail::write_record(p, 102, "two", log_level::warn);

        std::vector<int32_t> seen_lines;
        auto first = backend::drain_batch(1, [&](backend::producer& owner, const backend::record_view& view) {
            CHECK(owner.thread_id() == p.thread_id());
            seen_lines.push_back(view.header->source_line);
            return true;
        });

        CHECK(first.drained_records == 1u);
        CHECK(first.hit_limit);
        CHECK(seen_lines.size() == 1u);

        auto second = backend::drain_batch(32, [&](backend::producer&, const backend::record_view& view) {
            seen_lines.push_back(view.header->source_line);
            return true;
        });

        CHECK(second.drained_records == 1u);
        CHECK_FALSE(second.hit_limit);
        CHECK(seen_lines.size() == 2u);

        auto done = backend::drain_batch(32, [](backend::producer&, const backend::record_view&) { return true; });
        CHECK(done.drained_records == 0u);
    }

    TEST_CASE("004 - logger path uses producer ring", "[004][producer][logger]") {
        runtime_state_guard guard;

        auto cfg = config<>::make_sqpoll("hotpotato");
        make_logger(cfg, true);
        util::capture_test_logs(cfg);

        auto& producer = backend::get_thread_producer();
        auto seq_before = producer.next_sequence();

        unlog::info("ring-message");

        auto seq_after = producer.next_sequence();
        CHECK(seq_after == (seq_before + 2u));

        auto drained = backend::drain_batch(64, [](backend::producer&, const backend::record_view&) { return true; });
        CHECK(drained.drained_records == 0u);
        CHECK(drained.skipped_padding == 0u);

        REQUIRE_CONTAINS("ring-message");
    }

    TEST_CASE("004 - backend stats move on emitted log", "[004][backend][stats]") {
        runtime_state_guard guard;

        util::capture_test_logs();

        auto before = un::log::detail::backend_stats();
        unlog::info("stats-check-{}", 7);
        auto after = un::log::detail::backend_stats();

        CHECK(after.emitted == (before.emitted + 1u));
    }

    TEST_CASE("004 - producer counters drop oversize", "[004][backend][stats][overflow]") {
        runtime_state_guard guard;

        auto cfg = config<>::make_sqpoll("counter-drop-oversize");
        CHECK((std::same_as<typename decltype(cfg)::overflow_type, options::drop>));
        cfg.sqpoll_queue_depth = 64;

        make_logger(cfg, true);
        util::capture_test_logs(cfg);

        auto before = un::log::detail::backend_stats();
        auto msg = std::string{"oversize-drop-"} + std::string(8192u, 'x');
        REQUIRE(msg.size() == 8206u);

        unlog::info("{}", msg);

        auto after = un::log::detail::backend_stats();
        CHECK(after.emitted == before.emitted);
        CHECK(after.dropped == (before.dropped + 1u));
        CHECK(after.truncated == before.truncated);
        CHECK(util::stream.str().empty());
    }

    TEST_CASE("004 - producer counters truncate oversize", "[004][backend][stats][overflow]") {
        runtime_state_guard guard;

        auto cfg = config<options::truncate>::make_sqpoll("counter-truncate-oversize");
        CHECK((std::same_as<typename decltype(cfg)::overflow_type, options::truncate>));
        cfg.sqpoll_queue_depth = 64;

        make_logger(cfg, true);
        util::capture_test_logs(cfg);

        auto before = un::log::detail::backend_stats();
        auto msg = std::string{"oversize-truncate-"} + std::string(8192u, 'x');
        REQUIRE(msg.size() == 8210u);

        unlog::info("{}", msg);

        auto after = un::log::detail::backend_stats();
        CHECK(after.emitted == (before.emitted + 1u));
        CHECK(after.dropped == before.dropped);
        CHECK(after.truncated == (before.truncated + 1u));
        REQUIRE_CONTAINS("oversize-truncate-");
        CHECK(util::stream.str().contains(msg) == false);
    }

}  // namespace un::log::test
