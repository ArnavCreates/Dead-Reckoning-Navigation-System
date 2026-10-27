// C++ UBODT (Upper-Bounded Origin-Destination Table) Generator
// Computes all-pairs shortest paths up to D_max (default 500 m) on road network graph
#include <iostream>
#include <vector>
#include <queue>
#include <unordered_map>
#include <string>
#include <cstdint>
#include <sqlite3.h>

struct Edge {
  int64_t id;
  int64_t u;
  int64_t v;
  double length_m;
};

struct PathNode {
  int64_t node;
  double dist;
  int64_t next_n;
  int64_t next_e;
  int64_t prev_n;

  bool operator>(const PathNode& other) const {
    return dist > other.dist;
  }
};

int main(int argc, char** argv) {
  if (argc < 2) {
    std::cerr << "Usage: ubodt_gen <path_to_map.sqlite> [max_distance_m=500.0]\n";
    return 1;
  }

  std::string db_path = argv[1];
  double max_dist = (argc >= 3) ? std::stod(argv[2]) : 500.0;

  sqlite3* db = nullptr;
  if (sqlite3_open(db_path.c_str(), &db) != SQLITE_OK) {
    std::cerr << "Failed to open sqlite database: " << db_path << "\n";
    return 1;
  }

  std::cout << "Loading graph from " << db_path << "...\n";
  std::unordered_map<int64_t, std::vector<Edge>> adj;
  std::vector<int64_t> all_nodes;

  sqlite3_stmt* stmt = nullptr;
  const char* q_edges = "SELECT id, u, v, length_m FROM edges";
  if (sqlite3_prepare_v2(db, q_edges, -1, &stmt, nullptr) != SQLITE_OK) {
    std::cerr << "Failed to query edges\n";
    sqlite3_close(db);
    return 1;
  }

  while (sqlite3_step(stmt) == SQLITE_ROW) {
    Edge e;
    e.id = sqlite3_column_int64(stmt, 0);
    e.u = sqlite3_column_int64(stmt, 1);
    e.v = sqlite3_column_int64(stmt, 2);
    e.length_m = sqlite3_column_double(stmt, 3);
    adj[e.u].push_back(e);
  }
  sqlite3_finalize(stmt);

  for (const auto& kv : adj) {
    all_nodes.push_back(kv.first);
  }
  std::cout << "Loaded " << all_nodes.size() << " source nodes. Computing UBODT with D_max=" << max_dist << "m...\n";

  const char* q_create = "CREATE TABLE IF NOT EXISTS ubodt ("
                         "source INTEGER NOT NULL, "
                         "target INTEGER NOT NULL, "
                         "next_n INTEGER NOT NULL, "
                         "prev_n INTEGER NOT NULL, "
                         "next_e INTEGER NOT NULL, "
                         "cost REAL NOT NULL, "
                         "PRIMARY KEY(source, target)"
                         ") WITHOUT ROWID;";
  char* err_msg = nullptr;
  if (sqlite3_exec(db, q_create, nullptr, nullptr, &err_msg) != SQLITE_OK) {
    std::cerr << "Failed to create ubodt table: " << (err_msg ? err_msg : "unknown error") << "\n";
    if (err_msg) sqlite3_free(err_msg);
    sqlite3_close(db);
    return 1;
  }

  sqlite3_exec(db, "BEGIN TRANSACTION;", nullptr, nullptr, nullptr);
  const char* q_ins = "INSERT OR REPLACE INTO ubodt (source, target, next_n, prev_n, next_e, cost) VALUES (?, ?, ?, ?, ?, ?)";
  if (sqlite3_prepare_v2(db, q_ins, -1, &stmt, nullptr) != SQLITE_OK) {
    std::cerr << "Failed to prepare ubodt insert statement: " << sqlite3_errmsg(db) << "\n";
    sqlite3_exec(db, "ROLLBACK;", nullptr, nullptr, nullptr);
    sqlite3_close(db);
    return 1;
  }

  int64_t total_paths = 0;
  for (int64_t src : all_nodes) {
    std::priority_queue<PathNode, std::vector<PathNode>, std::greater<PathNode>> pq;
    std::unordered_map<int64_t, double> min_dist;

    // Start with 1-hop outgoing edges
    for (const auto& e : adj[src]) {
      if (e.length_m <= max_dist) {
        PathNode pn;
        pn.node = e.v;
        pn.dist = e.length_m;
        pn.next_n = e.v;
        pn.next_e = e.id;
        pn.prev_n = src;
        pq.push(pn);
        min_dist[e.v] = e.length_m;
      }
    }

    while (!pq.empty()) {
      PathNode curr = pq.top();
      pq.pop();

      if (curr.dist > min_dist[curr.node]) continue;

      // Write to UBODT
      sqlite3_bind_int64(stmt, 1, src);
      sqlite3_bind_int64(stmt, 2, curr.node);
      sqlite3_bind_int64(stmt, 3, curr.next_n);
      sqlite3_bind_int64(stmt, 4, curr.prev_n);
      sqlite3_bind_int64(stmt, 5, curr.next_e);
      sqlite3_bind_double(stmt, 6, curr.dist);
      sqlite3_step(stmt);
      sqlite3_reset(stmt);
      total_paths++;

      // Explore neighbors
      auto it = adj.find(curr.node);
      if (it != adj.end()) {
        for (const auto& e : it->second) {
          double new_dist = curr.dist + e.length_m;
          if (new_dist <= max_dist) {
            if (min_dist.find(e.v) == min_dist.end() || new_dist < min_dist[e.v]) {
              min_dist[e.v] = new_dist;
              PathNode next_node;
              next_node.node = e.v;
              next_node.dist = new_dist;
              next_node.next_n = curr.next_n;
              next_node.next_e = curr.next_e;
              next_node.prev_n = curr.node;
              pq.push(next_node);
            }
          }
        }
      }
    }
  }

  sqlite3_finalize(stmt);
  sqlite3_exec(db, "COMMIT;", nullptr, nullptr, nullptr);
  sqlite3_close(db);

  std::cout << "Done! Populated " << total_paths << " paths in UBODT.\n";
  return 0;
}
