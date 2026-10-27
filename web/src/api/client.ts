import type { DeviceTelemetry, NavEvent, RoadHazard } from "../types.js";

const getApiBase = (): string => {
  if (typeof import.meta !== "undefined" && (import.meta as any).env?.VITE_API_URL) {
    return (import.meta as any).env.VITE_API_URL;
  }
  if (typeof window === "undefined") return "http://localhost:8080";
  const { protocol, hostname, port } = window.location;
  if (port === "5173" || port === "3000") {
    return `${protocol}//${hostname}:8080`;
  }
  return `${protocol}//${window.location.host}`;
};

const getWsBase = (): string => {
  if (typeof import.meta !== "undefined" && (import.meta as any).env?.VITE_WS_URL) {
    return (import.meta as any).env.VITE_WS_URL;
  }
  if (typeof window === "undefined") return "ws://localhost:8080";
  const { protocol, hostname, port } = window.location;
  const wsProto = protocol === "https:" ? "wss:" : "ws:";
  if (port === "5173" || port === "3000") {
    return `${wsProto}//${hostname}:8080`;
  }
  return `${wsProto}//${window.location.host}`;
};

export class ApiClient {
  public isOnline = false;
  private ws: WebSocket | null = null;
  private listeners: ((telemetry: DeviceTelemetry) => void)[] = [];

  constructor() {
    this.initWebSocket();
  }

  private initWebSocket() {
    try {
      const wsUrl = `${getWsBase()}/v1/stream/live`;
      this.ws = new WebSocket(wsUrl);
      this.ws.onopen = () => {
        this.isOnline = true;
      };
      this.ws.onmessage = (event) => {
        try {
          const msg = JSON.parse(event.data);
          if (msg.type === "state" && msg.data) {
            this.notify(msg.data);
          } else if (msg.type === "snapshot" && Array.isArray(msg.devices)) {
            for (const d of msg.devices) {
              this.notify(d);
            }
          }
        } catch {}
      };
      this.ws.onerror = () => {
        this.isOnline = false;
      };
      this.ws.onclose = () => {
        this.isOnline = false;
        setTimeout(() => this.initWebSocket(), 4000);
      };
    } catch {
      this.isOnline = false;
    }
  }

  onTelemetry(callback: (telemetry: DeviceTelemetry) => void) {
    this.listeners.push(callback);
    return () => {
      this.listeners = this.listeners.filter((cb) => cb !== callback);
    };
  }

  private notify(data: any) {
    const formatted: DeviceTelemetry = {
      device_id: data.device_id,
      trip_id: data.trip_id,
      t_ms: data.t_ms,
      lat: data.lat,
      lon: data.lon,
      speed_mps: data.speed_mps,
      speed_kmh: Math.round(data.speed_mps * 3.6 * 10) / 10,
      heading_deg: data.heading_deg,
      mode: data.mode,
      hpl_m: data.hpl_m,
      ell_major_m: data.ell_major_m ?? data.hpl_m,
      ell_minor_m: data.ell_minor_m ?? data.hpl_m * 0.7,
      ell_angle_deg: data.ell_angle_deg ?? data.heading_deg,
      vehicle_mode: data.vehicle_mode || "CAR",
      way_id: data.way_id,
      level: data.level,
    };

    for (const listener of this.listeners) {
      listener(formatted);
    }
  }

  async getLiveFeatures(): Promise<DeviceTelemetry[]> {
    try {
      const res = await fetch(`${getApiBase()}/v1/live`);
      if (!res.ok) throw new Error("Offline");
      const geo = await res.json();
      if (!geo.features || geo.features.length === 0) {
        return this.generateFallbackDemo();
      }
      return geo.features.map((f: any) => ({
        device_id: f.properties.device_id,
        trip_id: f.properties.trip_id,
        t_ms: f.properties.t_ms,
        lat: f.geometry.coordinates[1],
        lon: f.geometry.coordinates[0],
        speed_mps: f.properties.speed_mps,
        speed_kmh: f.properties.speed_kmh,
        heading_deg: f.properties.heading_deg,
        mode: f.properties.mode,
        hpl_m: f.properties.hpl_m,
        ell_major_m: f.properties.ell_major_m,
        ell_minor_m: f.properties.ell_minor_m,
        ell_angle_deg: f.properties.ell_angle_deg,
        vehicle_mode: f.properties.vehicle_mode,
        way_id: f.properties.way_id,
        level: f.properties.level,
      }));
    } catch {
      return this.generateFallbackDemo();
    }
  }

  async getEvents(): Promise<NavEvent[]> {
    try {
      const res = await fetch(`${getApiBase()}/v1/events?limit=50`);
      if (!res.ok) throw new Error("Offline");
      const geo = await res.json();
      return geo.features.map((f: any) => ({
        id: f.properties.id,
        device_id: f.properties.device_id,
        trip_id: f.properties.trip_id,
        t_ms: f.properties.t_ms,
        type: f.properties.event_type,
        lat: f.geometry.coordinates[1],
        lon: f.geometry.coordinates[0],
        details: f.properties.details,
      }));
    } catch {
      return [
        {
          id: 1,
          device_id: "DEV_MB_101",
          trip_id: "TRIP_MUMBAI_01",
          t_ms: Date.now() - 35000,
          type: "TUNNEL_AHEAD",
          lat: 19.0195,
          lon: 72.8745,
          details: { tunnel: "Eastern Freeway Anik Tunnel", distance_ahead_m: 80.0 },
        },
        {
          id: 2,
          device_id: "DEV_MB_101",
          trip_id: "TRIP_MUMBAI_01",
          t_ms: Date.now() - 25000,
          type: "DR",
          lat: 19.0225,
          lon: 72.8760,
          details: { reason: "GNSS outage entered (tunnel)", hpl_m: 5.4 },
        },
        {
          id: 3,
          device_id: "DEV_MB_101",
          trip_id: "TRIP_MUMBAI_01",
          t_ms: Date.now() - 5000,
          type: "REACQ",
          lat: 19.0280,
          lon: 72.8780,
          details: { first_fix_r68_m: 3.2, pos_nis: 2.1 },
        },
      ];
    }
  }

  async getRoadHazards(): Promise<RoadHazard[]> {
    try {
      const res = await fetch(`${getApiBase()}/v1/road-conditions`);
      if (!res.ok) throw new Error("Offline");
      const geo = await res.json();
      return geo.features.map((f: any) => ({
        id: f.properties.id,
        device_id: f.properties.device_id,
        t_ms: f.properties.t_ms,
        lat: f.geometry.coordinates[1],
        lon: f.geometry.coordinates[0],
        kind: f.properties.kind,
        severity: f.properties.severity,
      }));
    } catch {
      return [
        { id: 101, device_id: "DEV_MB_101", t_ms: Date.now() - 60000, lat: 18.9610, lon: 72.8555, kind: "POTHOLE", severity: 2.4 },
        { id: 102, device_id: "DEV_MB_102", t_ms: Date.now() - 40000, lat: 18.9265, lon: 72.8250, kind: "SPEED_BREAKER", severity: 1.8 },
      ];
    }
  }

  async ingestTelemetry(payload: {
    device_id: string;
    trip_id: string;
    states?: Array<{
      t_ms: number;
      lat: number;
      lon: number;
      speed_mps?: number;
      heading_deg?: number;
      mode: string;
      hpl_m: number;
      ell_major_m?: number;
      ell_minor_m?: number;
      ell_angle_deg?: number;
      way_id?: number;
      vehicle_mode?: string;
    }>;
    events?: Array<{
      t_ms: number;
      type: string;
      lat?: number;
      lon?: number;
      payload?: any;
    }>;
    road_conditions?: Array<{
      t_ms: number;
      lat: number;
      lon: number;
      kind: string;
      severity: number;
    }>;
  }): Promise<boolean> {
    try {
      const res = await fetch(`${getApiBase()}/v1/ingest`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(payload),
      });
      return res.ok;
    } catch {
      return false;
    }
  }

  async getMapPacks(): Promise<Array<{ region: string; files: Array<{ filename: string; size_bytes: number; updated_at: number; download_url: string }> }>> {
    try {
      const res = await fetch(`${getApiBase()}/v1/map-packs`);
      if (!res.ok) throw new Error("Offline");
      return await res.json();
    } catch {
      return [
        {
          region: "national_highway_india",
          files: [
            { filename: "map.sqlite", size_bytes: 57344, updated_at: Date.now(), download_url: "/v1/map-packs/national_highway_india/map.sqlite" },
            { filename: "user.sqlite", size_bytes: 24576, updated_at: Date.now(), download_url: "/v1/map-packs/national_highway_india/user.sqlite" }
          ]
        },
        {
          region: "mumbai_metro",
          files: [
            { filename: "map.sqlite", size_bytes: 61440, updated_at: Date.now(), download_url: "/v1/map-packs/mumbai_metro/map.sqlite" },
            { filename: "user.sqlite", size_bytes: 36864, updated_at: Date.now(), download_url: "/v1/map-packs/mumbai_metro/user.sqlite" }
          ]
        },
        {
          region: "delhi_ncr",
          files: [
            { filename: "map.sqlite", size_bytes: 56320, updated_at: Date.now(), download_url: "/v1/map-packs/delhi_ncr/map.sqlite" },
            { filename: "user.sqlite", size_bytes: 24576, updated_at: Date.now(), download_url: "/v1/map-packs/delhi_ncr/user.sqlite" }
          ]
        },
        {
          region: "bengaluru_metro",
          files: [
            { filename: "map.sqlite", size_bytes: 54272, updated_at: Date.now(), download_url: "/v1/map-packs/bengaluru_metro/map.sqlite" },
            { filename: "user.sqlite", size_bytes: 24576, updated_at: Date.now(), download_url: "/v1/map-packs/bengaluru_metro/user.sqlite" }
          ]
        }
      ];
    }
  }

  getMapPackDownloadUrl(region: string, file: string): string {
    return `${getApiBase()}/v1/map-packs/${region}/${file}`;
  }

  private generateFallbackDemo(): DeviceTelemetry[] {
    const t = Date.now() / 1000.0;
    // Simulate vehicle driving along Eastern Freeway
    const progress = (t * 0.02) % 1.0;
    const lat = 18.9400 + progress * 0.09;
    const lon = 72.8450 + progress * 0.035;

    // Inside Anik Tunnel range
    const isTunnel = lat >= 19.0200 && lat <= 19.0280;

    return [
      {
        device_id: "DEV_MB_101",
        trip_id: "TRIP_MUMBAI_DEMO",
        t_ms: Date.now(),
        lat,
        lon,
        speed_mps: 18.2 + Math.sin(t) * 1.5,
        speed_kmh: Math.round((18.2 + Math.sin(t) * 1.5) * 3.6),
        heading_deg: 24.5,
        mode: isTunnel ? "DR" : "GNSS_OK",
        hpl_m: isTunnel ? 8.4 : 3.2,
        ell_major_m: isTunnel ? 8.4 : 3.2,
        ell_minor_m: isTunnel ? 4.1 : 2.1,
        ell_angle_deg: 24.5,
        vehicle_mode: "CAR",
        way_id: isTunnel ? 1002 : 1001,
      },
      {
        device_id: "DEV_MB_102",
        trip_id: "TRIP_BIKE_DEMO",
        t_ms: Date.now(),
        lat: 18.9280 + Math.sin(t * 0.05) * 0.005,
        lon: 72.8250 + Math.cos(t * 0.05) * 0.005,
        speed_mps: 8.5,
        speed_kmh: 30.6,
        heading_deg: 180.0 + (t * 10) % 360,
        mode: "GNSS_OK",
        hpl_m: 2.8,
        ell_major_m: 2.8,
        ell_minor_m: 1.9,
        ell_angle_deg: 180.0,
        vehicle_mode: "TWO_WHEELER",
        way_id: 2001,
      },
    ];
  }
}

export const api = new ApiClient();
