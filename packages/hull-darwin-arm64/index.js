import { fileURLToPath } from "node:url";

export const hostBinary = fileURLToPath(new URL("./bin/hull-host", import.meta.url));
export const secureBinary = fileURLToPath(new URL("./bin/hull-host-secure", import.meta.url));
export const hostDir = fileURLToPath(new URL("./bin/", import.meta.url));
