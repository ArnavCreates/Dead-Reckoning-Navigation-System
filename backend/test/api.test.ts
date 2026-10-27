import { strict as assert } from "node:assert";
import http from "node:http";

// Simple test against backend server
async function runTests() {
  console.log("Starting backend automated tests...");

  // Let's import fastify app directly for testing without needing a bound network socket
  const { healthRoutes } = await import("../src/routes/health.js");
  const { deviceRoutes } = await import("../src/routes/devices.js");
  const { ingestRoutes } = await import("../src/routes/ingest.js");
  const { liveRoutes } = await import("../src/routes/live.js");
  const { tripRoutes } = await import("../src/routes/trips.js");
  const { mapPackRoutes } = await import("../src/routes/mappacks.js");
  const fastify = (await import("fastify")).default;
  const websocket = (await import("@fastify/websocket")).default;

  const app = fastify();
  await app.register(websocket);
  await app.register(healthRoutes);
  await app.register(deviceRoutes);
  await app.register(ingestRoutes);
  await app.register(liveRoutes);
  await app.register(tripRoutes);
  await app.register(mapPackRoutes);

  // 1. Health test
  const resHealth = await app.inject({ method: "GET", url: "/v1/health" });
  assert.equal(resHealth.statusCode, 200);
  const healthJson = resHealth.json();
  assert.equal(healthJson.status, "ok");
  assert.equal(healthJson.db_connected, true);
  console.log("✓ Health endpoint OK");

  // 2. Devices test
  const resDevs = await app.inject({ method: "GET", url: "/v1/devices" });
  assert.equal(resDevs.statusCode, 200);
  const devsJson = resDevs.json();
  assert.ok(Array.isArray(devsJson));
  console.log(`✓ Devices endpoint OK (${devsJson.length} devices)`);

  // 3. Register a new device
  const resReg = await app.inject({
    method: "POST",
    url: "/v1/devices",
    payload: { name: "Test Telemetry Unit 99" },
  });
  assert.equal(resReg.statusCode, 201);
  const regJson = resReg.json();
  assert.ok(regJson.device_id);
  assert.ok(regJson.device_key);
  console.log(`✓ Device registration OK: ${regJson.device_id}`);

  // 4. Ingest batch telemetry
  const testTripId = `TRIP_TEST_${Date.now()}`;
  const resIngest = await app.inject({
    method: "POST",
    url: "/v1/ingest",
    payload: {
      device_id: regJson.device_id,
      trip_id: testTripId,
      states: [
        {
          t_ms: 1000,
          lat: 18.9400,
          lon: 72.8450,
          speed_mps: 12.5,
          heading_deg: 45.0,
          mode: "GNSS_OK",
          hpl_m: 4.2,
          vehicle_mode: "CAR",
        },
        {
          t_ms: 2000,
          lat: 18.9410,
          lon: 72.8460,
          speed_mps: 14.0,
          heading_deg: 45.0,
          mode: "DR",
          hpl_m: 6.8,
          vehicle_mode: "CAR",
        },
      ],
      events: [
        {
          t_ms: 1500,
          type: "TUNNEL_AHEAD",
          lat: 18.9405,
          lon: 72.8455,
          payload: { tunnel_id: 1, dist_m: 120.0 },
        },
      ],
    },
  });
  assert.equal(resIngest.statusCode, 200);
  const ingestJson = resIngest.json();
  assert.equal(ingestJson.ingested_states, 2);
  assert.equal(ingestJson.ingested_events, 1);
  console.log("✓ Ingestion OK (2 states, 1 event)");

  // 5. Live GeoJSON query
  const resLive = await app.inject({ method: "GET", url: "/v1/live" });
  assert.equal(resLive.statusCode, 200);
  const liveJson = resLive.json();
  assert.equal(liveJson.type, "FeatureCollection");
  const testDev = liveJson.features.find((f: any) => f.properties.device_id === regJson.device_id);
  assert.ok(testDev);
  assert.equal(testDev.properties.mode, "DR");
  assert.equal(testDev.properties.hpl_m, 6.8);
  console.log("✓ Live GeoJSON endpoint OK");

  // 6. Trip track query
  const resTrack = await app.inject({ method: "GET", url: `/v1/trips/${testTripId}/track` });
  assert.equal(resTrack.statusCode, 200);
  const trackJson = resTrack.json();
  assert.equal(trackJson.geometry.type, "LineString");
  assert.equal(trackJson.geometry.coordinates.length, 2);
  console.log("✓ Trip track LineString query OK");

  // 7. Map packs query
  const resPacks = await app.inject({ method: "GET", url: "/v1/map-packs" });
  assert.equal(resPacks.statusCode, 200);
  const packsJson = resPacks.json();
  assert.ok(Array.isArray(packsJson));
  console.log(`✓ Map packs query OK (${packsJson.length} regions found)`);

  console.log("\n ALL 7 BACKEND API TESTS PASSED SUCCESSFULLY!\n");
}

runTests().catch((err) => {
  console.error("Test failed:", err);
  process.exit(1);
});
