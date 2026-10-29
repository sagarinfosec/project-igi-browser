# Vercel deployment notes

This repository contains the browser UI and the source for the Project I.G.I. recompiler. The Vercel configuration at the repository root rewrites public routes to the `web/` directory and sends the COOP/COEP headers required for browser cross-origin isolation.

## Connect to Vercel

1. Import `developwithAhsan/project-igi-browser` in Vercel.
2. Keep the project root at the repository root (do not set `web/` as the Vercel root directory; `vercel.json` is at the repository root).
3. Use the static/no-build deployment defaults. The rewrite rules route `/` to `web/index.html`.
4. Deploy and check the home page, About, Controls, FAQ, and Privacy routes.

## Required game runtime files

The Git tree intentionally does not contain the generated game build. The launcher loads:

- `web/build/recomp.js`
- `web/build/recomp12.wasm`
- optionally, `web/game/manifest.json` and the matching hosted game payload if you configure hosted-game mode

These files are not in the current repository tree, so a static deployment can serve the website pages but cannot launch gameplay until the matching runtime is generated from an authorized local copy and added to the deployment. Do not upload or distribute the original game's proprietary files. The launcher supports selecting a game folder the player owns; hosted-game mode is optional.

## Verification checklist

- Confirm the response includes `Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy: require-corp`.
- Confirm `crossOriginIsolated` is true in a supported browser.
- Confirm `build/recomp.js` and `build/recomp12.wasm` return HTTP 200 before testing Play.
- Test the launcher and controls on a desktop browser with WebGPU, then test touch layout on a supported mobile browser.
