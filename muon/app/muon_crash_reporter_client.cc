// Copyright 2017 The Brave Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "muon/app/muon_crash_reporter_client.h"

#include "atom/common/atom_version.h"
#include "base/base_switches.h"
#include "base/command_line.h"
#include "base/debug/crash_logging.h"
#include "base/debug/leak_annotations.h"
#include "base/path_service.h"
#include "chrome/browser/browser_process_impl.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/chrome_paths.h"
#include "chrome/common/crash_keys.h"
#include "components/crash/content/app/crashpad.h"
#include "components/metrics/metrics_pref_names.h"
#include "components/metrics/metrics_service_accessor.h"
#include "components/prefs/pref_service.h"
#include "content/public/common/content_switches.h"

#if defined(OS_WIN)
#include "base/format_macros.h"
#include "base/path_service.h"
#include "base/files/file_path.h"
#include "chrome/install_static/install_util.h"
#include "chrome/install_static/user_data_dir.h"
#include "components/crash/content/app/crash_switches.h"
#include "chrome/common/chrome_constants.h"
#elif defined(OS_LINUX)
#include "components/crash/content/app/breakpad_linux.h"
#endif

#if defined(OS_MACOSX) || defined(OS_WIN)
#include "components/crash/content/app/crashpad.h"
#endif

namespace {

#if defined(OS_WIN)
// Type for the function pointer to enable and disable crash reporting on
// windows. Needed because the function is loaded from chrome_elf.
typedef void (*SetUploadConsentPointer)(bool);

// The name of the function used to set the uploads enabled state in
// components/crash/content/app/crashpad.cc. This is used to call the function
// exported by the chrome_elf dll.
const char kCrashpadUpdateConsentFunctionName[] = "SetUploadConsentImpl";
#endif  // OS_WIN

}

using metrics::MetricsServiceAccessor;

MuonCrashReporterClient::MuonCrashReporterClient() {
}

MuonCrashReporterClient::~MuonCrashReporterClient() {}

#if defined(OS_POSIX) && !defined(OS_MACOSX)
void MuonCrashReporterClient::GetProductNameAndVersion(
    const char** product_name,
    const char** product_version) {
  auto command_line = base::CommandLine::ForCurrentProcess();
  std::string name = ATOM_PRODUCT_NAME;
  if (command_line->HasSwitch("muon_product_name"))
    name = command_line->GetSwitchValueASCII("muon_product_name");

  std::string version = ATOM_VERSION_STRING;
  if (command_line->HasSwitch("muon_product_version"))
    version = command_line->GetSwitchValueASCII("muon_product_version");

  *product_name = name.c_str();
  *product_version = version.c_str();
}
#endif

#if defined(OS_WIN) || defined(OS_MACOSX)
bool MuonCrashReporterClient::ShouldMonitorCrashHandlerExpensively() {
  return false;
}

bool MuonCrashReporterClient::ReportingIsEnforcedByPolicy(
    bool* breakpad_enabled) {
  return false;
}
#endif

bool MuonCrashReporterClient::GetCollectStatsConsent() {
  return IsCrashReportingEnabled();
}

bool MuonCrashReporterClient::GetCollectStatsInSample() {
  return IsCrashReportingEnabled();
}

//  static
void MuonCrashReporterClient::InitCrashReporting() {
  static MuonCrashReporterClient* instance = nullptr;

  if (instance)
    return;

  instance = new MuonCrashReporterClient();
  ANNOTATE_LEAKING_OBJECT_PTR(instance);
  crash_reporter::SetCrashReporterClient(instance);

  auto command_line = base::CommandLine::ForCurrentProcess();

#if defined(OS_MACOSX)
  std::string process_type = command_line->GetSwitchValueASCII(
      ::switches::kProcessType);

  const bool install_from_dmg_relauncher_process =
      process_type == switches::kRelauncherProcess &&
      command_line->HasSwitch(switches::kRelauncherProcessDMGDevice);
  const bool browser_process = process_type.empty();

  const bool initial_client =
      browser_process || install_from_dmg_relauncher_process;

  crash_reporter::InitializeCrashpad(initial_client, process_type);
#elif defined(OS_WIN)
  std::wstring process_type = install_static::GetSwitchValueFromCommandLine(
      ::GetCommandLine(), install_static::kProcessType);
  crash_reporter::InitializeCrashpadWithEmbeddedHandler(
      process_type.empty(), install_static::UTF16ToUTF8(process_type));
#else
  breakpad::InitCrashReporter(process_type);
#endif

  SetCrashReportingEnabledForProcess(true);
  crash_keys::SetCrashKeysFromCommandLine(*command_line);
}

//  static
void MuonCrashReporterClient::SetCrashReportingEnabled(bool enabled) {
  CHECK(g_browser_process && g_browser_process->local_state());
  bool current_enabled_state = g_browser_process->local_state()->GetBoolean(
      metrics::prefs::kMetricsReportingEnabled);
  if (current_enabled_state == enabled)
    return;

  g_browser_process->local_state()->SetBoolean(
      metrics::prefs::kMetricsReportingEnabled, enabled);
  SetCrashReportingEnabledForProcess(enabled);
  if (enabled) {
    InitCrashReporting();
  }
}

//  static
void MuonCrashReporterClient::SetCrashKeyValue(const std::string& key,
                                                const std::string& value) {
  base::debug::SetCrashKeyValue(
      base::StringPiece(key), base::StringPiece(value));
}

//  static
void MuonCrashReporterClient::SetCrashReportingEnabledForProcess(bool enabled) {
  auto command_line = base::CommandLine::ForCurrentProcess();
  if (enabled) {
    command_line->AppendSwitch(switches::kEnableCrashReporter);
  } else {
    command_line->AppendSwitch(switches::kDisableBreakpad);
  }

#if defined(OS_MACOSX) || defined(OS_WIN)
  crash_reporter::SetUploadConsent(enabled);
#endif

#if defined(OS_WIN)
  install_static::SetCollectStatsInSample(enabled);

  // Next, get Crashpad to pick up the sampling state for this session.

  // The crash reporting is handled by chrome_elf.dll.
  HMODULE elf_module = GetModuleHandle(chrome::kChromeElfDllName);
  static SetUploadConsentPointer set_upload_consent =
      reinterpret_cast<SetUploadConsentPointer>(
          GetProcAddress(elf_module, kCrashpadUpdateConsentFunctionName));

  if (set_upload_consent) {
    // Crashpad will use the kRegUsageStatsInSample registry value to apply
    // sampling correctly, but may_record already reflects the sampling state.
    // This isn't a problem though, since they will be consistent.
    set_upload_consent(enabled);
  }
#endif
}


//  static
void MuonCrashReporterClient::AppendExtraCommandLineSwitches(
    base::CommandLine* command_line) {
  if (IsCrashReportingEnabled()) {
    command_line->AppendSwitch(switches::kEnableCrashReporter);
  }
}

//  static
void MuonCrashReporterClient::InitForProcess() {
  auto command_line = base::CommandLine::ForCurrentProcess();
  std::string process_type = command_line->GetSwitchValueASCII(
      ::switches::kProcessType);

  if (process_type.empty()) {
    return;
  }
#if defined(OS_POSIX) && !defined(OS_MACOSX)
  if (process_type == switches::kZygoteProcess)
    return;
#elif defined(OS_WIN)
  if (process_type == crash_reporter::switches::kCrashpadHandler) {
    return;
  }
#endif

  InitCrashReporting();
}

//  static
bool MuonCrashReporterClient::IsCrashReportingEnabled() {
  if (g_browser_process) {
    return g_browser_process->local_state()->GetBoolean(
        metrics::prefs::kMetricsReportingEnabled);
  }
  auto command_line = base::CommandLine::ForCurrentProcess();
  return !command_line->HasSwitch(switches::kDisableBreakpad) &&
      command_line->HasSwitch(switches::kEnableCrashReporter);
}
