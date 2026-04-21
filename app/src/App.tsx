import { useState, useEffect, useCallback } from "react";
import { invoke } from "@tauri-apps/api/core";
import { listen } from "@tauri-apps/api/event";

// ─── Types ────────────────────────────────────────────────────────────────────

interface Settings {
  ip: string;
  services: {
    stats: boolean;
    mirror: boolean;
    window_mirror: boolean;
    notifications: boolean;
  };
}

interface ServiceStatus {
  [key: string]: boolean;
}

interface ServiceConfig {
  id: string;
  label: string;
  description: string;
  icon: string;
  platformOnly?: "windows" | "macos";
}

const SERVICES: ServiceConfig[] = [
  {
    id: "stats",
    label: "System Stats",
    description: "CPU, RAM & network usage on the ESP32 display",
    icon: "📊",
  },
  {
    id: "mirror",
    label: "Screen Mirror",
    description: "Full-screen capture streamed to the ESP32",
    icon: "🖥️",
  },
  {
    id: "windowMirror",
    label: "Window Mirror",
    description: "Stream a specific application window",
    icon: "🪟",
  },
  {
    id: "notifications",
    label: "Notifications",
    description: "Forward desktop notifications to the ESP32",
    icon: "🔔",
    platformOnly: "windows",
  },
];

// ─── Toggle Switch ─────────────────────────────────────────────────────────────

function Toggle({
  checked,
  onChange,
  disabled,
}: {
  checked: boolean;
  onChange: (v: boolean) => void;
  disabled?: boolean;
}) {
  return (
    <button
      role="switch"
      aria-checked={checked}
      disabled={disabled}
      onClick={() => onChange(!checked)}
      style={{
        position: "relative",
        width: 44,
        height: 24,
        borderRadius: 12,
        border: "none",
        background: checked ? "#4a9eff" : "#3a3a3a",
        cursor: disabled ? "not-allowed" : "pointer",
        transition: "background 0.2s",
        flexShrink: 0,
        opacity: disabled ? 0.5 : 1,
      }}
    >
      <span
        style={{
          position: "absolute",
          top: 3,
          left: checked ? 23 : 3,
          width: 18,
          height: 18,
          borderRadius: "50%",
          background: "#fff",
          transition: "left 0.2s",
          display: "block",
        }}
      />
    </button>
  );
}

// ─── Status Dot ────────────────────────────────────────────────────────────────

function StatusDot({ running }: { running: boolean }) {
  return (
    <span
      title={running ? "Running" : "Stopped"}
      style={{
        display: "inline-block",
        width: 8,
        height: 8,
        borderRadius: "50%",
        background: running ? "#22c55e" : "#3a3a3a",
        flexShrink: 0,
        boxShadow: running ? "0 0 6px #22c55e" : "none",
        transition: "background 0.3s, box-shadow 0.3s",
      }}
    />
  );
}

// ─── Service Card ─────────────────────────────────────────────────────────────

function ServiceCard({
  service,
  enabled,
  running,
  onToggle,
  extra,
}: {
  service: ServiceConfig;
  enabled: boolean;
  running: boolean;
  onToggle: (enabled: boolean) => void;
  extra?: React.ReactNode;
}) {
  return (
    <div
      style={{
        background: "#1e1e1e",
        borderRadius: 10,
        padding: "14px 16px",
        marginBottom: 10,
        border: enabled ? "1px solid #2a3a50" : "1px solid #2a2a2a",
        transition: "border-color 0.2s",
      }}
    >
      <div
        style={{
          display: "flex",
          alignItems: "center",
          gap: 10,
        }}
      >
        <span style={{ fontSize: 20 }}>{service.icon}</span>
        <div style={{ flex: 1, minWidth: 0 }}>
          <div
            style={{
              display: "flex",
              alignItems: "center",
              gap: 6,
              marginBottom: 2,
            }}
          >
            <span
              style={{
                fontSize: 14,
                fontWeight: 600,
                color: "#f0f0f0",
                whiteSpace: "nowrap",
              }}
            >
              {service.label}
            </span>
            <StatusDot running={running} />
          </div>
          <p
            style={{
              fontSize: 12,
              color: "#888",
              margin: 0,
              lineHeight: 1.4,
            }}
          >
            {service.description}
          </p>
        </div>
        <Toggle checked={enabled} onChange={onToggle} />
      </div>
      {extra && <div style={{ marginTop: 10 }}>{extra}</div>}
    </div>
  );
}

// ─── App ──────────────────────────────────────────────────────────────────────

const IS_WINDOWS = navigator.userAgent.includes("Windows");

export default function App() {
  const [ip, setIp] = useState("192.168.1.100");
  const [ipDraft, setIpDraft] = useState("192.168.1.100");
  const [serviceEnabled, setServiceEnabled] = useState<Record<string, boolean>>(
    {}
  );
  const [serviceStatus, setServiceStatus] = useState<ServiceStatus>({});
  const [windowName, setWindowName] = useState("");
  const [loading, setLoading] = useState(true);
  const [error, setError] = useState<string | null>(null);

  // Load settings on mount
  useEffect(() => {
    (async () => {
      try {
        const settings = await invoke<Settings>("get_settings");
        setIp(settings.ip);
        setIpDraft(settings.ip);
        setServiceEnabled({
          stats: settings.services.stats,
          mirror: settings.services.mirror,
          windowMirror: settings.services.window_mirror,
          notifications: settings.services.notifications,
        });
      } catch (e) {
        console.error("Failed to load settings:", e);
      }
      try {
        const status = await invoke<ServiceStatus>("get_service_status");
        setServiceStatus(status);
      } catch (e) {
        console.error("Failed to load status:", e);
      }
      setLoading(false);
    })();
  }, []);

  // Poll service status every 3 seconds
  useEffect(() => {
    const interval = setInterval(async () => {
      try {
        const status = await invoke<ServiceStatus>("get_service_status");
        setServiceStatus(status);
      } catch (_) {}
    }, 3000);
    return () => clearInterval(interval);
  }, []);

  // Listen to status change events from Rust
  useEffect(() => {
    const unlisten = listen<ServiceStatus>("service-status-changed", (event) => {
      setServiceStatus(event.payload);
    });
    return () => {
      unlisten.then((fn) => fn());
    };
  }, []);

  const saveSettings = useCallback(
    async (newEnabled: Record<string, boolean>, newIp: string) => {
      try {
        await invoke("save_settings", {
          settings: {
            ip: newIp,
            services: {
              stats: newEnabled.stats ?? false,
              mirror: newEnabled.mirror ?? false,
              window_mirror: newEnabled.windowMirror ?? false,
              notifications: newEnabled.notifications ?? false,
            },
          },
        });
      } catch (e) {
        console.error("Failed to save settings:", e);
      }
    },
    []
  );

  const handleIpBlur = async () => {
    const trimmed = ipDraft.trim();
    if (trimmed === ip) return;
    setIp(trimmed);
    await saveSettings(serviceEnabled, trimmed);
  };

  const handleToggle = async (serviceId: string, enabled: boolean) => {
    const newEnabled = { ...serviceEnabled, [serviceId]: enabled };
    setServiceEnabled(newEnabled);
    setError(null);

    try {
      const options: Record<string, unknown> = {};
      if (serviceId === "windowMirror") {
        options.window = windowName;
        options.fps = 10;
      } else if (serviceId === "mirror") {
        options.fps = 15;
        options.quality = 50;
      } else if (serviceId === "stats") {
        options.interval = 2;
      } else if (serviceId === "notifications") {
        options.apps = "Claude";
      }

      await invoke("toggle_service", {
        name: serviceId,
        enabled,
        options,
      });
    } catch (e) {
      setError(String(e));
      // Revert the toggle on error
      setServiceEnabled((prev) => ({ ...prev, [serviceId]: !enabled }));
    }

    await saveSettings(newEnabled, ip);
  };

  const anyRunning = Object.values(serviceStatus).some(Boolean);

  return (
    <div
      style={{
        background: "#0d0d0d",
        minHeight: "100vh",
        color: "#f0f0f0",
        fontFamily:
          "-apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif",
        display: "flex",
        flexDirection: "column",
        userSelect: "none",
      }}
    >
      {/* Header */}
      <div
        style={{
          padding: "20px 20px 14px",
          borderBottom: "1px solid #1e1e1e",
          display: "flex",
          alignItems: "center",
          justifyContent: "space-between",
        }}
      >
        <div>
          <h1
            style={{
              margin: 0,
              fontSize: 18,
              fontWeight: 700,
              color: "#fff",
              letterSpacing: "-0.3px",
            }}
          >
            ESP32 Screen
          </h1>
          <p style={{ margin: "2px 0 0", fontSize: 12, color: "#555" }}>
            Display controller
          </p>
        </div>
        <div style={{ display: "flex", alignItems: "center", gap: 6 }}>
          <span
            style={{
              width: 8,
              height: 8,
              borderRadius: "50%",
              background: anyRunning ? "#22c55e" : "#555",
              display: "inline-block",
              boxShadow: anyRunning ? "0 0 6px #22c55e" : "none",
            }}
          />
          <span style={{ fontSize: 12, color: "#888" }}>
            {anyRunning ? "Active" : "Idle"}
          </span>
        </div>
      </div>

      {/* Content */}
      <div style={{ padding: "16px 16px 0", flex: 1 }}>
        {/* IP Setting */}
        <div style={{ marginBottom: 16 }}>
          <label
            style={{ fontSize: 12, color: "#666", display: "block", marginBottom: 6 }}
          >
            ESP32 IP Address
          </label>
          <input
            type="text"
            value={ipDraft}
            onChange={(e) => setIpDraft(e.target.value)}
            onBlur={handleIpBlur}
            onKeyDown={(e) => e.key === "Enter" && handleIpBlur()}
            placeholder="192.168.1.100"
            spellCheck={false}
            style={{
              width: "100%",
              background: "#1e1e1e",
              border: "1px solid #2a2a2a",
              borderRadius: 8,
              padding: "10px 12px",
              color: "#f0f0f0",
              fontSize: 14,
              fontFamily: "monospace",
              outline: "none",
              boxSizing: "border-box",
              transition: "border-color 0.2s",
            }}
            onFocus={(e) => (e.target.style.borderColor = "#4a9eff")}
            onBlurCapture={(e) => (e.target.style.borderColor = "#2a2a2a")}
          />
        </div>

        {/* Error Banner */}
        {error && (
          <div
            style={{
              background: "#2a1010",
              border: "1px solid #5a2020",
              borderRadius: 8,
              padding: "10px 12px",
              marginBottom: 12,
              fontSize: 12,
              color: "#ff8080",
              wordBreak: "break-word",
            }}
          >
            {error}
          </div>
        )}

        {/* Service Cards */}
        {loading ? (
          <div
            style={{ color: "#555", fontSize: 13, textAlign: "center", marginTop: 40 }}
          >
            Loading…
          </div>
        ) : (
          SERVICES.filter(
            (s) => !s.platformOnly || (s.platformOnly === "windows" && IS_WINDOWS)
          ).map((service) => (
            <ServiceCard
              key={service.id}
              service={service}
              enabled={serviceEnabled[service.id] ?? false}
              running={serviceStatus[service.id] ?? false}
              onToggle={(v) => handleToggle(service.id, v)}
              extra={
                service.id === "windowMirror" &&
                (serviceEnabled.windowMirror ?? false) ? (
                  <input
                    type="text"
                    value={windowName}
                    onChange={(e) => setWindowName(e.target.value)}
                    placeholder="Window name (e.g. Chrome)"
                    style={{
                      width: "100%",
                      background: "#141414",
                      border: "1px solid #2a2a2a",
                      borderRadius: 6,
                      padding: "8px 10px",
                      color: "#f0f0f0",
                      fontSize: 13,
                      outline: "none",
                      boxSizing: "border-box",
                    }}
                  />
                ) : undefined
              }
            />
          ))
        )}
      </div>

      {/* Footer */}
      <div
        style={{
          padding: "12px 20px",
          borderTop: "1px solid #1a1a1a",
          display: "flex",
          justifyContent: "space-between",
          alignItems: "center",
        }}
      >
        <span style={{ fontSize: 11, color: "#444" }}>v0.1.0</span>
        <span style={{ fontSize: 11, color: "#444" }}>
          {IS_WINDOWS ? "Windows" : "macOS"}
        </span>
      </div>
    </div>
  );
}
