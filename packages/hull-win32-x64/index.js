import { fileURLToPath } from "node:url";

// Absolute path to the prebuilt host executable and the directory holding it
// (the directory also contains the OpenSSL DLLs the host needs at runtime).
export const hostBinary = fileURLToPath(new URL("./bin/hull-host.exe", import.meta.url));
export const secureBinary = fileURLToPath(new URL("./bin/hull-host-secure.exe", import.meta.url));
export const hostDir = fileURLToPath(new URL("./bin/", import.meta.url));
