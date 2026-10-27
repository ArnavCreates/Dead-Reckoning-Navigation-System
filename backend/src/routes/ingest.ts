import type { FastifyPluginAsync } from "fastify";
import { getDatabase } from "../db/database.js";
import { telemetryService } from "../services/telemetry.js";

interface IngestPayload {
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
    match_posterior?: number;
    vehicle_mode?: string;
    level?: number;
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
}

export const ingestRoutes: FastifyPluginAsync = async (app) => {
  const db = getDatabase();

  app.post<{ Body: IngestPayload }>("/v1/ingest", async (request, reply) => {
    const payload = request.body;
    if (!payload || !payload.device_id || !payload.trip_id) {
      return reply.code(400).send({ error: "device_id and trip_id are required" });
    }

    const { device_id, trip_id, states = [], events = [], road_conditions = [] } = payload;
    const now = Date.now();

    // Ensure device exists or auto-register device
    const checkDevice = db.prepare("SELECT id FROM devices WHERE id = ?").get(device_id) as any;
    if (!checkDevice) {
      db.prepare(`
        INSERT OR IGNORE INTO devices (id, name, key_hash, created_at, last_seen)
        VALUES (?, ?, 'auto-registered', ?, ?)
      `).run(device_id, `Device ${device_id}`, now, now);
    }

    // Ensure trip exists or create
    const checkTrip = db.prepare("SELECT id FROM trips WHERE id = ?").get(trip_id) as any;
    if (!checkTrip) {
      const insTrip = db.prepare(`
        INSERT INTO trips (id, device_id, started_at, distance_m, dr_distance_m, max_hpl_m, vehicle_mode)
        VALUES (?, ?, ?, 0.0, 0.0, 0.0, ?)
      `);
      insTrip.run(trip_id, device_id, states[0]?.t_ms || now, states[0]?.vehicle_mode || "CAR");
    }

    // Update device last_seen
    db.prepare("UPDATE devices SET last_seen = ? WHERE id = ?").run(now, device_id);

    // Ingest states
    if (states.length > 0) {
      const insState = db.prepare(`
        INSERT INTO states (trip_id, device_id, t_ms, lat, lon, speed_mps, heading_deg,
                            mode, hpl_m, ell_major_m, ell_minor_m, ell_angle_deg, way_id,
                            match_posterior, vehicle_mode, level)
        VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
      `);

      for (const st of states) {
        insState.run(
          trip_id,
          device_id,
          st.t_ms,
          st.lat,
          st.lon,
          st.speed_mps ?? 0.0,
          st.heading_deg ?? 0.0,
          st.mode,
          st.hpl_m,
          st.ell_major_m ?? 0.0,
          st.ell_minor_m ?? 0.0,
          st.ell_angle_deg ?? 0.0,
          st.way_id ?? 0,
          st.match_posterior ?? 0.0,
          st.vehicle_mode ?? "CAR",
          st.level ?? 0
        );

        // Update live service
        telemetryService.updateDeviceState({
          device_id,
          trip_id,
          t_ms: st.t_ms,
          lat: st.lat,
          lon: st.lon,
          speed_mps: st.speed_mps ?? 0.0,
          heading_deg: st.heading_deg ?? 0.0,
          mode: st.mode,
          hpl_m: st.hpl_m,
          ell_major_m: st.ell_major_m,
          ell_minor_m: st.ell_minor_m,
          ell_angle_deg: st.ell_angle_deg,
          way_id: st.way_id,
          match_posterior: st.match_posterior,
          vehicle_mode: st.vehicle_mode,
          level: st.level,
        });
      }
    }

    // Ingest events
    if (events.length > 0) {
      const insEvent = db.prepare(`
        INSERT INTO events (device_id, trip_id, t_ms, lat, lon, type, payload)
        VALUES (?, ?, ?, ?, ?, ?, ?)
      `);

      for (const ev of events) {
        const payloadStr = typeof ev.payload === "string" ? ev.payload : JSON.stringify(ev.payload || {});
        insEvent.run(device_id, trip_id, ev.t_ms, ev.lat ?? null, ev.lon ?? null, ev.type, payloadStr);
        telemetryService.broadcastEvent({
          device_id,
          trip_id,
          ...ev,
        });
      }
    }

    // Ingest road hazards
    if (road_conditions.length > 0) {
      const insHaz = db.prepare(`
        INSERT INTO road_conditions (device_id, t_ms, lat, lon, kind, severity)
        VALUES (?, ?, ?, ?, ?, ?)
      `);

      for (const rc of road_conditions) {
        insHaz.run(device_id, rc.t_ms, rc.lat, rc.lon, rc.kind, rc.severity);
        telemetryService.broadcastHazard({
          device_id,
          ...rc,
        });
      }
    }

    return reply.code(200).send({
      ingested_states: states.length,
      ingested_events: events.length,
      ingested_hazards: road_conditions.length,
    });
  });
};
