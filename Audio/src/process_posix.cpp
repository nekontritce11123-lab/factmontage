// SPDX-License-Identifier: MIT
#include "audio.hpp"
#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
namespace studio_audio {
    ProcessResult runProcess(const std::vector < std::string > &args, const fs::path&cwd, int timeout, const std::atomic_bool*cancel,
                             std::function<void(std::string_view)> onOutput) {
        if (args.empty() || args.front().empty()) throw Error("Пустая команда.");
        if (cancel && cancel->load()) throw Cancelled();
        int pipes[2];
        if (pipe2(pipes, O_CLOEXEC)) throw Error("Не удалось создать канал процесса.");
        // Build argv before fork: no C++ allocation in the child of a multi-threaded Qt process.
        std::vector < char* > argv;
        for (auto&s: args) argv.push_back(const_cast < char* > (s.c_str()));
        argv.push_back(nullptr);
        const auto cwdString = cwd.string();
        pid_t pid = fork();
        if (pid < 0) {
            close(pipes[0]);
            close(pipes[1]);
            throw Error("Не удалось запустить обработчик.");
        }
        if (pid == 0) {
            setpgid(0, 0);
            dup2(pipes[1], STDOUT_FILENO);
            dup2(pipes[1], STDERR_FILENO);
            close(pipes[0]);
            close(pipes[1]);
            int nullfd = open("/dev/null", O_RDONLY);
            if (nullfd >= 0) {
                dup2(nullfd, STDIN_FILENO);
                close(nullfd);
            }
            if (!cwdString.empty() && chdir(cwdString.c_str()) != 0) _exit(126);
            execvp(argv[0], argv.data());
            _exit(127);
        }
        close(pipes[1]);
        setpgid(pid, pid);
        fcntl(pipes[0], F_SETFL, fcntl(pipes[0], F_GETFL)| O_NONBLOCK);
        using Clock = std::chrono::steady_clock;
        auto started = Clock::now(), terminated = started;
        bool wasCancelled = false, timedOut = false, termSent = false, done = false;
        int status = 0;
        std::string output;
        constexpr size_t maxOutput = 8*1024*1024;
        auto drain = [&] {
            char buf[16384];
            for (; ; ) {
                ssize_t n = read(pipes[0], buf, sizeof(buf));
                if (n <= 0) break;
                if (onOutput) onOutput(std::string_view(buf, size_t(n)));
                output.append(buf, size_t(n));
                if (output.size() > maxOutput) output.erase(0, output.size()- maxOutput);
            }
        };
        while (!done) {
            drain();
            const auto now = Clock::now();
            if (cancel && cancel->load()) wasCancelled = true;
            if (std::chrono::duration < double > (now- started).count() > timeout) timedOut = true;
            if ((wasCancelled || timedOut) && !termSent) {
                kill(- pid, SIGTERM);
                termSent = true;
                terminated = now;
            }
            if (termSent && std::chrono::duration < double > (now- terminated).count() > .7) kill(- pid, SIGKILL);
            pid_t w = waitpid(pid, &status, WNOHANG);
            if (w == pid) done = true;
            else if (w < 0 && errno != EINTR) {
                kill(- pid, SIGKILL);
                while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
                };
                close(pipes[0]);
                throw Error("Ошибка ожидания обработчика.");
            } else {
                pollfd fd {
                    pipes[0], POLLIN, 0
                };
                poll(&fd, 1, 40);
            }
        }
        drain();
        close(pipes[0]);
        if (wasCancelled) throw Cancelled();
        if (timedOut) throw Error("Превышен предел времени обработки; временные файлы удалены.");
        return {
            WIFEXITED(status)? WEXITSTATUS(status): 128+(WIFSIGNALED(status)? WTERMSIG(status): 0), output
        };
    }
}
