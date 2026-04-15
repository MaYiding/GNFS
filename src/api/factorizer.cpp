#include <gnfs/api/factorizer.hpp>
#include <gnfs/api/pipeline.hpp>

namespace gnfs::api {

FactorResult factorize(const Integer& n) {
    return factorize(n, Config::auto_detect());
}

FactorResult factorize(const Integer& n, const Config& config) {
    Pipeline pipeline(n, config);
    return pipeline.run();
}

FactorResult factorize(const Integer& n, const Config& config, ProgressCallback cb) {
    Pipeline pipeline(n, config);
    pipeline.set_progress_callback(std::move(cb));
    return pipeline.run();
}

FactorResult factorize(const std::string& n_str) {
    return factorize(Integer(n_str));
}

FactorResult factorize(const std::string& n_str, const Config& config) {
    return factorize(Integer(n_str), config);
}

FactorResult factorize(const std::string& n_str, const Config& config, ProgressCallback cb) {
    return factorize(Integer(n_str), config, std::move(cb));
}

} // namespace gnfs::api
