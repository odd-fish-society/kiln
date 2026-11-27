#include "core/live.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>

#include <elf.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <unistd.h>

#include "core/util.h"
#include "nlohmann/json.hpp"

namespace kiln::live {

using json = nlohmann::json;

namespace {

// The live directory's lock: a link's retain + record write and a prune
// never interleave (two links run at once in the link pool).
struct DirLock {
    int fd = -1;
    explicit DirLock(const fs::path& dir) {
        std::error_code ec;
        fs::create_directories(dir, ec);
        fd = ::open((dir / ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
        if (fd >= 0)
            ::flock(fd, LOCK_EX);
    }
    ~DirLock() {
        if (fd >= 0) {
            ::flock(fd, LOCK_UN);
            ::close(fd);
        }
    }
};

Result<void> write_atomic(const fs::path& path, const std::string& text) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    fs::path tmp = path;
    tmp += ".tmp." + std::to_string(::getpid());
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f)
            return Result<void>::fail("cannot write " + tmp.generic_string());
        f << text;
        if (!f)
            return Result<void>::fail("cannot write " + tmp.generic_string());
    }
    fs::rename(tmp, path, ec);
    if (ec)
        return Result<void>::fail("cannot move " + tmp.generic_string() + " into place: " +
                                  ec.message());
    return Result<void>::success();
}

// Retains `src` as the content-addressed `dst`. A hard link costs nothing and
// is safe because every object writer replaces its output by rename (Clang
// writes a temporary and renames it), so the retained inode never changes.
Result<void> retain(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    if (fs::exists(dst, ec))
        return Result<void>::success();
    fs::create_directories(dst.parent_path(), ec);
    fs::path tmp = dst;
    tmp += ".tmp." + std::to_string(::getpid());
    fs::remove(tmp, ec);
    if (::link(src.c_str(), tmp.c_str()) != 0) {
        fs::copy_file(src, tmp, fs::copy_options::overwrite_existing, ec);
        if (ec)
            return Result<void>::fail("cannot retain " + src.generic_string() + ": " +
                                      ec.message());
    }
    fs::rename(tmp, dst, ec);
    if (ec)
        return Result<void>::fail("cannot retain " + src.generic_string() + ": " + ec.message());
    return Result<void>::success();
}

json to_json(const Record& r) {
    json tus = json::array();
    for (auto& t : r.tus)
        tus.push_back({{"source", t.source}, {"object", t.object}, {"hash", t.hash}});
    return {{"version", 1},     {"build_id", r.build_id}, {"target", r.target},
            {"config", r.config}, {"exe", r.exe},         {"tus", tus}};
}

bool pid_alive(int pid) {
    return pid > 0 && (::kill(pid, 0) == 0 || errno == EPERM);
}

std::string hex_of(const unsigned char* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s.push_back(d[p[i] >> 4]);
        s.push_back(d[p[i] & 15]);
    }
    return s;
}

} // namespace

bool is_live_config(const WorkspaceConfig& ws, const ConfigTuple& ct) {
    return std::find(ws.link_record.begin(), ws.link_record.end(), ct.config) != ws.link_record.end();
}

const std::vector<std::string>& live_compile_flags() {
    // One section per function and per variable: a relocation against a
    // section symbol then names exactly one symbol, so the loader never has
    // to decode an addend to know which static an edge reaches.
    static const std::vector<std::string> flags = {"-falign-functions=16", "-fno-omit-frame-pointer",
                                                   "-ffunction-sections", "-fdata-sections"};
    return flags;
}

std::string forbidden_live_flag(const std::vector<std::string>& argv) {
    for (auto& a : argv) {
        if (util::starts_with(a, "-flto") || a.find("--icf") != std::string::npos ||
            a.find("--gc-sections") != std::string::npos || a == "-s" ||
            a.find("--strip-all") != std::string::npos || a == "-Wl,-s" ||
            a.find("--discard-all") != std::string::npos || a == "-Wl,-x")
            return a;
    }
    return {};
}

fs::path live_dir(const fs::path& root, const std::string& config) {
    return root / ".kiln/out" / config / "live";
}
fs::path object_path(const fs::path& root, const std::string& config, const std::string& hash) {
    return live_dir(root, config) / "objects" / (hash + ".o");
}
fs::path record_path(const fs::path& root, const std::string& config,
                     const std::string& build_id) {
    return live_dir(root, config) / "records" / (build_id + ".json");
}
fs::path newest_record_path(const fs::path& root, const std::string& exe_rel) {
    return root / (exe_rel + ".live.json");
}

Result<std::string> read_build_id(const fs::path& elf) {
    std::ifstream f(elf, std::ios::binary);
    if (!f)
        return Result<std::string>::fail("cannot open " + elf.generic_string());
    Elf64_Ehdr eh{};
    f.read(reinterpret_cast<char*>(&eh), sizeof(eh));
    if (!f || std::memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
        eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_shentsize != sizeof(Elf64_Shdr))
        return Result<std::string>::fail(elf.generic_string() + ": not a 64-bit ELF file");
    // Extended numbering: 0xff00+ sections keep the count in section 0.
    size_t shnum = eh.e_shnum;
    if (shnum == 0) {
        Elf64_Shdr first{};
        f.seekg(static_cast<std::streamoff>(eh.e_shoff));
        f.read(reinterpret_cast<char*>(&first), sizeof(first));
        shnum = static_cast<size_t>(first.sh_size);
    }
    std::vector<Elf64_Shdr> sh(shnum);
    f.seekg(static_cast<std::streamoff>(eh.e_shoff));
    f.read(reinterpret_cast<char*>(sh.data()),
           static_cast<std::streamsize>(sh.size() * sizeof(Elf64_Shdr)));
    if (!f)
        return Result<std::string>::fail(elf.generic_string() + ": bad section headers");
    for (auto& s : sh) {
        if (s.sh_type != SHT_NOTE || s.sh_size > (1u << 20))
            continue;
        std::vector<unsigned char> buf(s.sh_size);
        f.seekg(static_cast<std::streamoff>(s.sh_offset));
        f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
        if (!f)
            break;
        size_t off = 0;
        while (off + sizeof(Elf64_Nhdr) <= buf.size()) {
            Elf64_Nhdr nh;
            std::memcpy(&nh, buf.data() + off, sizeof(nh));
            size_t name_off = off + sizeof(nh);
            size_t desc_off = name_off + ((nh.n_namesz + 3) & ~3u);
            size_t next = desc_off + ((nh.n_descsz + 3) & ~3u);
            if (next > buf.size())
                break;
            if (nh.n_type == NT_GNU_BUILD_ID && nh.n_namesz == 4 &&
                std::memcmp(buf.data() + name_off, "GNU", 4) == 0)
                return Result<std::string>::success(hex_of(buf.data() + desc_off, nh.n_descsz));
            off = next;
        }
    }
    return Result<std::string>::fail(elf.generic_string() + ": no GNU build-id note");
}

Result<Record> write_link_record(const fs::path& root, const std::string& config,
                                 const std::string& target, const std::string& exe_rel,
                                 std::vector<std::pair<std::string, std::string>> source_objects,
                                 const HashFn& hash_of) {
    Record r;
    r.target = target;
    r.config = config;
    r.exe = exe_rel;
    auto bid = read_build_id(root / exe_rel);
    if (!bid.ok())
        return Result<Record>::fail(bid.error);
    r.build_id = *bid;
    std::sort(source_objects.begin(), source_objects.end());
    for (auto& [source, object] : source_objects) {
        auto h = hash_of(object);
        if (!h.ok())
            return Result<Record>::fail("live record: " + h.error);
        r.tus.push_back({source, object, h->hex()});
    }

    DirLock lock(live_dir(root, config));
    for (auto& t : r.tus) {
        auto kept = retain(root / t.object, object_path(root, config, t.hash));
        if (!kept.ok())
            return Result<Record>::fail(kept.error);
    }
    std::string text = to_json(r).dump(1);
    auto w = write_atomic(record_path(root, config, r.build_id), text);
    if (!w.ok())
        return Result<Record>::fail(w.error);
    w = write_atomic(newest_record_path(root, exe_rel), text);
    if (!w.ok())
        return Result<Record>::fail(w.error);
    return Result<Record>::success(std::move(r));
}

Result<Record> load_record(const fs::path& path) {
    auto text = util::read_file(path);
    if (!text.ok())
        return Result<Record>::fail(text.error);
    auto doc = json::parse(*text, nullptr, false);
    if (!doc.is_object() || doc.value("version", 0) != 1)
        return Result<Record>::fail(path.generic_string() + ": not a live record");
    Record r;
    r.build_id = doc.value("build_id", "");
    r.target = doc.value("target", "");
    r.config = doc.value("config", "");
    r.exe = doc.value("exe", "");
    for (auto& t : doc.value("tus", json::array()))
        r.tus.push_back({t.value("source", ""), t.value("object", ""), t.value("hash", "")});
    return Result<Record>::success(std::move(r));
}

std::optional<Record> find_record(const fs::path& root, const std::string& build_id) {
    std::error_code ec;
    for (auto& cfg : fs::directory_iterator(root / ".kiln/out", ec)) {
        fs::path p = cfg.path() / "live/records" / (build_id + ".json");
        if (!fs::exists(p, ec))
            continue;
        auto r = load_record(p);
        if (r.ok())
            return *r;
    }
    return std::nullopt;
}

std::set<std::string> running_build_ids(const fs::path& root) {
    std::set<std::string> ids;
    std::error_code ec;
    const std::string out_prefix = fs::weakly_canonical(root / ".kiln/out", ec).generic_string() + "/";
    for (auto& e : fs::directory_iterator("/proc", ec)) {
        const std::string name = e.path().filename().generic_string();
        if (name.empty() || name.find_first_not_of("0123456789") != std::string::npos)
            continue;
        char buf[4096];
        ssize_t n = ::readlink((e.path() / "exe").c_str(), buf, sizeof(buf) - 1);
        if (n <= 0)
            continue;
        buf[n] = 0;
        if (!util::starts_with(buf, out_prefix))
            continue;
        // /proc/<pid>/exe opens the image the process runs, even after a
        // relink replaced (or deleted) the file at its path.
        auto bid = read_build_id(e.path() / "exe");
        if (bid.ok())
            ids.insert(*bid);
    }
    return ids;
}

void write_pin(const fs::path& root, const std::string& config, int pid,
               const std::set<std::string>& hashes) {
    json doc = {{"pid", pid}, {"hashes", json(std::vector<std::string>(hashes.begin(), hashes.end()))}};
    (void)write_atomic(live_dir(root, config) / "pins" / (std::to_string(pid) + ".json"),
                       doc.dump());
}

void remove_pin(const fs::path& root, const std::string& config, int pid) {
    std::error_code ec;
    fs::remove(live_dir(root, config) / "pins" / (std::to_string(pid) + ".json"), ec);
}

PruneResult prune(const fs::path& root, const std::string& config) {
    PruneResult out;
    fs::path dir = live_dir(root, config);
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
        return out;
    DirLock lock(dir);
    std::set<std::string> keep_ids = running_build_ids(root);
    // Each target's newest record stays: it is what the next launch runs.
    for (auto& e : fs::directory_iterator(root / ".kiln/out" / config / "bin", ec)) {
        const std::string fn = e.path().filename().generic_string();
        if (fn.size() > 10 && fn.compare(fn.size() - 10, 10, ".live.json") == 0) {
            auto r = load_record(e.path());
            if (r.ok())
                keep_ids.insert(r->build_id);
        }
    }
    std::set<std::string> keep_hashes;
    for (auto& e : fs::directory_iterator(dir / "records", ec)) {
        if (e.path().extension() != ".json")
            continue;
        std::string bid = e.path().stem().generic_string();
        if (!keep_ids.count(bid)) {
            fs::remove(e.path(), ec);
            ++out.records;
            continue;
        }
        auto r = load_record(e.path());
        if (r.ok())
            for (auto& t : r->tus)
                keep_hashes.insert(t.hash);
    }
    for (auto& e : fs::directory_iterator(dir / "pins", ec)) {
        int pid = std::atoi(e.path().stem().c_str());
        if (!pid_alive(pid)) {
            fs::remove(e.path(), ec);
            continue;
        }
        auto text = util::read_file(e.path());
        if (!text.ok())
            continue;
        auto doc = json::parse(*text, nullptr, false);
        if (doc.is_object())
            for (auto& h : doc.value("hashes", json::array()))
                if (h.is_string())
                    keep_hashes.insert(h.get<std::string>());
    }
    for (auto& e : fs::directory_iterator(dir / "objects", ec)) {
        if (e.path().extension() != ".o")
            continue; // a retain's in-flight temporary
        if (!keep_hashes.count(e.path().stem().generic_string())) {
            fs::remove(e.path(), ec);
            ++out.objects;
        }
    }
    return out;
}

std::vector<PatchTu> diff(const std::map<std::string, std::string>& running,
                          const Record& current) {
    std::vector<PatchTu> out;
    for (auto& t : current.tus) {
        auto it = running.find(t.source);
        if (it == running.end())
            out.push_back({t.source, t.hash, ""});
        else if (it->second != t.hash)
            out.push_back({t.source, t.hash, it->second});
    }
    return out;
}

std::string reflection_plugin(const std::string& source) {
    // .kiln/out/<cfg>/gen/<plugin>/<plugin>.reflection[.presentation].gen.cpp
    const std::string base = util::path_basename(source);
    auto dot = base.find(".reflection.");
    if (dot == std::string::npos || source.find("/gen/") == std::string::npos ||
        base.size() < 8 || base.compare(base.size() - 8, 8, ".gen.cpp") != 0)
        return {};
    return base.substr(0, dot);
}

} // namespace kiln::live
