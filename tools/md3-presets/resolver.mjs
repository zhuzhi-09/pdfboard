// Appends the missing .js extension for the MCU package's extensionless ESM
// relative imports (the package is built for bundlers, not bare Node ESM).
export async function resolve(specifier, context, next) {
  try {
    return await next(specifier, context);
  } catch (e) {
    const rel = specifier.startsWith('./') || specifier.startsWith('../');
    const hasExt = /\.[a-zA-Z0-9]+$/.test(specifier);
    if (rel && !hasExt) {
      for (const cand of [specifier + '.js', specifier + '/index.js']) {
        try { return await next(cand, context); } catch {}
      }
    }
    throw e;
  }
}
