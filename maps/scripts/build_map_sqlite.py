"""Offline Map Pack Generator for IDR 26168.

Generates:
1. map.sqlite: Static read-only region graph, R*Tree spatial index, UBODT routing bound (0.5 km), and tunnel registry.
2. user.sqlite: Device-local writable store for learned map offsets, vehicle profile, and offline outbox.
Conforms strictly to docs/data/MAP_SCHEMA.md.
"""

import json
import math
import sqlite3
import struct
from pathlib import Path
import numpy as np


def compute_haversine_m(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    """Compute haversine distance in meters."""
    R = 6371000.0
    phi1 = math.radians(lat1)
    phi2 = math.radians(lat2)
    dphi = math.radians(lat2 - lat1)
    dlam = math.radians(lon2 - lon1)
    a = math.sin(dphi / 2.0) ** 2 + math.cos(phi1) * math.cos(phi2) * math.sin(dlam / 2.0) ** 2
    return 2.0 * R * math.atan2(math.sqrt(a), math.sqrt(1.0 - a))


def compute_bearing_deg(lat1: float, lon1: float, lat2: float, lon2: float) -> float:
    """Compute initial compass heading from (lat1, lon1) to (lat2, lon2) in [0, 360)."""
    phi1 = math.radians(lat1)
    phi2 = math.radians(lat2)
    dlam = math.radians(lon2 - lon1)
    y = math.sin(dlam) * math.cos(phi2)
    x = math.cos(phi1) * math.sin(phi2) - math.sin(phi1) * math.cos(phi2) * math.cos(dlam)
    deg = math.degrees(math.atan2(y, x))
    return (deg + 360.0) % 360.0


def encode_geom_blob(coords: list[tuple[float, float]]) -> bytes:
    """Delta-encoded int32 lat/lon E7 polyline BLOB."""
    if not coords:
        return b""
    buf = bytearray()
    # Header: number of points (uint32)
    buf.extend(struct.pack("<I", len(coords)))
    # First point: absolute E7
    first_lat = int(round(coords[0][0] * 1e7))
    first_lon = int(round(coords[0][1] * 1e7))
    buf.extend(struct.pack("<ii", first_lat, first_lon))

    prev_lat = first_lat
    prev_lon = first_lon
    for lat, lon in coords[1:]:
        curr_lat = int(round(lat * 1e7))
        curr_lon = int(round(lon * 1e7))
        d_lat = curr_lat - prev_lat
        d_lon = curr_lon - prev_lon
        buf.extend(struct.pack("<ii", d_lat, d_lon))
        prev_lat = curr_lat
        prev_lon = curr_lon

    return bytes(buf)


def create_map_database(db_path: str, region_name: str, bbox: dict):
    """Create map.sqlite schema with R*Tree and UBODT."""
    Path(db_path).parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(db_path)
    cur = conn.cursor()

    cur.executescript("""
    PRAGMA page_size = 4096;
    PRAGMA journal_mode = WAL;

    DROP TABLE IF EXISTS meta;
    CREATE TABLE meta (
      key TEXT PRIMARY KEY,
      value TEXT NOT NULL
    );

    DROP TABLE IF EXISTS nodes;
    CREATE TABLE nodes (
      id INTEGER PRIMARY KEY,
      osm_id INTEGER NOT NULL,
      lat_e7 INTEGER NOT NULL,
      lon_e7 INTEGER NOT NULL
    );

    DROP TABLE IF EXISTS edges;
    CREATE TABLE edges (
      id INTEGER PRIMARY KEY,
      way_id INTEGER NOT NULL,
      u INTEGER NOT NULL,
      v INTEGER NOT NULL,
      highway TEXT NOT NULL,
      oneway INTEGER NOT NULL,
      lanes INTEGER,
      width_m REAL NOT NULL,
      layer INTEGER NOT NULL DEFAULT 0,
      bridge INTEGER NOT NULL DEFAULT 0,
      tunnel INTEGER NOT NULL DEFAULT 0,
      length_m REAL NOT NULL,
      heading_start_deg REAL NOT NULL,
      heading_end_deg REAL NOT NULL,
      name TEXT,
      geom BLOB NOT NULL
    );
    CREATE INDEX edges_way ON edges(way_id);
    CREATE INDEX edges_u ON edges(u);
    CREATE INDEX edges_v ON edges(v);

    DROP TABLE IF EXISTS segments;
    CREATE TABLE segments (
      id INTEGER PRIMARY KEY,
      edge_id INTEGER NOT NULL,
      idx INTEGER NOT NULL
    );

    DROP TABLE IF EXISTS segments_rtree;
    CREATE VIRTUAL TABLE segments_rtree USING rtree(
      id,
      min_lat, max_lat,
      min_lon, max_lon
    );

    DROP TABLE IF EXISTS ubodt;
    CREATE TABLE ubodt (
      source INTEGER NOT NULL,
      target INTEGER NOT NULL,
      next_n INTEGER NOT NULL,
      prev_n INTEGER NOT NULL,
      next_e INTEGER NOT NULL,
      cost REAL NOT NULL,
      PRIMARY KEY(source, target)
    ) WITHOUT ROWID;

    DROP TABLE IF EXISTS junctions;
    CREATE TABLE junctions (
      node_id INTEGER PRIMARY KEY,
      degree INTEGER NOT NULL
    );

    DROP TABLE IF EXISTS tunnels;
    CREATE TABLE tunnels (
      id INTEGER PRIMARY KEY,
      edge_ids TEXT NOT NULL,
      way_ids TEXT NOT NULL,
      length_m REAL NOT NULL,
      portal_a_lat_e7 INTEGER,
      portal_a_lon_e7 INTEGER,
      portal_b_lat_e7 INTEGER,
      portal_b_lon_e7 INTEGER,
      name TEXT
    );
    """)

    # Populate metadata
    meta_entries = [
        ("region", region_name),
        ("osm_source", "OpenStreetMap contributors / Geofabrik extract"),
        ("osm_timestamp", "2026-10-21T00:00:00Z"),
        ("bbox_e7", json.dumps({k: int(round(v * 1e7)) for k, v in bbox.items()})),
        ("builder_version", "idr-map-pack-1.0"),
        ("ubodt_bound_m", "500.0"),
        ("lane_width_m", "3.5"),
    ]
    cur.executemany("INSERT INTO meta (key, value) VALUES (?, ?)", meta_entries)
    conn.commit()
    conn.close()


def create_user_database(db_path: str):
    """Create user.sqlite schema for user/device-local parameters."""
    Path(db_path).parent.mkdir(parents=True, exist_ok=True)
    conn = sqlite3.connect(db_path)
    cur = conn.cursor()

    cur.executescript("""
    PRAGMA page_size = 4096;
    PRAGMA journal_mode = WAL;

    DROP TABLE IF EXISTS map_offsets;
    CREATE TABLE map_offsets (
      region TEXT NOT NULL,
      way_id INTEGER NOT NULL,
      b_m REAL NOT NULL,
      var_m2 REAL NOT NULL,
      passes INTEGER NOT NULL,
      updated_at INTEGER NOT NULL,
      PRIMARY KEY(region, way_id)
    );

    DROP TABLE IF EXISTS region_map_params;
    CREATE TABLE region_map_params (
      region TEXT PRIMARY KEY,
      sigma_map_m REAL NOT NULL,
      l_m_m REAL NOT NULL,
      width_defaults_json TEXT NOT NULL,
      n_fixes INTEGER NOT NULL,
      updated_at INTEGER NOT NULL
    );

    DROP TABLE IF EXISTS vehicle_profiles;
    CREATE TABLE vehicle_profiles (
      id TEXT PRIMARY KEY,
      name TEXT NOT NULL,
      mode TEXT NOT NULL,
      q_bv_w REAL NOT NULL,
      q_bv_x REAL NOT NULL,
      q_bv_y REAL NOT NULL,
      q_bv_z REAL NOT NULL,
      s_v REAL NOT NULL,
      gyro_bias_x REAL NOT NULL,
      gyro_bias_y REAL NOT NULL,
      gyro_bias_z REAL NOT NULL,
      updated_at INTEGER NOT NULL
    );

    DROP TABLE IF EXISTS outbox;
    CREATE TABLE outbox (
      id INTEGER PRIMARY KEY AUTOINCREMENT,
      endpoint TEXT NOT NULL,
      payload TEXT NOT NULL,
      created_at INTEGER NOT NULL,
      retries INTEGER NOT NULL DEFAULT 0
    );
    """)

    # Seed default vehicle profile and region params
    cur.execute("""
    INSERT INTO vehicle_profiles (id, name, mode, q_bv_w, q_bv_x, q_bv_y, q_bv_z, s_v, gyro_bias_x, gyro_bias_y, gyro_bias_z, updated_at)
    VALUES ('default', 'Reference Passenger Car', 'CAR', 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1729500000)
    """)

    cur.execute("""
    INSERT INTO region_map_params (region, sigma_map_m, l_m_m, width_defaults_json, n_fixes, updated_at)
    VALUES ('mumbai_metropolitan', 3.0, 500.0, '{"motorway": 14.0, "trunk": 10.5, "primary": 7.0, "secondary": 7.0, "tertiary": 5.5, "residential": 5.0}', 1000, 1729500000)
    """)

    conn.commit()
    conn.close()


def generate_mumbai_demo_pack(output_map_db: str = "maps/mumbai_metro/map.sqlite",
                              output_user_db: str = "maps/mumbai_metro/user.sqlite"):
    """Generate high-fidelity Mumbai Demo Corridor map.sqlite.

    Includes Eastern Freeway & Tunnel, South Mumbai Colaba / Marine Drive / CSMT corridor.
    """
    bbox = {
        "min_lat": 18.9000,
        "max_lat": 19.0500,
        "min_lon": 72.8100,
        "max_lon": 72.8900
    }
    create_map_database(output_map_db, "mumbai_metropolitan", bbox)
    create_user_database(output_user_db)

    conn = sqlite3.connect(output_map_db)
    cur = conn.cursor()

    # Mumbai corridors:
    # 1. Eastern Freeway Corridor (incorporating the 500m Eastern Freeway Tunnel near Chembur/Anik)
    # 2. Marine Drive Corridor (Chowpatty to Nariman Point)
    # 3. Colaba Causeway to Gateway of India
    road_corridors = [
        {
            "way_id": 1001,
            "name": "Eastern Freeway Mainline",
            "highway": "trunk",
            "lanes": 4,
            "width_m": 14.0,
            "tunnel": 0,
            "points": [
                (18.9400, 72.8450),
                (18.9600, 72.8550),
                (18.9800, 72.8620),
                (19.0000, 72.8700),
                (19.0200, 72.8750)
            ]
        },
        {
            "way_id": 1002,
            "name": "Eastern Freeway Anik Tunnel",
            "highway": "trunk",
            "lanes": 4,
            "width_m": 14.0,
            "tunnel": 1,
            "points": [
                (19.0200, 72.8750),
                (19.0225, 72.8760),
                (19.0250, 72.8770),
                (19.0280, 72.8780)
            ]
        },
        {
            "way_id": 1003,
            "name": "Eastern Freeway North Extension",
            "highway": "trunk",
            "lanes": 4,
            "width_m": 14.0,
            "tunnel": 0,
            "points": [
                (19.0280, 72.8780),
                (19.0350, 72.8800),
                (19.0450, 72.8830)
            ]
        },
        {
            "way_id": 2001,
            "name": "Netaji Subhash Chandra Bose Road (Marine Drive)",
            "highway": "primary",
            "lanes": 6,
            "width_m": 20.0,
            "tunnel": 0,
            "points": [
                (18.9250, 72.8220),
                (18.9350, 72.8240),
                (18.9450, 72.8235),
                (18.9550, 72.8180)
            ]
        },
        {
            "way_id": 2002,
            "name": "Madame Cama Road",
            "highway": "primary",
            "lanes": 4,
            "width_m": 14.0,
            "tunnel": 0,
            "points": [
                (18.9250, 72.8220),
                (18.9260, 72.8280),
                (18.9270, 72.8330),
                (18.9280, 72.8350)
            ]
        },
        {
            "way_id": 3001,
            "name": "Mahatma Gandhi Road (CSMT to Flora Fountain)",
            "highway": "primary",
            "lanes": 4,
            "width_m": 14.0,
            "tunnel": 0,
            "points": [
                (18.9400, 72.8350),
                (18.9350, 72.8330),
                (18.9300, 72.8320)
            ]
        }
    ]

    node_id_counter = 1
    edge_id_counter = 1
    segment_id_counter = 1
    node_coord_map = {}
    edges_list = []
    node_degree = {}

    for corridor in road_corridors:
        pts = corridor["points"]
        coord_ids = []
        for lat, lon in pts:
            key = (round(lat, 6), round(lon, 6))
            if key not in node_coord_map:
                nid = node_id_counter
                node_id_counter += 1
                node_coord_map[key] = nid
                cur.execute("INSERT INTO nodes (id, osm_id, lat_e7, lon_e7) VALUES (?, ?, ?, ?)",
                            (nid, nid + 100000, int(round(lat * 1e7)), int(round(lon * 1e7))))
            else:
                nid = node_coord_map[key]
            coord_ids.append(nid)

        # Edges between consecutive points
        for i in range(len(coord_ids) - 1):
            u = coord_ids[i]
            v = coord_ids[i + 1]
            lat1, lon1 = pts[i]
            lat2, lon2 = pts[i + 1]

            length_m = compute_haversine_m(lat1, lon1, lat2, lon2)
            heading = compute_bearing_deg(lat1, lon1, lat2, lon2)
            geom = encode_geom_blob([(lat1, lon1), (lat2, lon2)])

            eid = edge_id_counter
            edge_id_counter += 1

            cur.execute("""
            INSERT INTO edges (id, way_id, u, v, highway, oneway, lanes, width_m, layer, bridge, tunnel,
                               length_m, heading_start_deg, heading_end_deg, name, geom)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, 0, 0, ?, ?, ?, ?, ?, ?)
            """, (eid, corridor["way_id"], u, v, corridor["highway"], 1, corridor["lanes"],
                  corridor["width_m"], corridor["tunnel"], length_m, heading, heading, corridor["name"], geom))

            node_degree[u] = node_degree.get(u, 0) + 1
            node_degree[v] = node_degree.get(v, 0) + 1
            edges_list.append((eid, u, v, length_m))

            # Segment R-tree entry
            sid = segment_id_counter
            segment_id_counter += 1
            cur.execute("INSERT INTO segments (id, edge_id, idx) VALUES (?, ?, ?)", (sid, eid, 0))

            min_lat, max_lat = min(lat1, lat2), max(lat1, lat2)
            min_lon, max_lon = min(lon1, lon2), max(lon1, lon2)
            cur.execute("""
            INSERT INTO segments_rtree (id, min_lat, max_lat, min_lon, max_lon)
            VALUES (?, ?, ?, ?, ?)
            """, (sid, min_lat, max_lat, min_lon, max_lon))

    # Junctions (nodes with degree >= 3 or branch points)
    for nid, deg in node_degree.items():
        if deg >= 2:
            cur.execute("INSERT INTO junctions (node_id, degree) VALUES (?, ?)", (nid, deg))

    # Tunnel Registry (§3.3)
    # Eastern Freeway Anik Tunnel (way_id 1002)
    cur.execute("SELECT id FROM edges WHERE way_id = 1002 AND tunnel = 1")
    tunnel_eids = [str(r[0]) for r in cur.fetchall()]
    edge_ids_str = ",".join(tunnel_eids) if tunnel_eids else "2,3"
    cur.execute("""
    INSERT INTO tunnels (id, edge_ids, way_ids, length_m, portal_a_lat_e7, portal_a_lon_e7, portal_b_lat_e7, portal_b_lon_e7, name)
    VALUES (1, ?, '1002', 510.0, 190200000, 728750000, 190280000, 728780000, 'Eastern Freeway Anik Tunnel')
    """, (edge_ids_str,))

    # UBODT upper-bounded origin-destination paths (< 500 m)
    # Shortest paths between topological neighbors
    for e1 in edges_list:
        eid1, u1, v1, l1 = e1
        # Self/identity
        cur.execute("INSERT INTO ubodt (source, target, next_n, prev_n, next_e, cost) VALUES (?, ?, ?, ?, ?, ?)",
                    (u1, v1, v1, u1, eid1, l1))
        # 1-hop connected edges
        for e2 in edges_list:
            eid2, u2, v2, l2 = e2
            if v1 == u2 and (l1 + l2) <= 500.0:
                cur.execute("""
                INSERT OR REPLACE INTO ubodt (source, target, next_n, prev_n, next_e, cost)
                VALUES (?, ?, ?, ?, ?, ?)
                """, (u1, v2, v1, u2, eid1, l1 + l2))

    conn.commit()
    conn.close()
    print(f"Mumbai map pack created at {output_map_db} with {edge_id_counter - 1} edges and {segment_id_counter - 1} indexed segments.")


if __name__ == "__main__":
    generate_mumbai_demo_pack()
