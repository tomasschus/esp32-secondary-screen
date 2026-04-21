use std::collections::HashMap;
use std::process::{Child, Command};
use std::sync::Mutex;
use tauri::{
    menu::{Menu, MenuItem, PredefinedMenuItem},
    tray::{MouseButton, MouseButtonState, TrayIconBuilder, TrayIconEvent},
    AppHandle, Emitter, Manager, State,
};
use tauri_plugin_store::StoreExt;

struct ProcessManager {
    children: HashMap<String, Child>,
}

impl ProcessManager {
    fn new() -> Self {
        Self {
            children: HashMap::new(),
        }
    }
}

struct AppState {
    process_manager: Mutex<ProcessManager>,
    running: Mutex<HashMap<String, bool>>,
}

#[derive(serde::Serialize, serde::Deserialize, Clone, Debug)]
struct ServiceOptions {
    window: Option<String>,
    fps: Option<u32>,
    quality: Option<u32>,
    interval: Option<u32>,
    apps: Option<String>,
}

#[derive(serde::Serialize, serde::Deserialize, Clone, Debug)]
struct Settings {
    ip: String,
    services: ServicesSettings,
}

#[derive(serde::Serialize, serde::Deserialize, Clone, Debug)]
struct ServicesSettings {
    stats: bool,
    mirror: bool,
    window_mirror: bool,
    notifications: bool,
}

impl Default for Settings {
    fn default() -> Self {
        Settings {
            ip: "192.168.1.100".to_string(),
            services: ServicesSettings {
                stats: false,
                mirror: false,
                window_mirror: false,
                notifications: false,
            },
        }
    }
}

fn get_python_cmd() -> &'static str {
    #[cfg(target_os = "windows")]
    return "python";
    #[cfg(not(target_os = "windows"))]
    return "python3";
}

fn get_client_path(app: &AppHandle) -> std::path::PathBuf {
    // The client scripts are at ../client/ relative to the app directory
    // In dev: next to the app folder
    // In production: next to the bundle
    if let Ok(resource_dir) = app.path().resource_dir() {
        // Try resource dir first (for bundled app)
        let client = resource_dir.join("client");
        if client.exists() {
            return client;
        }
    }
    // Fall back to relative path from current exe
    if let Ok(exe) = std::env::current_exe() {
        if let Some(parent) = exe.parent() {
            // In dev mode the exe is deep in target/debug, so walk up
            let mut p = parent.to_path_buf();
            for _ in 0..6 {
                let candidate = p.join("client");
                if candidate.exists() {
                    return candidate;
                }
                if let Some(up) = p.parent() {
                    p = up.to_path_buf();
                } else {
                    break;
                }
            }
        }
    }
    // Last resort: use the hardcoded relative path from the repo root
    std::path::PathBuf::from("../client")
}

fn build_command(name: &str, ip: &str, options: &ServiceOptions, client_path: &std::path::Path) -> Option<Command> {
    let python = get_python_cmd();
    let mut cmd = Command::new(python);

    match name {
        "stats" => {
            let interval = options.interval.unwrap_or(2);
            cmd.arg(client_path.join("system_stats.py"))
                .arg("--host").arg(ip)
                .arg("--interval").arg(interval.to_string());
        }
        "mirror" => {
            let fps = options.fps.unwrap_or(15);
            let quality = options.quality.unwrap_or(50);
            cmd.arg(client_path.join("screen_capture.py"))
                .arg("--host").arg(ip)
                .arg("--fps").arg(fps.to_string())
                .arg("--quality").arg(quality.to_string());
        }
        "windowMirror" => {
            let fps = options.fps.unwrap_or(10);
            let window_name = options.window.clone().unwrap_or_default();
            cmd.arg(client_path.join("screen_capture.py"))
                .arg("--host").arg(ip)
                .arg("--fps").arg(fps.to_string())
                .arg("--window").arg(window_name);
        }
        "notifications" => {
            #[cfg(target_os = "windows")]
            {
                let apps = options.apps.clone().unwrap_or_else(|| "Claude".to_string());
                cmd.arg(client_path.join("windows_notifier.py"))
                    .arg("--host").arg(ip)
                    .arg("--apps").arg(apps);
            }
            #[cfg(not(target_os = "windows"))]
            {
                // Not supported on macOS
                return None;
            }
        }
        _ => return None,
    }

    // Hide console window on Windows
    #[cfg(target_os = "windows")]
    {
        use std::os::windows::process::CommandExt;
        cmd.creation_flags(0x08000000); // CREATE_NO_WINDOW
    }

    Some(cmd)
}

#[tauri::command]
fn get_settings(app: AppHandle) -> Result<Settings, String> {
    let store = app.store("settings.json").map_err(|e| e.to_string())?;
    let settings: Settings = store
        .get("settings")
        .and_then(|v| serde_json::from_value(v).ok())
        .unwrap_or_default();
    Ok(settings)
}

#[tauri::command]
fn save_settings(app: AppHandle, settings: Settings) -> Result<(), String> {
    let store = app.store("settings.json").map_err(|e| e.to_string())?;
    store.set("settings", serde_json::to_value(&settings).map_err(|e| e.to_string())?);
    store.save().map_err(|e| e.to_string())?;
    Ok(())
}

#[tauri::command]
fn toggle_service(
    app: AppHandle,
    state: State<AppState>,
    name: String,
    enabled: bool,
    options: ServiceOptions,
) -> Result<(), String> {
    let mut pm = state.process_manager.lock().map_err(|e| e.to_string())?;
    let mut running = state.running.lock().map_err(|e| e.to_string())?;

    if enabled {
        // Stop existing process for this service if running
        if let Some(mut child) = pm.children.remove(&name) {
            let _ = child.kill();
        }

        // Get settings to retrieve IP
        let store = app.store("settings.json").map_err(|e| e.to_string())?;
        let settings: Settings = store
            .get("settings")
            .and_then(|v| serde_json::from_value(v).ok())
            .unwrap_or_default();

        let client_path = get_client_path(&app);
        if let Some(mut cmd) = build_command(&name, &settings.ip, &options, &client_path) {
            match cmd.spawn() {
                Ok(child) => {
                    pm.children.insert(name.clone(), child);
                    running.insert(name, true);
                }
                Err(e) => {
                    running.insert(name.clone(), false);
                    return Err(format!("Failed to start {}: {}", name, e));
                }
            }
        } else {
            running.insert(name, false);
        }
    } else {
        if let Some(mut child) = pm.children.remove(&name) {
            let _ = child.kill();
        }
        running.insert(name, false);
    }

    // Emit status update
    let statuses: HashMap<String, bool> = running.clone();
    let _ = app.emit("service-status-changed", statuses);

    Ok(())
}

#[tauri::command]
fn get_service_status(state: State<AppState>) -> Result<HashMap<String, bool>, String> {
    let mut pm = state.process_manager.lock().map_err(|e| e.to_string())?;
    let mut running = state.running.lock().map_err(|e| e.to_string())?;

    // First pass: collect exit status for each tracked process
    let keys: Vec<String> = pm.children.keys().cloned().collect();
    let mut exited: Vec<String> = Vec::new();
    for key in &keys {
        if let Some(child) = pm.children.get_mut(key) {
            match child.try_wait() {
                Ok(Some(_)) => exited.push(key.clone()),
                Ok(None) => { running.insert(key.clone(), true); }
                Err(_) => exited.push(key.clone()),
            }
        }
    }
    // Second pass: remove exited processes
    for key in exited {
        pm.children.remove(&key);
        running.insert(key, false);
    }

    Ok(running.clone())
}

#[tauri::command]
fn list_windows() -> Result<Vec<String>, String> {
    #[cfg(target_os = "windows")]
    {
        let output = Command::new("tasklist")
            .args(["/fo", "csv", "/nh"])
            .output()
            .map_err(|e| e.to_string())?;
        let stdout = String::from_utf8_lossy(&output.stdout);
        let mut names: Vec<String> = stdout
            .lines()
            .filter_map(|line| {
                let parts: Vec<&str> = line.splitn(2, ',').collect();
                if let Some(name) = parts.first() {
                    let clean = name.trim_matches('"').trim().to_string();
                    if !clean.is_empty() {
                        Some(clean)
                    } else {
                        None
                    }
                } else {
                    None
                }
            })
            .collect();
        names.sort();
        names.dedup();
        Ok(names)
    }
    #[cfg(not(target_os = "windows"))]
    {
        let output = Command::new("osascript")
            .args(["-e", "tell application \"System Events\" to get name of every process where background only is false"])
            .output()
            .map_err(|e| e.to_string())?;
        let stdout = String::from_utf8_lossy(&output.stdout);
        let names: Vec<String> = stdout
            .split(", ")
            .map(|s| s.trim().to_string())
            .filter(|s| !s.is_empty())
            .collect();
        Ok(names)
    }
}

fn show_main_window(app: &AppHandle) {
    if let Some(window) = app.get_webview_window("main") {
        let _ = window.show();
        let _ = window.set_focus();
    }
}

fn build_tray_menu(app: &AppHandle, running: &HashMap<String, bool>) -> tauri::Result<Menu<tauri::Wry>> {
    let stats_dot = if *running.get("stats").unwrap_or(&false) { "●" } else { "○" };
    let mirror_dot = if *running.get("mirror").unwrap_or(&false) { "●" } else { "○" };
    let notif_dot = if *running.get("notifications").unwrap_or(&false) { "●" } else { "○" };

    Menu::with_items(
        app,
        &[
            &MenuItem::with_id(app, "open", "Open", true, None::<&str>)?,
            &PredefinedMenuItem::separator(app)?,
            &MenuItem::with_id(app, "stats", format!("Stats {}", stats_dot), false, None::<&str>)?,
            &MenuItem::with_id(app, "mirror", format!("Mirror {}", mirror_dot), false, None::<&str>)?,
            &MenuItem::with_id(app, "notifications", format!("Notifications {}", notif_dot), false, None::<&str>)?,
            &PredefinedMenuItem::separator(app)?,
            &MenuItem::with_id(app, "quit", "Quit", true, None::<&str>)?,
        ],
    )
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_store::Builder::default().build())
        .manage(AppState {
            process_manager: Mutex::new(ProcessManager::new()),
            running: Mutex::new(HashMap::new()),
        })
        .setup(|app| {
            // Build tray menu
            let running: HashMap<String, bool> = HashMap::new();
            let menu = build_tray_menu(app.handle(), &running)?;

            let _tray = TrayIconBuilder::new()
                .menu(&menu)
                .icon(app.default_window_icon().unwrap().clone())
                .tooltip("ESP32 Screen")
                .on_menu_event(|app, event| match event.id.as_ref() {
                    "open" => show_main_window(app),
                    "quit" => {
                        // Kill all child processes before quitting
                        if let Some(state) = app.try_state::<AppState>() {
                            if let Ok(mut pm) = state.process_manager.lock() {
                                for (_, mut child) in pm.children.drain() {
                                    let _ = child.kill();
                                }
                            }
                        }
                        app.exit(0);
                    }
                    _ => {}
                })
                .on_tray_icon_event(|tray, event| {
                    if let TrayIconEvent::Click {
                        button: MouseButton::Left,
                        button_state: MouseButtonState::Up,
                        ..
                    } = event
                    {
                        show_main_window(tray.app_handle());
                    }
                })
                .build(app)?;

            Ok(())
        })
        .on_window_event(|window, event| {
            if let tauri::WindowEvent::CloseRequested { api, .. } = event {
                api.prevent_close();
                let _ = window.hide();
            }
        })
        .invoke_handler(tauri::generate_handler![
            get_settings,
            save_settings,
            toggle_service,
            get_service_status,
            list_windows,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
