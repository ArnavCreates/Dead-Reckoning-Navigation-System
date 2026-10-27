import type { FastifyPluginAsync } from "fastify";
import fs from "node:fs";
import path from "node:path";
import { CONFIG } from "../config.js";

export const mapPackRoutes: FastifyPluginAsync = async (app) => {
  // List map packs
  app.get("/v1/map-packs", async () => {
    const packsDir = CONFIG.MAP_PACKS_DIR;
    if (!fs.existsSync(packsDir)) {
      return [];
    }

    const regions = fs.readdirSync(packsDir, { withFileTypes: true })
      .filter((d) => d.isDirectory() && d.name !== "scripts" && !d.name.startsWith("."))
      .map((d) => d.name);

    const result = [];
    for (const reg of regions) {
      const regDir = path.join(packsDir, reg);
      const files = fs.readdirSync(regDir)
        .filter((f) => !f.startsWith(".") && !f.endsWith(".py"))
        .map((f) => {
          const filePath = path.join(regDir, f);
          const stats = fs.statSync(filePath);
          return {
            filename: f,
            size_bytes: stats.size,
            updated_at: Math.floor(stats.mtimeMs),
            download_url: `/v1/map-packs/${reg}/${f}`,
          };
        });

      result.push({
        region: reg,
        files,
      });
    }

    return result;
  });

  // Serve binary files with HTTP range requests
  const serveRangeFile = async (filePath: string, request: any, reply: any) => {
    // Prevent directory traversal
    const resolvedPath = path.resolve(filePath);
    const allowedRoot = path.resolve(CONFIG.MAP_PACKS_DIR);
    if (!resolvedPath.startsWith(allowedRoot) || !fs.existsSync(resolvedPath)) {
      return reply.code(404).send({ error: "File not found" });
    }

    const stat = fs.statSync(resolvedPath);
    const fileSize = stat.size;
    const range = request.headers.range;

    if (range && range.startsWith("bytes=")) {
      const parts = range.replace(/bytes=/, "").split("-");
      let start: number;
      let end: number;

      if (parts[0] === "") {
        // Suffix range: bytes=-500
        const suffixLength = parseInt(parts[1], 10);
        if (isNaN(suffixLength) || suffixLength <= 0) {
          reply.header("Content-Range", `bytes */${fileSize}`);
          return reply.code(416).send();
        }
        start = Math.max(0, fileSize - suffixLength);
        end = fileSize - 1;
      } else {
        start = parseInt(parts[0], 10);
        end = parts[1] ? parseInt(parts[1], 10) : fileSize - 1;
      }

      if (isNaN(start) || isNaN(end) || start < 0 || start >= fileSize || end < start) {
        reply.header("Content-Range", `bytes */${fileSize}`);
        return reply.code(416).send();
      }

      end = Math.min(end, fileSize - 1);
      const chunksize = end - start + 1;
      const fileStream = fs.createReadStream(resolvedPath, { start, end });

      reply.code(206);
      reply.header("Content-Range", `bytes ${start}-${end}/${fileSize}`);
      reply.header("Accept-Ranges", "bytes");
      reply.header("Content-Length", chunksize);
      reply.header("Content-Type", "application/octet-stream");

      return reply.send(fileStream);
    } else {
      reply.header("Content-Length", fileSize);
      reply.header("Accept-Ranges", "bytes");
      reply.header("Content-Type", "application/octet-stream");
      return reply.send(fs.createReadStream(resolvedPath));
    }
  };

  app.get<{ Params: { region: string; file: string } }>("/v1/map-packs/:region/:file", async (request, reply) => {
    const { region, file } = request.params;
    // Sanitize path traversal for both region and filename
    const safeRegion = path.basename(region);
    const safeFile = path.basename(file);
    if (safeRegion === "scripts" || safeFile.endsWith(".py")) {
      return reply.code(404).send({ error: "File not found" });
    }
    const filePath = path.join(CONFIG.MAP_PACKS_DIR, safeRegion, safeFile);
    return serveRangeFile(filePath, request, reply);
  });

  app.get<{ Params: { region: string } }>("/maps/:region/basemap.pmtiles", async (request, reply) => {
    const { region } = request.params;
    const safeRegion = path.basename(region);
    const filePath = path.join(CONFIG.MAP_PACKS_DIR, safeRegion, "basemap.pmtiles");
    return serveRangeFile(filePath, request, reply);
  });
};
