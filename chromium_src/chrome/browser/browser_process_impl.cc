// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/browser_process_impl.h"

#include "atom/browser/api/atom_api_app.h"
#include "atom/browser/atom_resource_dispatcher_host_delegate.h"
#include "base/base_switches.h"
#include "base/command_line.h"
#include "base/metrics/field_trial.h"
#include "base/path_service.h"
#include "base/threading/thread.h"
#include "base/threading/thread_restrictions.h"
#include "brave/browser/brave_content_browser_client.h"
#include "brave/browser/component_updater/brave_component_updater_configurator.h"
#include "brave/browser/memory/guest_tab_manager.h"
#include "chrome/browser/background/background_mode_manager.h"
#include "chrome/browser/browser_shutdown.h"
#include "chrome/browser/prefs/chrome_command_line_pref_store.h"
#include "chrome/browser/printing/print_job_manager.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/shell_integration.h"
#include "chrome/browser/status_icons/status_tray.h"
#include "chrome/common/chrome_features.h"
#include "chrome/common/chrome_paths.h"
#include "components/component_updater/component_updater_service.h"
// #include "components/crash/content/app/crashpad.h"
#include "components/metrics/metrics_pref_names.h"
#include "components/password_manager/core/browser/password_manager.h"
#include "components/prefs/json_pref_store.h"
#include "components/prefs/pref_filter.h"
#include "components/sync_preferences/pref_service_syncable_factory.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/child_process_security_policy.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/browser/resource_dispatcher_host.h"
#include "content/public/common/content_switches.h"
#include "muon/app/muon_crash_reporter_client.h"
#include "ppapi/features/features.h"
#include "ui/base/idle/idle.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/message_center/message_center.h"

#if BUILDFLAG(ENABLE_EXTENSIONS)
#include "atom/browser/extensions/atom_extensions_browser_client.h"
#include "chrome/browser/extensions/event_router_forwarder.h"
#include "chrome/common/extensions/extension_process_policy.h"
#include "chrome/common/extensions/chrome_extensions_client.h"
#include "components/storage_monitor/storage_monitor.h"
#include "extensions/common/constants.h"
#include "extensions/common/features/feature_provider.h"
#include "ui/base/resource/resource_bundle.h"
#endif

#if BUILDFLAG(ENABLE_PLUGINS)
#include "brave/browser/plugins/brave_plugin_service_filter.h"
#include "chrome/browser/plugins/plugin_finder.h"
#include "content/public/browser/plugin_service.h"
#endif

#if defined(OS_POSIX) && !defined(OS_MACOSX)
#include "components/crash/content/app/breakpad_linux.h"
#endif

#if defined(OS_WIN)
#include "components/crash/content/app/breakpad_win.h"
#endif

#if defined(USE_X11) || defined(OS_WIN) || defined(USE_OZONE)
// How long to wait for the File thread to complete during EndSession, on Linux
// and Windows. We have a timeout here because we're unable to run the UI
// messageloop and there's some deadlock risk. Our only option is to exit
// anyway.
static constexpr base::TimeDelta kEndSessionTimeout =
    base::TimeDelta::FromSeconds(10);
#endif

using content::ChildProcessSecurityPolicy;
using content::PluginService;

BrowserProcessImpl::BrowserProcessImpl(
      base::SequencedTaskRunner* local_state_task_runner) :
    local_state_task_runner_(local_state_task_runner),
    tearing_down_(false),
    created_profile_manager_(false),
    created_local_state_(false),
    locale_(l10n_util::GetApplicationLocale("")) {
  g_browser_process = this;

  print_job_manager_.reset(new printing::PrintJobManager);

#if defined(OS_MACOSX)
  ui::InitIdleMonitor();
#endif

#if BUILDFLAG(ENABLE_EXTENSIONS)
  extension_event_router_forwarder_ = new extensions::EventRouterForwarder;

  extensions::ExtensionsClient::Set(
      extensions::ChromeExtensionsClient::GetInstance());
  extensions_browser_client_.reset(
      new extensions::AtomExtensionsBrowserClient());
  extensions::ExtensionsBrowserClient::Set(extensions_browser_client_.get());
#endif

  message_center::MessageCenter::Initialize();
}

BrowserProcessImpl::~BrowserProcessImpl() {
#if BUILDFLAG(ENABLE_EXTENSIONS)
  extensions::ExtensionsBrowserClient::Set(nullptr);
#endif
  g_browser_process = NULL;
}

void BrowserProcessImpl::CreateProfileManager() {
  DCHECK(!created_profile_manager_ && !profile_manager_);
  created_profile_manager_ = true;

  base::FilePath user_data_dir;
  PathService::Get(chrome::DIR_USER_DATA, &user_data_dir);
  profile_manager_.reset(new ProfileManager(user_data_dir));
}

ProfileManager* BrowserProcessImpl::profile_manager() {
  DCHECK(thread_checker_.CalledOnValidThread());
  if (!created_profile_manager_)
    CreateProfileManager();
  return profile_manager_.get();
}

rappor::RapporServiceImpl* BrowserProcessImpl::rappor_service() {
  return nullptr;
}

ukm::UkmRecorder* BrowserProcessImpl::ukm_recorder() {
  return nullptr;
}

component_updater::ComponentUpdateService*
BrowserProcessImpl::component_updater(
    std::unique_ptr<component_updater::ComponentUpdateService> &component_updater,
    bool use_brave_server) {
  if (!component_updater.get()) {
    if (!content::BrowserThread::CurrentlyOn(content::BrowserThread::UI))
      return NULL;
    Profile* profile = ProfileManager::GetPrimaryUserProfile();
    scoped_refptr<update_client::Configurator> configurator =
        component_updater::MakeBraveComponentUpdaterConfigurator(
            base::CommandLine::ForCurrentProcess(),
            profile->GetRequestContext(),
            use_brave_server);
    // Creating the component updater does not do anything, components
    // need to be registered and Start() needs to be called.
    component_updater.reset(component_updater::ComponentUpdateServiceFactory(
                                 configurator).release());
  }
  return component_updater.get();
}

component_updater::ComponentUpdateService*
BrowserProcessImpl::brave_component_updater() {
  return component_updater(brave_component_updater_, true);
}

component_updater::ComponentUpdateService*
BrowserProcessImpl::component_updater() {
  return component_updater(component_updater_, false);
}

extensions::EventRouterForwarder*
BrowserProcessImpl::extension_event_router_forwarder() {
  return extension_event_router_forwarder_.get();
}

message_center::MessageCenter* BrowserProcessImpl::message_center() {
  DCHECK(thread_checker_.CalledOnValidThread());
  return message_center::MessageCenter::Get();
}

void BrowserProcessImpl::StartTearDown() {
  TRACE_EVENT0("shutdown", "BrowserProcessImpl::StartTearDown");
  // TODO(crbug.com/560486): Fix the tests that make the check of
  // |tearing_down_| necessary in IsShuttingDown().
  tearing_down_ = true;
  DCHECK(IsShuttingDown());

  browser_shutdown::SetTryingToQuit(true);

  for (content::RenderProcessHost::iterator i(
          content::RenderProcessHost::AllHostsIterator());
       !i.IsAtEnd(); i.Advance()) {
    i.GetCurrentValue()->FastShutdownIfPossible();
  }

  print_job_manager_->Shutdown();

  profile_manager_.reset();

  message_center::MessageCenter::Shutdown();

  if (local_state()) {
    local_state()->CommitPendingWrite();
  }
}

void BrowserProcessImpl::SetApplicationLocale(const std::string& locale) {
  locale_ = locale;
  brave::BraveContentBrowserClient::SetApplicationLocale(locale);
}

const std::string& BrowserProcessImpl::GetApplicationLocale() {
  DCHECK(!locale_.empty());
  return locale_;
}

printing::PrintJobManager* BrowserProcessImpl::print_job_manager() {
  return print_job_manager_.get();
}

void BrowserProcessImpl::CreateLocalState() {
  DCHECK(!created_local_state_ && !local_state_);
  created_local_state_ = true;

  base::FilePath local_state_path;
  CHECK(PathService::Get(chrome::FILE_LOCAL_STATE, &local_state_path));
  scoped_refptr<PrefRegistrySimple> pref_registry = new PrefRegistrySimple;

  // metrics::MetricsService::RegisterPrefs(registry);
  // metrics::StabilityMetricsHelper::RegisterPrefs(registry);
  // metrics::RegisterMetricsReportingStatePrefs(registry);

// #if BUILDFLAG(ENABLE_PLUGINS)
//   PluginMetricsProvider::RegisterPrefs(registry);
// #endif  // BUILDFLAG(ENABLE_PLUGINS)
//   browser_shutdown::RegisterPrefs(registry);
  // variations::VariationsService::RegisterPrefs(registry);
  // component_updater::RegisterPrefs(registry);
  // ExternalProtocolHandler::RegisterPrefs(registry);
  // geolocation::RegisterPrefs(registry);
  // network_time::NetworkTimeTracker::RegisterPrefs(registry);
  // ssl_config::SSLConfigServiceManager::RegisterPrefs(registry);
  // subresource_filter::IndexedRulesetVersion::RegisterPrefs(registry);
  // startup_metric_utils::RegisterPrefs(registry);
  // update_client::RegisterPrefs(registry);

// #if BUILDFLAG(ENABLE_PLUGINS)
//   PluginFinder::RegisterPrefs(registry);
//   PluginsResourceService::RegisterPrefs(registry);
// #endif

//   task_manager::TaskManagerInterface::RegisterPrefs(registry);
//   gcm::GCMChannelStatusSyncer::RegisterPrefs(registry);
//   gcm::RegisterPrefs(registry);

#if defined(OS_WIN)
  password_manager::PasswordManager::RegisterLocalPrefs(pref_registry.get());
#endif
  pref_registry->RegisterBooleanPref(
      metrics::prefs::kMetricsReportingEnabled, false);

  sync_preferences::PrefServiceSyncableFactory factory;
  factory.set_async(false);
  factory.set_user_prefs(
      new JsonPrefStore(local_state_path,
                        local_state_task_runner_.get(),
                        std::unique_ptr<PrefFilter>()));
  local_state_ = factory.Create(pref_registry.get());
}

bool BrowserProcessImpl::created_local_state() const {
  return created_local_state_;
}

PrefService* BrowserProcessImpl::local_state() {
  DCHECK(thread_checker_.CalledOnValidThread());
  if (!created_local_state_)
    CreateLocalState();
  return local_state_.get();
}

bool BrowserProcessImpl::IsShuttingDown() {
  return tearing_down_;
}

void BrowserProcessImpl::ResourceDispatcherHostCreated() {
  resource_dispatcher_host_delegate_.reset(
      new atom::AtomResourceDispatcherHostDelegate);
  content::ResourceDispatcherHost::Get()->SetDelegate(
      resource_dispatcher_host_delegate_.get());
}

void BrowserProcessImpl::PreCreateThreads() {
#if BUILDFLAG(ENABLE_EXTENSIONS)
  // Disable Isolate Extensions by default
  ChildProcessSecurityPolicy::GetInstance()->RegisterWebSafeScheme(
      extensions::kExtensionScheme);
#endif
  auto command_line = base::CommandLine::ForCurrentProcess();

  // initialize local state
  local_state()->UpdateCommandLinePrefStore(
      new ChromeCommandLinePrefStore(command_line));
}

void BrowserProcessImpl::PreMainMessageLoopRun() {
  TRACE_EVENT0("startup", "BrowserProcessImpl::PreMainMessageLoopRun");

  // TODO(bridiver) ??
  // #if !defined(OS_ANDROID)
  //   ApplyMetricsReportingPolicy();
  // #endif

#if BUILDFLAG(ENABLE_PLUGINS)
  PluginService* plugin_service = PluginService::GetInstance();
  plugin_service->SetFilter(BravePluginServiceFilter::GetInstance());

  // Triggers initialization of the singleton instance on UI thread.
  PluginFinder::GetInstance()->Init();

  // TODO(bridiver) ??
  // DCHECK(!plugins_resource_service_);
  // plugins_resource_service_ =
  //     base::MakeUnique<PluginsResourceService>(local_state());
  // plugins_resource_service_->Init();

#endif  // BUILDFLAG(ENABLE_PLUGINS)

#if BUILDFLAG(ENABLE_EXTENSIONS)
  storage_monitor::StorageMonitor::Create();
#endif

  // TODO(bridiver) ??
  // child_process_watcher_.reset(new ChromeChildProcessWatcher());


// Start the tab manager here so that we give the most amount of time for the
// other services to start up before we start adjusting the oom priority.
#if defined(OS_WIN) || defined(OS_MACOSX) || defined(OS_LINUX)
  g_browser_process->GetTabManager()->Start();
#endif
}

void BrowserProcessImpl::PostDestroyThreads() {
  // With the file_thread_ flushed, we can release any icon resources.
//   icon_manager_.reset();

// #if BUILDFLAG(ENABLE_WEBRTC)
//   // Must outlive the file thread.
//   webrtc_log_uploader_.reset();
// #endif

  // Reset associated state right after actual thread is stopped,
  // as io_thread_.global_ cleanup happens in CleanUp on the IO
  // thread, i.e. as the thread exits its message loop.
  //
  // This is important also because in various places, the
  // IOThread object being NULL is considered synonymous with the
  // IO thread having stopped.
  // io_thread_.reset();
}

memory::TabManager* BrowserProcessImpl::GetTabManager() {
  DCHECK(thread_checker_.CalledOnValidThread());
#if defined(OS_WIN) || defined(OS_MACOSX) || defined(OS_LINUX)
  if (!tab_manager_.get())
    tab_manager_.reset(new memory::GuestTabManager());
  return tab_manager_.get();
#else
  return nullptr;
#endif
}

namespace {

// Used at the end of session to block the UI thread for completion of sentinel
// tasks on the set of threads used to persist profile data and local state.
// This is done to ensure that the data has been persisted to disk before
// continuing.
class RundownTaskCounter :
    public base::RefCountedThreadSafe<RundownTaskCounter> {
 public:
  RundownTaskCounter();

  // Posts a rundown task to |task_runner|, can be invoked an arbitrary number
  // of times before calling TimedWait.
  void Post(base::SequencedTaskRunner* task_runner);

  // Waits until the count is zero or |end_time| is reached.
  // This can only be called once per instance. Returns true if a count of zero
  // is reached or false if the |end_time| is reached. It is valid to pass an
  // |end_time| in the past.
  bool TimedWaitUntil(const base::TimeTicks& end_time);

 private:
  friend class base::RefCountedThreadSafe<RundownTaskCounter>;
  ~RundownTaskCounter() {}

  // Decrements the counter and releases the waitable event on transition to
  // zero.
  void Decrement();

  // The count starts at one to defer the possibility of one->zero transitions
  // until TimedWait is called.
  base::AtomicRefCount count_;
  base::WaitableEvent waitable_event_;

  DISALLOW_COPY_AND_ASSIGN(RundownTaskCounter);
};

RundownTaskCounter::RundownTaskCounter()
    : count_(1),
      waitable_event_(base::WaitableEvent::ResetPolicy::MANUAL,
                      base::WaitableEvent::InitialState::NOT_SIGNALED) {}

void RundownTaskCounter::Post(base::SequencedTaskRunner* task_runner) {
  // As the count starts off at one, it should never get to zero unless
  // TimedWait has been called.
  DCHECK(!base::AtomicRefCountIsZero(&count_));

  base::AtomicRefCountInc(&count_);

  // The task must be non-nestable to guarantee that it runs after all tasks
  // currently scheduled on |task_runner| have completed.
  task_runner->PostNonNestableTask(
      FROM_HERE, base::BindOnce(&RundownTaskCounter::Decrement, this));
}

void RundownTaskCounter::Decrement() {
  if (!base::AtomicRefCountDec(&count_))
    waitable_event_.Signal();
}

bool RundownTaskCounter::TimedWaitUntil(const base::TimeTicks& end_time) {
  // Decrement the excess count from the constructor.
  Decrement();

  return waitable_event_.TimedWaitUntil(end_time);
}

}  // namespace

void BrowserProcessImpl::EndSession() {
  // Mark all the profiles as clean.
  ProfileManager* pm = profile_manager();
  std::vector<Profile*> profiles(pm->GetLoadedProfiles());
  scoped_refptr<RundownTaskCounter> rundown_counter(new RundownTaskCounter());
  const bool pref_service_enabled =
      base::FeatureList::IsEnabled(features::kPrefService);
  std::vector<scoped_refptr<base::SequencedTaskRunner>> profile_writer_runners;
  for (size_t i = 0; i < profiles.size(); ++i) {
    Profile* profile = profiles[i];
    profile->SetExitType(Profile::EXIT_SESSION_ENDED);
    if (profile->GetPrefs()) {
      profile->GetPrefs()->CommitPendingWrite();
      if (pref_service_enabled) {
        rundown_counter->Post(content::BrowserThread::GetTaskRunnerForThread(
                                  content::BrowserThread::IO)
                                  .get());
        profile_writer_runners.push_back(profile->GetIOTaskRunner());
      } else {
        rundown_counter->Post(profile->GetIOTaskRunner().get());
      }
    }
  }

  if (local_state()) {
    local_state()->CommitPendingWrite();
    rundown_counter->Post(local_state_task_runner_.get());
  }

  // http://crbug.com/125207
  base::ThreadRestrictions::ScopedAllowWait allow_wait;

  // We must write that the profile and metrics service shutdown cleanly,
  // otherwise on startup we'll think we crashed. So we block until done and
  // then proceed with normal shutdown.
  //
  // If you change the condition here, be sure to also change
  // ProfileBrowserTests to match.
#if defined(USE_X11) || defined(OS_WIN) || defined(USE_OZONE)
  // Do a best-effort wait on the successful countdown of rundown tasks. Note
  // that if we don't complete "quickly enough", Windows will terminate our
  // process.
  //
  // On Windows, we previously posted a message to FILE and then ran a nested
  // message loop, waiting for that message to be processed until quitting.
  // However, doing so means that other messages will also be processed. In
  // particular, if the GPU process host notices that the GPU has been killed
  // during shutdown, it races exiting the nested loop with the process host
  // blocking the message loop attempting to re-establish a connection to the
  // GPU process synchronously. Because the system may not be allowing
  // processes to launch, this can result in a hang. See
  // http://crbug.com/318527.
  const base::TimeTicks end_time = base::TimeTicks::Now() + kEndSessionTimeout;
  const bool timed_out = !rundown_counter->TimedWaitUntil(end_time);
  if (timed_out || !pref_service_enabled)
    return;

  scoped_refptr<RundownTaskCounter> profile_write_rundown_counter(
      new RundownTaskCounter());
  for (auto& profile_writer_runner : profile_writer_runners)
    profile_write_rundown_counter->Post(profile_writer_runner.get());
  profile_write_rundown_counter->TimedWaitUntil(end_time);
#else
  NOTIMPLEMENTED();
#endif
}

net_log::ChromeNetLog* BrowserProcessImpl::net_log() {
  NOTIMPLEMENTED();
  return nullptr;
}

metrics_services_manager::MetricsServicesManager*
BrowserProcessImpl::GetMetricsServicesManager() {
  NOTIMPLEMENTED();
  return nullptr;
}

net::URLRequestContextGetter* BrowserProcessImpl::system_request_context() {
  NOTIMPLEMENTED();
  return nullptr;
}

variations::VariationsService* BrowserProcessImpl::variations_service() {
  NOTIMPLEMENTED();
  return nullptr;
}

BrowserProcessPlatformPart* BrowserProcessImpl::platform_part() {
  NOTIMPLEMENTED();
  return nullptr;
}

NotificationUIManager* BrowserProcessImpl::notification_ui_manager() {
  NOTIMPLEMENTED();
  return nullptr;
}

NotificationPlatformBridge* BrowserProcessImpl::notification_platform_bridge() {
  NOTIMPLEMENTED();
  return nullptr;
}

IOThread* BrowserProcessImpl::io_thread() {
  NOTIMPLEMENTED();
  return nullptr;
}

WatchDogThread* BrowserProcessImpl::watchdog_thread() {
  NOTIMPLEMENTED();
  return nullptr;
}

policy::BrowserPolicyConnector* BrowserProcessImpl::browser_policy_connector() {
  NOTIMPLEMENTED();
  return nullptr;
}

policy::PolicyService* BrowserProcessImpl::policy_service() {
  NOTIMPLEMENTED();
  return nullptr;
}

IconManager* BrowserProcessImpl::icon_manager() {
  NOTIMPLEMENTED();
  return nullptr;
}

GpuModeManager* BrowserProcessImpl::gpu_mode_manager() {
  NOTIMPLEMENTED();
  return nullptr;
}

GpuProfileCache* BrowserProcessImpl::gpu_profile_cache() {
  NOTIMPLEMENTED();
  return nullptr;
}

void BrowserProcessImpl::CreateDevToolsHttpProtocolHandler(
    const std::string& ip, uint16_t port) {
  NOTIMPLEMENTED();
}

void BrowserProcessImpl::CreateDevToolsAutoOpener() {
  NOTIMPLEMENTED();
}

printing::PrintPreviewDialogController*
BrowserProcessImpl::print_preview_dialog_controller() {
  NOTIMPLEMENTED();
  return nullptr;
}

printing::BackgroundPrintingManager*
BrowserProcessImpl::background_printing_manager() {
  NOTIMPLEMENTED();
  return nullptr;
}

IntranetRedirectDetector* BrowserProcessImpl::intranet_redirect_detector() {
  NOTIMPLEMENTED();
  return nullptr;
}

DownloadStatusUpdater* BrowserProcessImpl::download_status_updater() {
  NOTIMPLEMENTED();
  return nullptr;
}

DownloadRequestLimiter* BrowserProcessImpl::download_request_limiter() {
  NOTIMPLEMENTED();
  return nullptr;
}

BackgroundModeManager* BrowserProcessImpl::background_mode_manager() {
  NOTIMPLEMENTED();
  return nullptr;
}

void BrowserProcessImpl::set_background_mode_manager_for_test(
    std::unique_ptr<BackgroundModeManager> manager) {
  NOTIMPLEMENTED();
}

safe_browsing::SafeBrowsingService*
BrowserProcessImpl::safe_browsing_service() {
  return nullptr;
}

safe_browsing::ClientSideDetectionService*
BrowserProcessImpl::safe_browsing_detection_service() {
  return nullptr;
}

subresource_filter::ContentRulesetService*
BrowserProcessImpl::subresource_filter_ruleset_service() {
  NOTIMPLEMENTED();
  return nullptr;
}

CRLSetFetcher* BrowserProcessImpl::crl_set_fetcher() {
  NOTIMPLEMENTED();
  return nullptr;
}

component_updater::PnaclComponentInstaller*
BrowserProcessImpl::pnacl_component_installer() {
  NOTIMPLEMENTED();
  return nullptr;
}

component_updater::SupervisedUserWhitelistInstaller*
BrowserProcessImpl::supervised_user_whitelist_installer() {
  NOTIMPLEMENTED();
  return nullptr;
}

MediaFileSystemRegistry* BrowserProcessImpl::media_file_system_registry() {
  NOTIMPLEMENTED();
  return nullptr;
}

#if BUILDFLAG(ENABLE_WEBRTC)
WebRtcLogUploader* BrowserProcessImpl::webrtc_log_uploader() {
  NOTIMPLEMENTED();
  return nullptr;
}

#endif
network_time::NetworkTimeTracker* BrowserProcessImpl::network_time_tracker() {
  NOTIMPLEMENTED();
  return nullptr;
}

gcm::GCMDriver* BrowserProcessImpl::gcm_driver() {
  NOTIMPLEMENTED();
  return nullptr;
}

StatusTray* BrowserProcessImpl::status_tray() {
  NOTIMPLEMENTED();
  return nullptr;
}

shell_integration::DefaultWebClientState
BrowserProcessImpl::CachedDefaultWebClientState() {
  NOTIMPLEMENTED();
  return shell_integration::UNKNOWN_DEFAULT;
}

physical_web::PhysicalWebDataSource*
BrowserProcessImpl::GetPhysicalWebDataSource() {
  NOTIMPLEMENTED();
  return nullptr;
}

#if (defined(OS_WIN) || defined(OS_LINUX)) && !defined(OS_CHROMEOS)
void BrowserProcessImpl::StartAutoupdateTimer() {
  NOTIMPLEMENTED();
}
#endif
