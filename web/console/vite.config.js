// Vite 配置：控制台是「静态托管的五页单页应用」，由 cue-serve 的 GET / 提供。
// 不引入任何外部 CDN：无字体、无图表库、无图标包，主题与图标全部手写/本地。
import { defineConfig } from 'vite';

export default defineConfig({
  // 相对 base：cue-serve 把 dist 挂在根路径，用 './' 让 assets 引用与部署位置解耦
  base: './',
  esbuild: {
    // 直接用 esbuild 的 automatic JSX 运行时，不需要 @vitejs/plugin-react（少装一个包）
    jsx: 'automatic',
  },
  build: {
    outDir: '../dist',
    emptyOutDir: true,
    target: 'es2020',
  },
  server: {
    port: 5173,
    proxy: {
      // 开发时把 API/SSE 代理到 cue-serve，前端代码不用区分环境
      '/api': 'http://127.0.0.1:8080',
      '/events': 'http://127.0.0.1:8080',
    },
  },
});
