// The stub CAD backend: the interface with no kernel behind it. See cad_backend.h.
#include "cad/cad_backend.h"

#include <map>
#include <mutex>

namespace judas {
namespace {

class StubCad : public CadBackend {
public:
    explicit StubCad(std::string why) : why_(std::move(why)) {}
    std::string name() const override { return "stub"; }
    bool available(std::string & why) const override { why = why_; return false; }
    CadResult state() override { return no(); }
    CadResult new_part(const std::string &) override { return no(); }
    CadResult sketch(const json &) override { return no(); }
    CadResult feature(const json &) override { return no(); }
    CadResult set_param(const std::string &, const std::string &) override { return no(); }
    CadResult measure(const json &) override { return no(); }
    CadResult export_to(const std::string &, const std::string &) override { return no(); }
    CadResult undo() override { return no(); }

private:
    CadResult no() const { return {false, "CAD is unavailable: " + why_ + ". Do the calculations and written work; say plainly that no model was built."}; }
    std::string why_;
};

}  // namespace

std::shared_ptr<CadBackend> make_stub_cad_backend(const std::string & why) { return std::make_shared<StubCad>(why); }
#ifndef JUDAS_WITH_CAD
std::shared_ptr<CadBackend> make_cad_backend() { return make_stub_cad_backend(); }
#endif

std::shared_ptr<CadBackend> shared_cad_backend() {
    static std::shared_ptr<CadBackend> be = make_cad_backend();
    return be;
}

std::shared_ptr<CadBackend> cad_backend_for(const std::string & session) {
    if (session.empty()) return shared_cad_backend();
    static std::mutex mu;
    static std::map<std::string, std::shared_ptr<CadBackend>> by_session;
    std::lock_guard<std::mutex> lk(mu);
    auto & be = by_session[session];
    if (!be) be = make_cad_backend();
    return be;
}

std::mutex & cad_kernel_mutex() {
    static std::mutex mu;
    return mu;
}

}  // namespace judas
