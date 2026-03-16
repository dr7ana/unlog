#include "utils.hpp"

namespace un::log::test {

    std::stringstream util::stream = std::stringstream{};
    std::mutex util::stream_mutex{};

}  // namespace un::log::test
