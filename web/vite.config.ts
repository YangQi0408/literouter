import { fileURLToPath } from 'node:url'
import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import tailwindcss from '@tailwindcss/vite'

// The C++ side embeds four fixed paths — web/dist/{index.html,app.js,app.css,
// favicon.svg} — with #embed, so the output names must not carry a content
// hash: a hash would mean editing core/src/lr_proxy.cpp on every UI change.
// The console is served with `Cache-Control: no-store`, so there is nothing a
// hash would buy here.
export default defineConfig({
  base: '/ui/',
  plugins: [react(), tailwindcss()],
  resolve: {
    alias: { '@': fileURLToPath(new URL('./src', import.meta.url)) },
  },
  build: {
    outDir: 'dist',
    emptyOutDir: true,
    target: 'es2022',
    cssCodeSplit: false,
    modulePreload: false,
    sourcemap: false,
    rollupOptions: {
      output: {
        entryFileNames: 'app.js',
        chunkFileNames: 'app.js',
        assetFileNames: (info) => {
          const name = info.names?.[0] ?? info.name ?? ''
          return name.endsWith('.css') ? 'app.css' : '[name][extname]'
        },
      },
    },
  },
})
