// Copyright (C) 2002 Andrew Tridgell
// Copyright (C) 2011-2024 Joel Rosdahl and other contributors
//
// See doc/AUTHORS.adoc for a complete list of contributors.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by the Free
// Software Foundation; either version 3 of the License, or (at your option)
// any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
// FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
// more details.
//
// You should have received a copy of the GNU General Public License along with
// this program; if not, write to the Free Software Foundation, Inc., 51
// Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA

#include "execute.hpp"

#include "Config.hpp"
#include "Context.hpp"
#include "SignalHandler.hpp"
#include "Util.hpp"

#include <ccache.hpp>
#include <core/exceptions.hpp>
#include <util/DirEntry.hpp>
#include <util/Fd.hpp>
#include <util/Finalizer.hpp>
#include <util/PathString.hpp>
#include <util/TemporaryFile.hpp>
#include <util/error.hpp>
#include <util/expected.hpp>
#include <util/file.hpp>
#include <util/filesystem.hpp>
#include <util/fmtmacros.hpp>
#include <util/logging.hpp>
#include <util/path.hpp>
#include <util/string.hpp>
#include <util/wincompat.hpp>

#include <vector>

#ifdef HAVE_UNISTD_H
#  include <unistd.h>
#endif

#ifdef HAVE_SYS_WAIT_H
#  include <sys/wait.h>
#endif

namespace fs = util::filesystem;

using pstr = util::PathString;

#ifdef _WIN32
static int win32execute(const char* path,
                        const char* const* argv,
                        int doreturn,
                        int fd_stdout,
                        int fd_stderr,
                        const std::string& temp_dir);

int
execute(Context& ctx,
        const char* const* argv,
        util::Fd&& fd_out,
        util::Fd&& fd_err)
{
  LOG("Executing {}", util::format_argv_for_logging(argv));

  return win32execute(argv[0],
                      argv,
                      1,
                      fd_out.release(),
                      fd_err.release(),
                      ctx.config.temporary_dir());
}

void
execute_noreturn(const char* const* argv, const std::string& temp_dir)
{
  win32execute(argv[0], argv, 0, -1, -1, temp_dir);
}

std::string
win32getshell(const std::string& path)
{
  const char* path_list = getenv("PATH");
  std::string sh;
  if (util::to_lowercase(pstr(fs::path(path).extension()).str()) == ".sh"
      && path_list) {
    sh = pstr(find_executable_in_path("sh.exe", path_list)).str();
  }
  if (sh.empty() && getenv("CCACHE_DETECT_SHEBANG")) {
    // Detect shebang.
    util::FileStream fp(path, "r");
    if (fp) {
      char buf[10] = {0};
      fgets(buf, sizeof(buf) - 1, fp.get());
      if (std::string(buf) == "#!/bin/sh" && path_list) {
        sh = pstr(find_executable_in_path("sh.exe", path_list)).str();
      }
    }
  }

  return sh;
}

int
win32execute(const char* path,
             const char* const* argv,
             int doreturn,
             int fd_stdout,
             int fd_stderr,
             const std::string& temp_dir)
{
  BOOL is_process_in_job = false;
  DWORD dw_creation_flags = 0;

  {
    BOOL job_success =
      IsProcessInJob(GetCurrentProcess(), nullptr, &is_process_in_job);
    if (!job_success) {
      DWORD error = GetLastError();
      LOG("failed to IsProcessInJob: {} ({})",
          util::win32_error_message(error),
          error);
      return 0;
    }
    if (is_process_in_job) {
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobInfo = {};
      BOOL querySuccess =
        QueryInformationJobObject(nullptr,
                                  JobObjectExtendedLimitInformation,
                                  &jobInfo,
                                  sizeof(jobInfo),
                                  nullptr);
      if (!querySuccess) {
        DWORD error = GetLastError();
        LOG("failed to QueryInformationJobObject: {} ({})",
            util::win32_error_message(error),
            error);
        return 0;
      }

      const auto& limit_flags = jobInfo.BasicLimitInformation.LimitFlags;
      bool is_kill_active = limit_flags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
      bool allow_break_away = limit_flags & JOB_OBJECT_LIMIT_BREAKAWAY_OK;
      if (!is_kill_active && allow_break_away) {
        is_process_in_job = false;
        dw_creation_flags = CREATE_BREAKAWAY_FROM_JOB | CREATE_SUSPENDED;
      }
    } else {
      dw_creation_flags = CREATE_SUSPENDED;
    }
  }

  HANDLE job = nullptr;
  if (!is_process_in_job) {
    job = CreateJobObject(nullptr, nullptr);
    if (job == nullptr) {
      DWORD error = GetLastError();
      LOG("failed to CreateJobObject: {} ({})",
          util::win32_error_message(error),
          error);
      return -1;
    }

    {
      // Set the job object to terminate all child processes when the parent
      // process is killed.
      JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobInfo = {};
      jobInfo.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        | JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK;
      BOOL job_success = SetInformationJobObject(
        job, JobObjectExtendedLimitInformation, &jobInfo, sizeof(jobInfo));
      if (!job_success) {
        DWORD error = GetLastError();
        LOG("failed to JobObjectExtendedLimitInformation: {} ({})",
            util::win32_error_message(error),
            error);
        return -1;
      }
    }
  }

  PROCESS_INFORMATION pi;
  memset(&pi, 0x00, sizeof(pi));

  STARTUPINFO si;
  memset(&si, 0x00, sizeof(si));

  std::string sh = win32getshell(path);
  if (!sh.empty()) {
    path = sh.c_str();
  }

  si.cb = sizeof(STARTUPINFO);
  if (fd_stdout != -1) {
    si.hStdOutput = (HANDLE)_get_osfhandle(fd_stdout);
    si.hStdError = (HANDLE)_get_osfhandle(fd_stderr);
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.dwFlags = STARTF_USESTDHANDLES;
    if (si.hStdOutput == INVALID_HANDLE_VALUE
        || si.hStdError == INVALID_HANDLE_VALUE) {
      return -1;
    }
  } else {
    // Redirect subprocess stdout, stderr into current process.
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.dwFlags = STARTF_USESTDHANDLES;
    if (si.hStdOutput == INVALID_HANDLE_VALUE
        || si.hStdError == INVALID_HANDLE_VALUE) {
      return -1;
    }
  }

  std::string args = util::format_argv_as_win32_command_string(argv, sh);
  std::string full_path = util::add_exe_suffix(path);
  fs::path tmp_file_path;

  util::Finalizer tmp_file_remover([&tmp_file_path] {
    if (!tmp_file_path.empty()) {
      util::remove(tmp_file_path);
    }
  });

  if (args.length() > 8192) {
    auto tmp_file = util::value_or_throw<core::Fatal>(
      util::TemporaryFile::create(FMT("{}/cmd_args", temp_dir)));
    args = util::format_argv_as_win32_command_string(argv + 1, sh, true);
    util::write_fd(*tmp_file.fd, args.data(), args.length());
    args = FMT(R"("{}" "@{}")", full_path, tmp_file.path);
    tmp_file_path = tmp_file.path;
    LOG("Arguments from {}", tmp_file.path);
  }
  BOOL ret = CreateProcess(full_path.c_str(),
                           const_cast<char*>(args.c_str()),
                           nullptr,
                           nullptr,
                           1,
                           dw_creation_flags,
                           nullptr,
                           nullptr,
                           &si,
                           &pi);
  if (fd_stdout != -1) {
    close(fd_stdout);
    close(fd_stderr);
  }
  if (ret == 0) {
    DWORD error = GetLastError();
    LOG("failed to execute {}: {} ({})",
        full_path,
        util::win32_error_message(error),
        error);
    return -1;
  }
  if (job) {
    BOOL assign_success = AssignProcessToJobObject(job, pi.hProcess);
    if (!assign_success) {
      TerminateProcess(pi.hProcess, 1);

      DWORD error = GetLastError();
      LOG("failed to assign process to job object {}: {} ({})",
          full_path,
          util::win32_error_message(error),
          error);
      return -1;
    }
    ResumeThread(pi.hThread);
  }
  WaitForSingleObject(pi.hProcess, INFINITE);

  DWORD exitcode;
  GetExitCodeProcess(pi.hProcess, &exitcode);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  CloseHandle(job);
  if (!doreturn) {
    exit(exitcode);
  }
  return exitcode;
}

#else

// Execute a compiler backend, capturing all output to the given paths the full
// path to the compiler to run is in argv[0].
int
execute(Context& ctx,
        const char* const* argv,
        util::Fd&& fd_out,
        util::Fd&& fd_err)
{
  LOG("Executing {}", util::format_argv_for_logging(argv));

  {
    SignalHandlerBlocker signal_handler_blocker;
    ctx.compiler_pid = fork();
  }

  if (ctx.compiler_pid == -1) {
    throw core::Fatal(FMT("Failed to fork: {}", strerror(errno)));
  }

  if (ctx.compiler_pid == 0) {
    // Child.
    dup2(*fd_out, STDOUT_FILENO);
    fd_out.close();
    dup2(*fd_err, STDERR_FILENO);
    fd_err.close();
    exit(execv(argv[0], const_cast<char* const*>(argv)));
  }

  fd_out.close();
  fd_err.close();

  int status;
  int result;

  while ((result = waitpid(ctx.compiler_pid, &status, 0)) != ctx.compiler_pid) {
    if (result == -1 && errno == EINTR) {
      continue;
    }
    throw core::Fatal(FMT("waitpid failed: {}", strerror(errno)));
  }

  {
    SignalHandlerBlocker signal_handler_blocker;
    ctx.compiler_pid = 0;
  }

  if (WEXITSTATUS(status) == 0 && WIFSIGNALED(status)) {
    return -1;
  }

  return WEXITSTATUS(status);
}

void
execute_noreturn(const char* const* argv, const std::string& /*temp_dir*/)
{
  execv(argv[0], const_cast<char* const*>(argv));
}
#endif

std::string
find_executable(const Context& ctx,
                const std::string& name,
                const std::string& exclude_path)
{
  if (fs::path(name).is_absolute()) {
    return name;
  }

  std::string path_list = ctx.config.path();
  if (path_list.empty()) {
    path_list = getenv("PATH");
  }
  if (path_list.empty()) {
    LOG_RAW("No PATH variable");
    return {};
  }

  return find_executable_in_path(name, path_list, exclude_path).string();
}

fs::path
find_executable_in_path(const std::string& name,
                        const std::string& path_list,
                        const std::optional<fs::path>& exclude_path)
{
  if (path_list.empty()) {
    return {};
  }

  auto real_exclude_path =
    exclude_path ? fs::canonical(*exclude_path).value_or("") : "";

  // Search the path list looking for the first compiler of the right name that
  // isn't us.
  for (const auto& dir : util::split_path_list(path_list)) {
    const std::vector<fs::path> candidates = {
      dir / name,
#ifdef _WIN32
      dir / FMT("{}.exe", name),
#endif
    };
    for (const auto& candidate : candidates) {
      // A valid candidate:
      //
      // 1. Must exist (e.g., should not be a broken symlink) and be an
      //    executable.
      // 2. Must not resolve to the same program as argv[0] (i.e.,
      //    exclude_path). This can happen if ccache is masquerading as the
      //    compiler (with or without using a symlink).
      // 3. As an extra safety measure: must not be a ccache executable after
      //    resolving symlinks. This can happen if the candidate compiler is a
      //    symlink to another ccache executable.
      const bool candidate_exists =
#ifdef _WIN32
        util::DirEntry(candidate).is_regular_file();
#else
        access(candidate.c_str(), X_OK) == 0;
#endif
      if (candidate_exists) {
        auto real_candidate = fs::canonical(candidate);
        if (real_candidate && *real_candidate != real_exclude_path
            && !is_ccache_executable(*real_candidate)) {
          return candidate;
        }
      }
    }
  }

  return {};
}
