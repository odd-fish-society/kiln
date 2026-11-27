// K8 smoke test (spec §10): daemon watches and rebuilds on change.
#include "hamr/test.h"

#include <csignal>
#include <fstream>
#include <thread>

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include "core/util.h"
#include "test_util.h"

using namespace kiln::testutil;

TEST_CASE("K8.1 daemon rebuilds on file change and exits cleanly") {
    TempDir tmp;
    write_tree(tmp.path, {
        {"kiln.workspace.cluau", "workspace \"k8\" { strict_globs = true }\n"},
        {"build/game.target.cluau",
         "target \"game\" {\n  platforms = { \"linux\" },\n  default_config = \"dev\",\n "
         " link = { \"app/main\" },\n}\n"},
        {"plugins/app/plugin.build.cluau",
         "plugin \"app\" {}\nmodule \"main\" { sources = glob(\"src/*.cpp\") }\n"},
        {"plugins/app/src/main.cpp", "int main() { return 0; }\n"},
    });

    int pipefd[2];
    REQUIRE(pipe(pipefd) == 0);
    pid_t pid = fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        chdir(tmp.path.c_str());
        execl(KILN_BINARY_PATH, KILN_BINARY_PATH, "daemon", "--progress=plain",
              "--workspace", ".", (char*)nullptr);
        _exit(127);
    }
    close(pipefd[1]);

    // Read output asynchronously.
    std::string out;
    auto drain = [&](int timeout_ms) {
        auto deadline = kiln::util::now_ms() + timeout_ms;
        char buf[4096];
        for (;;) {
            fd_set fds;
            FD_ZERO(&fds);
            FD_SET(pipefd[0], &fds);
            timeval tv;
            long remain = static_cast<long>(deadline - kiln::util::now_ms());
            tv.tv_sec = remain / 1000;
            tv.tv_usec = (remain % 1000) * 1000;
            int rc = select(pipefd[0] + 1, &fds, nullptr, nullptr, &tv);
            if (rc <= 0)
                return;
            ssize_t n = read(pipefd[0], buf, sizeof(buf));
            if (n <= 0)
                return;
            out.append(buf, static_cast<size_t>(n));
        }
    };

    // Wait for the initial build + watching line.
    for (int i = 0; i < 100 && out.find("watching") == std::string::npos; ++i) {
        drain(300);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    CHECK(out.find("watching") != std::string::npos);
    CHECK(out.find("tasks") != std::string::npos);

    // Touch a source; the daemon should rebuild.
    size_t before = out.size();
    std::ofstream f(tmp.path / "plugins/app/src/main.cpp", std::ios::app);
    f << "// daemon touch\n";
    f.close();

    bool rebuilt = false;
    for (int i = 0; i < 150 && !rebuilt; ++i) {
        drain(300);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        // New build output appeared after the touch.
        if (out.size() > before) {
            std::string tail = out.substr(before);
            if (tail.find("ok") != std::string::npos ||
                tail.find("watching") != std::string::npos)
                rebuilt = true;
        }
    }
    CHECK(rebuilt);

    kill(pid, SIGINT);
    int status = 0;
    waitpid(pid, &status, 0);
    drain(200);
    close(pipefd[0]);
    CHECK(WIFEXITED(status));
    CHECK(WEXITSTATUS(status) == 0);
}
