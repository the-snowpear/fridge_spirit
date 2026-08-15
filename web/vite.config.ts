import { defineConfig, type Plugin } from "vite";
import react from "@vitejs/plugin-react";
import { createReadStream, cpSync, existsSync, statSync } from "node:fs";
import path from "node:path";
import { fileURLToPath } from "node:url";

// Vite 开发服务配置：管理面板需要通过 localhost 使用 Web Serial API。
// 额外托管 ui-reference/ver2 到 /ui-ver2，供「UI 预览」分区 iframe 使用。

const __dirname = path.dirname(fileURLToPath(import.meta.url));
const uiVer2Root = path.resolve(__dirname, "../ui-reference/ver2");

const MIME: Record<string, string> = {
  ".html": "text/html; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".mjs": "text/javascript; charset=utf-8",
  ".json": "application/json; charset=utf-8",
  ".png": "image/png",
  ".jpg": "image/jpeg",
  ".jpeg": "image/jpeg",
  ".svg": "image/svg+xml",
  ".webp": "image/webp",
  ".ico": "image/x-icon",
  ".md": "text/markdown; charset=utf-8",
  ".txt": "text/plain; charset=utf-8",
  ".woff": "font/woff",
  ".woff2": "font/woff2",
};

/** 开发态中间件 + 构建态拷贝：把 ver2 静态 UI 挂到 /ui-ver2 */
function uiVer2StaticPlugin(): Plugin {
  return {
    name: "ui-ver2-static",
    configureServer(server) {
      server.middlewares.use("/ui-ver2", (req, res, next) => {
        try {
          if (!existsSync(uiVer2Root)) {
            res.statusCode = 404;
            res.end("ui-reference/ver2 not found");
            return;
          }
          let rel = decodeURIComponent((req.url || "/").split("?")[0] || "/");
          if (rel === "/" || rel === "") {
            rel = "/index.html";
          }
          // 防止路径穿越
          const file = path.resolve(uiVer2Root, `.${rel}`);
          if (!file.startsWith(uiVer2Root)) {
            res.statusCode = 403;
            res.end("Forbidden");
            return;
          }
          let target = file;
          if (existsSync(target) && statSync(target).isDirectory()) {
            target = path.join(target, "index.html");
          }
          if (!existsSync(target) || !statSync(target).isFile()) {
            res.statusCode = 404;
            res.end("Not found");
            return;
          }
          const ext = path.extname(target).toLowerCase();
          res.setHeader("Content-Type", MIME[ext] || "application/octet-stream");
          res.setHeader("Cache-Control", "no-cache");
          createReadStream(target).pipe(res);
        } catch (error) {
          next(error);
        }
      });
    },
    closeBundle() {
      if (!existsSync(uiVer2Root)) {
        console.warn("[ui-ver2-static] skip copy: ui-reference/ver2 missing");
        return;
      }
      const outDir = path.resolve(__dirname, "dist/ui-ver2");
      cpSync(uiVer2Root, outDir, {
        recursive: true,
        // 预览不需要规划文档与截图，减小 dist 体积
        filter: (src) => {
          const normalized = src.replace(/\\/g, "/");
          if (normalized.includes("/doc")) return false;
          if (normalized.includes("/screenshot")) return false;
          return true;
        },
      });
      console.log("[ui-ver2-static] copied to dist/ui-ver2");
    },
  };
}

export default defineConfig({
  plugins: [react(), uiVer2StaticPlugin()],
  server: {
    port: 5173,
    strictPort: false,
    fs: {
      // 允许开发服读取仓库内 ui-reference
      allow: [path.resolve(__dirname, "..")],
    },
  },
});
