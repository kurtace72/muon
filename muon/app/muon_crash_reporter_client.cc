// Copyright 2017 The Brave Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "muon/app/muon_crash_reporter_client.h"

#include "atom/common/atom_version.h"
#include "base/base_switches.h"
#include "base/command_line.h"
#include "base/debug/crash_logging.h"
#include "base/debug/leak_annotations.h"
#include "chrome/browser/browser_process_impl.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/crash_keys.h"
#include "components/crash/content/app/crashpad.h"
#include "components/metrics/metrics_pref_names.h"
#include "components/metrics/metrics_service_accessor.h"
#include "components/prefs/pref_service.h"
#include "content/public/common/content_switches.h"

#if defined(OS_WIN)
#include "base/path_service.h"
#include "base/files/file_path.h"
#include "chrome/common/chrome_paths.h"
#include "chrome/install_static/install_util.h"
#include "components/crash/content/app/crash_switches.h"
#elif defined(OS_LINUX)
#include "components/crash/content/app/breakpad_linux.h"
#endif

#if defined(OS_MACOSX) || defined(OS_WIN)
#include "components/crash/content/app/crashpad.h"
#endif

using metrics::MetricsServiceAccessor;

MuonCrashReporterClient::MuonCrashReporterClient() {
  crash_reporter::SetCrashReporterClient(this);
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

#if defined(OS_MACOSX)
bool MuonCrashReporterClient::ShouldMonitorCrashHandlerExpensively() {
  return false;
}
#endif

#if defined(OS_WIN) || defined(OS_MACOSX)
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
  if (!IsCrashReportingEnabled()) {
    LOG(ERROR) << "Crash reporting is disabled";
    return;
  } else {
    LOG(ERROR) << "enabling crash reporting";
  }

  auto command_line = base::CommandLine::ForCurrentProcess();
  std::string process_type = command_line->GetSwitchValueASCII(
      ::switches::kProcessType);

  crash_keys::SetCrashKeysFromCommandLine(*command_line);
  MuonCrashReporterClient* crash_client = new MuonCrashReporterClient();
  ANNOTATE_LEAKING_OBJECT_PTR(crash_client);

#if defined(OS_MACOSX)
  const bool install_from_dmg_relauncher_process =
      process_type == switches::kRelauncherProcess &&
      command_line->HasSwitch(switches::kRelauncherProcessDMGDevice);
  const bool browser_process = process_type.empty();

  const bool initial_client =
      browser_process || install_from_dmg_relauncher_process;

  crash_reporter::InitializeCrashpad(initial_client, process_type);
#elif defined(OS_WIN)
  base::FilePath user_data_dir;
  if (!PathService::Get(chrome::DIR_USER_DATA, &user_data_dir))
    return;

  LOG(ERROR) << user_data_dir.MaybeAsASCII();
  crash_reporter::InitializeCrashpadWithEmbeddedHandler(
      process_type.empty(), process_type,
      install_static::UTF16ToUTF8(user_data_dir.value()));
#else
  breakpad::InitCrashReporter(process_type);
#endif

  SetCrashReportingEnabledForProcess(true);
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

#if defined(OS_MACOSX) || defined (OS_WIN)
  crash_reporter::SetUploadConsent(enabled);
#endif
}

//  static
void MuonCrashReporterClient::AppendExtraCommandLineSwitches(
    base::CommandLine* command_line) {
  if (IsCrashReportingEnabled()) {
    LOG(ERROR) << "enable crash reporting for process";
    command_line->AppendSwitch(switches::kEnableCrashReporter);
  }
}

//  static
void MuonCrashReporterClient::InitForProcess() {
  auto command_line = base::CommandLine::ForCurrentProcess();
  std::string process_type = command_line->GetSwitchValueASCII(
      ::switches::kProcessType);

  // browser process initializes in BrowserProcessImpl::PreCreateThreads
  if (process_type.empty())
    return;
#if defined(OS_POSIX) && !defined(OS_MACOSX)
  if (process_type == switches::kZygoteProcess)
    return;
#elif defined(OS_WIN)
  if (process_type == crash_reporter::switches::kCrashpadHandler)
    return;
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
