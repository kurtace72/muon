// Copyright (c) 2013 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "atom/browser/atom_browser_main_parts.h"

#include "atom/browser/api/trackable_object.h"
#include "atom/browser/atom_access_token_store.h"
#include "atom/browser/atom_browser_client.h"
#include "atom/browser/atom_browser_context.h"
#include "atom/browser/bridge_task_runner.h"
#include "atom/browser/browser.h"
#include "atom/browser/browser_context_keyed_service_factories.h"
#include "atom/browser/javascript_environment.h"
#include "atom/common/api/atom_bindings.h"
#include "atom/common/node_bindings.h"
#include "atom/common/node_includes.h"
#include "base/allocator/allocator_extension.h"
#include "base/command_line.h"
#include "base/feature_list.h"
#include "base/files/file_util.h"
#include "base/memory/memory_pressure_monitor.h"
#include "base/path_service.h"
#include "base/profiler/scoped_tracker.h"
#include "base/profiler/stack_sampling_profiler.h"
#include "base/threading/thread_task_runner_handle.h"
#include "base/time/default_tick_clock.h"
#include "browser/media/media_capture_devices_dispatcher.h"
#include "chrome/browser/browser_process_impl.h"
#include "chrome/browser/browser_shutdown.h"
// #include "chrome/browser/chrome_process_singleton.h"
#include "chrome/browser/profiles/profile_manager.h"
#include "chrome/browser/ui/webui/chrome_web_ui_controller_factory.h"
#include "chrome/common/chrome_constants.h"
#include "chrome/common/chrome_paths.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/common/chrome_result_codes.h"
#include "components/metrics/profiler/content/content_tracking_synchronizer_delegate.h"
#include "components/metrics/profiler/tracking_synchronizer.h"
#include "components/prefs/json_pref_store.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/child_process_security_policy.h"
#include "content/public/browser/web_ui_controller_factory.h"
#include "content/public/common/content_switches.h"
#include "device/geolocation/geolocation_delegate.h"
#include "device/geolocation/geolocation_provider.h"
#include "muon/app/muon_crash_reporter_client.h"
#include "v8/include/v8.h"
#include "v8/include/v8-debug.h"

#if defined(OS_MACOSX)
#include <Security/Security.h>
#endif  // defined(OS_MACOSX)


#if defined(USE_X11)
#include "chrome/browser/ui/libgtkui/gtk_util.h"
#include "ui/events/devices/x11/touch_factory_x11.h"
#include "ui/views/linux_ui/linux_ui.h"
#endif

#if defined(USE_AURA)
#include "chrome/browser/lifetime/application_lifetime.h"
#include "content/public/common/service_manager_connection.h"
#include "services/service_manager/runner/common/client_util.h"
#include "ui/display/display.h"
#include "ui/display/screen.h"
#include "ui/views/widget/desktop_aura/desktop_screen.h"
#endif

#if defined(OS_WIN)
#include "base/win/pe_image.h"
#include "base/win/registry.h"
#include "base/win/win_util.h"
#include "base/win/windows_version.h"
#include "base/win/wrapped_window_proc.h"
// #include "chrome/browser/win/chrome_elf_init.h"
#endif



// #if defined(USE_AURA)
//
// #endif

// #if defined(TOOLKIT_VIEWS)
// #include "browser/views/views_delegate.h"
// #include "chrome/browser/ui/views/harmony/harmony_layout_provider.h"
// #endif

// #if defined(USE_X11)
// #include "base/environment.h"
// #include "base/path_service.h"
// #include "base/nix/xdg_util.h"
// #include "base/threading/thread_task_runner_handle.h"
// #include "chrome/browser/ui/libgtkui/gtk_ui.h"
// #include "ui/base/x/x11_util.h"
// #include "ui/base/x/x11_util_internal.h"

// #include "ui/wm/core/wm_state.h"
// #endif



namespace atom {

namespace {

#if defined(OS_MACOSX)
OSStatus KeychainCallback(SecKeychainEvent keychain_event,
                          SecKeychainCallbackInfo* info, void* context) {
  return noErr;
}
#endif  // defined(OS_MACOSX)

#if defined(OS_WIN)
void InitializeWindowProcExceptions() {
  // Get the breakpad pointer
  base::win::WinProcExceptionFilter exception_filter =
      reinterpret_cast<base::win::WinProcExceptionFilter>(::GetProcAddress(
          ::GetModuleHandle(chrome::kChromeElfDllName), "CrashForException"));
  CHECK(exception_filter);
  exception_filter = base::win::SetWinProcExceptionFilter(exception_filter);
  DCHECK(!exception_filter);
}
#endif  // defined (OS_WIN)

}  // namespace

// A provider of Geolocation services to override AccessTokenStore.
class AtomGeolocationDelegate : public device::GeolocationDelegate {
 public:
  AtomGeolocationDelegate() = default;

  scoped_refptr<device::AccessTokenStore> CreateAccessTokenStore() final {
    return new AtomAccessTokenStore();
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(AtomGeolocationDelegate);
};

template<typename T>
void Erase(T* container, typename T::iterator iter) {
  container->erase(iter);
}

// static
AtomBrowserMainParts* AtomBrowserMainParts::self_ = nullptr;

AtomBrowserMainParts::AtomBrowserMainParts(
    const content::MainFunctionParams& parameters)
    : exit_code_(nullptr),
      parameters_(parameters),
      parsed_command_line_(parameters.command_line),
      browser_(new Browser),
      node_bindings_(NodeBindings::Create()),
      atom_bindings_(new AtomBindings),
      gc_timer_(true, true) {
  DCHECK(!self_) << "Cannot have two AtomBrowserMainParts";
  self_ = this;
}

AtomBrowserMainParts::~AtomBrowserMainParts() {
  // Leak the JavascriptEnvironment on exit.
  // This is to work around the bug that V8 would be waiting for background
  // tasks to finish on exit, while somehow it waits forever in Electron, more
  // about this can be found at https://github.com/electron/electron/issues/4767.
  // On the other handle there is actually no need to gracefully shutdown V8
  // on exit in the main process, we already ensured all necessary resources get
  // cleaned up, and it would make quitting faster.
  ignore_result(js_env_.release());
}

// static
AtomBrowserMainParts* AtomBrowserMainParts::Get() {
  DCHECK(self_);
  return self_;
}

bool AtomBrowserMainParts::SetExitCode(int code) {
  if (!exit_code_)
    return false;

  *exit_code_ = code;
  return true;
}

int AtomBrowserMainParts::GetExitCode() {
  return exit_code_ != nullptr ? *exit_code_ : 0;
}

base::Closure AtomBrowserMainParts::RegisterDestructionCallback(
    const base::Closure& callback) {
  auto iter = destructors_.insert(destructors_.end(), callback);
  return base::Bind(&Erase<std::list<base::Closure>>, &destructors_, iter);
}

void AtomBrowserMainParts::PreEarlyInitialization() {
  brightray::BrowserMainParts::PreEarlyInitialization();
#if defined(OS_POSIX)
  HandleSIGCHLD();
#endif
}

int AtomBrowserMainParts::PreCreateThreads() {
  LOG(ERROR) << "PreCreateThreads";
  TRACE_EVENT0("startup", "AtomBrowserMainParts::PreCreateThreads")

  base::FilePath user_data_dir;
  if (!PathService::Get(chrome::DIR_USER_DATA, &user_data_dir))
    return chrome::RESULT_CODE_MISSING_DATA;

  // Force MediaCaptureDevicesDispatcher to be created on UI thread.
  brightray::MediaCaptureDevicesDispatcher::GetInstance();

  // process_singleton_.reset(new ChromeProcessSingleton(
  //     user_data_dir_, base::Bind(&ProcessSingletonNotificationCallback)));

  scoped_refptr<base::SequencedTaskRunner> local_state_task_runner =
      JsonPrefStore::GetTaskRunnerForFile(
          base::FilePath(chrome::kLocalStorePoolName),
          content::BrowserThread::GetBlockingPool());

  {
    TRACE_EVENT0("startup",
      "AtomBrowserMainParts::PreCreateThreads:InitBrowswerProcessImpl");
    fake_browser_process_.reset(
        new BrowserProcessImpl(local_state_task_runner.get()));
  }

  if (parsed_command_line_.HasSwitch(switches::kEnableProfiling)) {
    TRACE_EVENT0("startup",
        "AtomBrowserMainParts::PreCreateThreads:InitProfiling");
    // User wants to override default tracking status.
    std::string flag =
      parsed_command_line_.GetSwitchValueASCII(switches::kEnableProfiling);
    // Default to basic profiling (no parent child support).
    tracked_objects::ThreadData::Status status =
          tracked_objects::ThreadData::PROFILING_ACTIVE;
    if (flag.compare("0") != 0)
      status = tracked_objects::ThreadData::DEACTIVATED;
    tracked_objects::ThreadData::InitializeAndSetTrackingStatus(status);
  }

  // Initialize tracking synchronizer system.
  tracking_synchronizer_ = new metrics::TrackingSynchronizer(
      base::MakeUnique<base::DefaultTickClock>(),
      base::Bind(&metrics::ContentTrackingSynchronizerDelegate::Create));

#if defined(OS_MACOSX)
  // Get the Keychain API to register for distributed notifications on the main
  // thread, which has a proper CFRunloop, instead of later on the I/O thread,
  // which doesn't. This ensures those notifications will get delivered
  // properly. See issue 37766.
  // (Note that the callback mask here is empty. I don't want to register for
  // any callbacks, I just want to initialize the mechanism.)
  SecKeychainAddCallback(&KeychainCallback, 0, NULL);
#endif  // defined(OS_MACOSX)

  device::GeolocationProvider::SetGeolocationDelegate(
      new AtomGeolocationDelegate());

  fake_browser_process_->PreCreateThreads();

  MuonCrashReporterClient::InitCrashReporting();

#if defined(USE_AURA)
  // The screen may have already been set in test initialization.
  if (!display::Screen::GetScreen())
    display::Screen::SetScreenInstance(views::CreateDesktopScreen());
#if defined(USE_X11)
  views::LinuxUI::instance()->UpdateDeviceScaleFactor();
#endif
#endif
  LOG(ERROR) << "PreCreateThreads done";

  return content::RESULT_CODE_NORMAL_EXIT;;
}

void AtomBrowserMainParts::PostEarlyInitialization() {
  brightray::BrowserMainParts::PostEarlyInitialization();
}

void AtomBrowserMainParts::OnMemoryPressure(
    base::MemoryPressureListener::MemoryPressureLevel memory_pressure_level) {
  if (atom::Browser::Get()->is_shutting_down())
    return;

  base::allocator::ReleaseFreeMemory();

  if (js_env_.get() && js_env_->isolate()) {
    js_env_->isolate()->LowMemoryNotification();
  }
}

void AtomBrowserMainParts::IdleHandler() {
  base::allocator::ReleaseFreeMemory();
}

#if defined(OS_WIN)
void AtomBrowserMainParts::PreMainMessageLoopStart() {
  LOG(ERROR) << "PreMainMessageLoopStart";
  brightray::BrowserMainParts::PreMainMessageLoopStart();
  if (!parameters_.ui_task) {
    // Make sure that we know how to handle exceptions from the message loop.
    InitializeWindowProcExceptions();
  }
  LOG(ERROR) << "PreMainMessageLoopStart done";
}
#endif

void AtomBrowserMainParts::PreMainMessageLoopRun() {
  LOG(ERROR) << "PreMainMessageLoopRun";
#if defined(USE_AURA)
  if (content::ServiceManagerConnection::GetForProcess() &&
      service_manager::ServiceManagerIsRemote()) {
    content::ServiceManagerConnection::GetForProcess()->
        SetConnectionLostClosure(base::Bind(&chrome::SessionEnding));
  }
#endif

// #if defined(OS_WIN)
//   InitializeChromeElf();
// #endif

  LOG(ERROR) << "PreMainMessageLoopRun 1";
  fake_browser_process_->PreMainMessageLoopRun();

  LOG(ERROR) << "PreMainMessageLoopRun 2";
  content::WebUIControllerFactory::RegisterFactory(
      ChromeWebUIControllerFactory::GetInstance());

  LOG(ERROR) << "PreMainMessageLoopRun 3";
  js_env_.reset(new JavascriptEnvironment);
  LOG(ERROR) << "PreMainMessageLoopRun 3.5";
  js_env_->isolate()->Enter();

  LOG(ERROR) << "PreMainMessageLoopRun 4";
  node_bindings_->Initialize();

  LOG(ERROR) << "PreMainMessageLoopRun 5";
  // Create the global environment.
  node::Environment* env =
      node_bindings_->CreateEnvironment(js_env_->context());

      LOG(ERROR) << "PreMainMessageLoopRun 6";
  // Add atom-shell extended APIs.
  atom_bindings_->BindTo(js_env_->isolate(), env->process_object());

  LOG(ERROR) << "PreMainMessageLoopRun 7";
  // Load everything.
  node_bindings_->LoadEnvironment(env);

  LOG(ERROR) << "PreMainMessageLoopRun 8";
  // Wrap the uv loop with global env.
  node_bindings_->set_uv_env(env);

  LOG(ERROR) << "PreMainMessageLoopRun 9";
#if defined(USE_X11)
  ui::TouchFactory::SetTouchDeviceListFromCommandLine();
#endif

  LOG(ERROR) << "PreMainMessageLoopRun 10";
  // Start idle gc.
  gc_timer_.Start(
      FROM_HERE, base::TimeDelta::FromMinutes(1),
      base::Bind(&AtomBrowserMainParts::IdleHandler,
                 base::Unretained(this)));

  LOG(ERROR) << "PreMainMessageLoopRun 11";
  memory_pressure_listener_.reset(new base::MemoryPressureListener(
      base::Bind(&AtomBrowserMainParts::OnMemoryPressure,
        base::Unretained(this))));

  LOG(ERROR) << "PreMainMessageLoopRun 12";
  // Make sure the userData directory is created.
  base::FilePath user_data;
  if (PathService::Get(chrome::DIR_USER_DATA, &user_data))
    base::CreateDirectoryAndGetError(user_data, nullptr);

  LOG(ERROR) << "PreMainMessageLoopRun 13";
  // PreProfileInit
  EnsureBrowserContextKeyedServiceFactoriesBuilt();

  LOG(ERROR) << "PreMainMessageLoopRun 14";
  browser_context_ = ProfileManager::GetActiveUserProfile();
  brightray::BrowserMainParts::PreMainMessageLoopRun();

  LOG(ERROR) << "PreMainMessageLoopRun 15";
  js_env_->OnMessageLoopCreated();
  LOG(ERROR) << "PreMainMessageLoopRun 16";
  node_bindings_->PrepareMessageLoop();
  LOG(ERROR) << "PreMainMessageLoopRun 17";
  node_bindings_->RunMessageLoop();

  LOG(ERROR) << "PreMainMessageLoopRun 18";
#if defined(USE_X11)
  libgtkui::GtkInitFromCommandLine(*base::CommandLine::ForCurrentProcess());
#endif

  LOG(ERROR) << "PreMainMessageLoopRun 19";
#if !defined(OS_MACOSX)
  // The corresponding call in macOS is in AtomApplicationDelegate.
  Browser::Get()->WillFinishLaunching();
  LOG(ERROR) << "PreMainMessageLoopRun 20";
  std::unique_ptr<base::DictionaryValue> empty_info(new base::DictionaryValue);
  LOG(ERROR) << "PreMainMessageLoopRun 21";
  Browser::Get()->DidFinishLaunching(*empty_info);
#endif

  LOG(ERROR) << "PreMainMessageLoopRun 22";
  // we want to allow the app to override the command line before running this
  auto command_line = base::CommandLine::ForCurrentProcess();
  // auto feature_list = base::FeatureList::GetInstance();
  base::FeatureList::InitializeInstance(
      command_line->GetSwitchValueASCII(switches::kEnableFeatures),
      command_line->GetSwitchValueASCII(switches::kDisableFeatures));
  LOG(ERROR) << "PreMainMessageLoopRun done";
}

bool AtomBrowserMainParts::MainMessageLoopRun(int* result_code) {
  LOG(ERROR) << "MainMessageLoopRun";
  exit_code_ = result_code;
  return brightray::BrowserMainParts::MainMessageLoopRun(result_code);
}

void AtomBrowserMainParts::PostMainMessageLoopStart() {
  LOG(ERROR) << "PostMainMessageLoopStart";
  brightray::BrowserMainParts::PostMainMessageLoopStart();
#if defined(OS_POSIX)
  HandleShutdownSignals();
#endif
  LOG(ERROR) << "PostMainMessageLoopStart done";
}

void AtomBrowserMainParts::PostMainMessageLoopRun() {
  LOG(ERROR) << "PostMainMessageLoopRun";
  browser_context_ = nullptr;
  brightray::BrowserMainParts::PostMainMessageLoopRun();

  js_env_->OnMessageLoopDestroying();

  js_env_->isolate()->Exit();
#if defined(OS_MACOSX)
  FreeAppDelegate();
#endif

  // Make sure destruction callbacks are called before message loop is
  // destroyed, otherwise some objects that need to be deleted on IO thread
  // won't be freed.
  // We don't use ranged for loop because iterators are getting invalided when
  // the callback runs.
  for (auto iter = destructors_.begin(); iter != destructors_.end();) {
    base::Closure& callback = *iter;
    ++iter;
    callback.Run();
  }

  restart_last_session_ = browser_shutdown::ShutdownPreThreadsStop();

  fake_browser_process_->StartTearDown();
  LOG(ERROR) << "PostMainMessageLoopRun done";
}

void AtomBrowserMainParts::PostDestroyThreads() {
  int restart_flags = restart_last_session_
                          ? browser_shutdown::RESTART_LAST_SESSION
                          : browser_shutdown::NO_FLAGS;

  fake_browser_process_->PostDestroyThreads();
  // browser_shutdown takes care of deleting browser_process, so we need to
  // release it.
  ignore_result(fake_browser_process_.release());

  browser_shutdown::ShutdownPostThreadsStop(restart_flags);
  // process_singleton_.reset();
  // device_event_log::Shutdown();

  // We need to do this check as late as possible, but due to modularity, this
  // may be the last point in Chrome.  This would be more effective if done at
  // a higher level on the stack, so that it is impossible for an early return
  // to bypass this code.  Perhaps we need a *final* hook that is called on all
  // paths from content/browser/browser_main.
  // CHECK(metrics::MetricsService::UmaMetricsProperlyShutdown());

}

}  // namespace atom
