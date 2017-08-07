// Copyright (c) 2017 The Brave Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "atom/app/atom_main.h"

#include <stdlib.h>

#include "atom/app/atom_main_delegate.h"
#include "atom/app/uv_task_runner.h"
#include "atom/common/atom_command_line.h"
#include "base/at_exit.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "content/public/app/content_main.h"

#include "base/strings/string_util.h"
#include "chrome/common/chrome_switches.h"
#include "brave/common/brave_paths.h"

#if defined(OS_WIN)
#include <windows.h>  // windows.h must be included first

#include <shellapi.h>
#include <shellscalingapi.h>
#include <tchar.h>

#include "base/debug/dump_without_crashing.h"
#include "base/win/win_util.h"
#include "chrome/common/chrome_constants.h"
#include "chrome/install_static/initialize_from_primary_module.h"
#include "chrome/install_static/install_details.h"
#include "chrome/install_static/install_util.h"
#include "chrome/install_static/product_install_details.h"
#include "content/public/app/sandbox_helper_win.h"
#include "sandbox/win/src/sandbox_types.h"

#include "chrome_elf/chrome_elf_main.h"
#include "components/crash/content/app/crash_switches.h"
#include "components/crash/content/app/crashpad.h"
#include "components/crash/content/app/fallback_crash_handling_win.h"
#include "components/crash/content/app/run_as_crashpad_handler_win.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/result_codes.h"

#include "chrome_elf/crash/crash_helper.h"
#include "muon/app/muon_crash_reporter_client.h"

#endif  // defined(OS_WIN)

#if defined(OS_MACOSX)
extern "C" {
__attribute__((visibility("default")))
int ChromeMain(int argc, const char* argv[]);
}
#endif  // OS_MACOSX

#if defined(OS_WIN)
int APIENTRY wWinMain(HINSTANCE instance, HINSTANCE, wchar_t* cmd, int) {
  int argc = 0;
  wchar_t** argv_setup = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
#else  // OS_WIN
#if defined(OS_MACOSX)
int ChromeMain(int argc, const char* argv[]) {
#else  // OS_MACOSX
int main(int argc, const char* argv[]) {
#endif
  char** argv_setup = uv_setup_args(argc, const_cast<char**>(argv));
#endif  // OS_WIN
  int64_t exe_entry_point_ticks = 0;
  // const base::TimeTicks exe_entry_point_ticks = base::TimeTicks::Now();

  base::CommandLine::Init(argc, argv_setup);

#if defined(OS_WIN)
  install_static::InitializeFromPrimaryModule();
  MuonCrashReporterClient::InitCrashReporting();
  SignalInitializeCrashReporting();
#endif

  // Initialize the CommandLine singleton from the environment.

  base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();

  base::FilePath user_data_dir;

  if (!command_line->HasSwitch(switches::kUserDataDir)) {
    LOG(ERROR) << "no user data dir";
    // first check the env
    std::string user_data_dir_string;
    std::unique_ptr<base::Environment> environment(base::Environment::Create());
    if (environment->GetVar("BRAVE_USER_DATA_DIR", &user_data_dir_string) &&
        base::IsStringUTF8(user_data_dir_string)) {
      user_data_dir = base::FilePath::FromUTF8Unsafe(user_data_dir_string);
      command_line->AppendSwitchPath(switches::kUserDataDir, user_data_dir);
    }

    // next check the user-data-dir switch
    if (user_data_dir.empty() &&
        command_line->HasSwitch("user-data-dir-name")) {
      LOG(ERROR) << "switch value ascii " << command_line->GetSwitchValueASCII("user-data-dir-name");
      LOG(ERROR) << "switch value path " << command_line->GetSwitchValuePath("user-data-dir-name").value();
      user_data_dir =
        command_line->GetSwitchValuePath("user-data-dir-name");
        LOG(ERROR) << "has name '" << user_data_dir.AsUTF8Unsafe() << "'";
      if (!user_data_dir.empty() && !user_data_dir.IsAbsolute()) {
        LOG(ERROR) << "not abnsolute";
        base::FilePath app_data_dir;
        brave::GetDefaultAppDataDirectory(&app_data_dir);
        user_data_dir = app_data_dir.Append(user_data_dir);
        command_line->AppendSwitchPath(switches::kUserDataDir, user_data_dir);
      }
    }
  }

#if defined(OS_WIN)
  // base::CommandLine::Init(0, nullptr);

  const std::string process_type =
      command_line->GetSwitchValueASCII(switches::kProcessType);

  // Confirm that an explicit prefetch profile is used for all process types
  // except for the browser process. Any new process type will have to assign
  // itself a prefetch id. See kPrefetchArgument* constants in
  // content_switches.cc for details.
  // DCHECK(process_type.empty() ||
  //        HasValidWindowsPrefetchArgument(*command_line));

  if (process_type == crash_reporter::switches::kCrashpadHandler) {
    crash_reporter::SetupFallbackCrashHandling(*command_line);
    return crash_reporter::RunAsCrashpadHandler(
        *base::CommandLine::ForCurrentProcess(), switches::kProcessType);
  } else if (process_type == crash_reporter::switches::kFallbackCrashHandler) {
    // return RunFallbackCrashHandler(*command_line);
  }

  // Signal Chrome Elf that Chrome has begun to start.
  SignalChromeElf();

  // Initialize the sandbox services.
  sandbox::SandboxInterfaceInfo sandbox_info = {0};
  const bool is_browser = process_type.empty();
  const bool is_sandboxed = !command_line->HasSwitch(switches::kNoSandbox);
  if (is_browser || is_sandboxed) {
    // For child processes that are running as --no-sandbox, don't initialize
    // the sandbox info, otherwise they'll be treated as brokers (as if they
    // were the browser).
    content::InitializeSandboxInfo(&sandbox_info);
  }

  // ::SetProcessShutdownParameters(0x280 - 1, SHUTDOWN_NORETRY); ??
#endif
  // The exit manager is in charge of calling the dtors of singletons.
  base::AtExitManager exit_manager;

  atom::AtomMainDelegate chrome_main_delegate(
      base::TimeTicks::FromInternalValue(exe_entry_point_ticks));
  content::ContentMainParams params(&chrome_main_delegate);

#if defined(OS_WIN)
  // The process should crash when going through abnormal termination.
  base::win::SetShouldCrashOnProcessDetach(true);
  base::win::SetAbortBehaviorForCrashReporting();
  params.instance = instance;
  params.sandbox_info = &sandbox_info;

  // SetDumpWithoutCrashingFunction must be passed the DumpProcess function
  // from chrome_elf and not from the DLL in order for DumpWithoutCrashing to
  // function correctly.
  typedef void (__cdecl *DumpProcessFunction)();
  DumpProcessFunction DumpProcess = reinterpret_cast<DumpProcessFunction>(
      ::GetProcAddress(::GetModuleHandle(chrome::kChromeElfDllName),
                       "DumpProcessWithoutCrash"));
  CHECK(DumpProcess);
  base::debug::SetDumpWithoutCrashingFunction(DumpProcess);

  atom::AtomCommandLine::InitW(argc, argv_setup);
#else
  params.argc = argc;
  params.argv = argv;
  atom::AtomCommandLine::Init(argc, argv_setup);
  // base::CommandLine::Init(params.argc, params.argv);
#endif  // defined(OS_WIN)
  // base::CommandLine::Init(0, nullptr);
  // command_line = base::CommandLine::ForCurrentProcess();
  // ALLOW_UNUSED_LOCAL(command_line);

  // if (command_line->HasSwitch(switches::kHeadless)) {
// #if defined(OS_MACOSX)
//     SetUpBundleOverrides();
// #endif
    // return headless::HeadlessShellMain(params);
  // }
// #endif  // defined(OS_LINUX) || defined(OS_MACOSX) || defined(OS_WIN)

  int rv = content::ContentMain(params);

#if defined(OS_WIN)
  base::win::SetShouldCrashOnProcessDetach(false);
#endif

  return rv;
}
