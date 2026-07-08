// Semver-ish version compare, factored out of the bridge so it has no host
// dependency and can be unit-tested under `node --test` (index.js pulls in
// bridge-core.js, which needs a Vite/import.meta.env context).
//
// Returns 1 if a > b, -1 if a < b, 0 if equal. Follows semver §11: a prerelease
// ranks LOWER than its release (1.0.0-beta < 1.0.0); among prereleases, numeric
// identifiers rank below alphanumeric, compared identifier by identifier.
export function compareVersions(a, b) {
  const split = (v) => {
    const [core, pre] = String(v ?? "").replace(/^v/, "").split("-", 2);
    return { core: core.split(".").map((p) => parseInt(p, 10) || 0), pre };
  };
  const va = split(a), vb = split(b);
  for (let i = 0; i < Math.max(va.core.length, vb.core.length); i++) {
    const d = (va.core[i] ?? 0) - (vb.core[i] ?? 0);
    if (d) return d > 0 ? 1 : -1;
  }
  if (!va.pre && !vb.pre) return 0;
  if (!va.pre) return 1;   // release outranks any prerelease
  if (!vb.pre) return -1;
  const ia = va.pre.split("."), ib = vb.pre.split(".");
  for (let i = 0; i < Math.max(ia.length, ib.length); i++) {
    if (ia[i] === undefined) return -1; // shorter prerelease is lower
    if (ib[i] === undefined) return 1;
    if (ia[i] === ib[i]) continue;
    const na = /^\d+$/.test(ia[i]), nb = /^\d+$/.test(ib[i]);
    if (na && nb) return Number(ia[i]) - Number(ib[i]) > 0 ? 1 : -1;
    if (na) return -1;      // numeric identifiers rank below alphanumeric
    if (nb) return 1;
    return ia[i] < ib[i] ? -1 : 1;
  }
  return 0;
}
