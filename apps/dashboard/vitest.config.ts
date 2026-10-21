import { defineConfig } from "vitest/config";

// Minimal unit tests for the server: the protocol schemas and how requests to the board are built.
export default defineConfig({
  test: { include: ["server/tests/**/*.test.ts"] },
});
