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


#include "chrome/common/chrome_switches.h"
#include "chrome_elf/chrome_elf_main.h"
#include "components/crash/content/app/crash_switches.h"
#include "components/crash/content/app/crashpad.h"
#include "components/crash/content/app/fallback_crash_handling_win.h"
#include "components/crash/content/app/run_as_crashpad_handler_win.h"
#include "content/public/common/content_switches.h"
#include "content/public/common/result_codes.h"
#include "chrome_elf/chrome_elf_main.h"

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

#if defined(OS_WIN)
  install_static::InitializeFromPrimaryModule();
  SignalInitializeCrashReporting();

  // Initialize the CommandLine singleton from the environment.
  base::CommandLine::Init(0, nullptr);
  const base::CommandLine* command_line =
      base::CommandLine::ForCurrentProcess();

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
#endif

  atom::AtomMainDelegate chrome_main_delegate(
      base::TimeTicks::FromInternalValue(exe_entry_point_ticks));
  content::ContentMainParams params(&chrome_main_delegate);

#if defined(OS_WIN)
  sandbox::SandboxInterfaceInfo sandbox_info = {0};
  content::InitializeSandboxInfo(&sandbox_info);

  // The process should crash when going through abnormal termination.
  base::win::SetShouldCrashOnProcessDetach(true);
  base::win::SetAbortBehaviorForCrashReporting();
  // SetDumpWithoutCrashingFunction must be passed the DumpProcess function
  // from chrome_elf and not from the DLL in order for DumpWithoutCrashing to
  // function correctly.
  typedef void (__cdecl *DumpProcessFunction)();
  DumpProcessFunction DumpProcess = reinterpret_cast<DumpProcessFunction>(
      ::GetProcAddress(::GetModuleHandle(chrome::kChromeElfDllName),
                       "DumpProcessWithoutCrash"));
  CHECK(DumpProcess);
  base::debug::SetDumpWithoutCrashingFunction(DumpProcess);

  params.instance = instance;
  params.sandbox_info = &sandbox_info;
  atom::AtomCommandLine::InitW(argc, argv_setup);
#else
  params.argc = argc;
  params.argv = argv;
  atom::AtomCommandLine::Init(argc, argv_setup);
#endif

  base::AtExitManager exit_manager;

  int rv = content::ContentMain(params);

#if defined(OS_WIN)
  base::win::SetShouldCrashOnProcessDetach(false);
#endif

  return rv;
}
