#include "core/config.h"

#include <algorithm>

namespace kiln {

Hash256 ConfigTuple::hash() const {
    CanonicalHasher h;
    h.field("platform", platform);
    h.field("config", config);
    h.begin_list("features");
    for (auto& [k, v] : features) { // std::map ⇒ sorted ⇒ deterministic
        h.item(k);
        h.item(v);
    }
    h.end_list();
    h.begin_list("enabled_plugins");
    for (auto& p : enabled_plugins)
        h.item(p);
    h.end_list();
    h.begin_list("env_allow");
    for (auto& [k, v] : env_allow) {
        h.item(k);
        h.item(v);
    }
    h.end_list();
    return h.finish();
}

std::string ConfigTuple::name() const { return platform + "-" + config; }

std::string ConfigTuple::display() const {
    std::string out = platform + "-" + config;
    if (!features.empty()) {
        out += "+";
        bool first = true;
        for (auto& [k, v] : features) {
            if (!first)
                out += ",";
            first = false;
            out += k + "=" + v;
        }
    }
    return out;
}

} // namespace kiln
