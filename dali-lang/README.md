# dali-lang (Foundation v0)

`dali-lang` is the first foundation of the `.dali`/`.dl` audio DSL for web/Electron audio processing.

Workflow note: changes limited to `dali-lang/**` should not trigger the main Linux app build workflow.

## npm package

You can use `dali-lang` independently from the main app via npm:

```bash
npm install dali-lang
```

CLI usage after install:

```bash
npx dali dali-lang/examples/web-bass-enhancer.dali /tmp/web-bass-enhancer.generated.js
npx dali dali-lang/examples/web-eq32-reference.dl /tmp/web-eq32-reference.generated.js --hardened
```

One-step editor setup (VS Code syntax extension):

```bash
npx dali setup
```

This installs `aurivo.dali-language` for VS Code and enables `.dali/.dl` highlighting.

Publish package:

```bash
cd dali-lang
npm login
npm publish --access public
```

Automated publish (GitHub Actions):

1. Add repository secret: `NPM_TOKEN` (npm automation token).
2. Bump `dali-lang/package.json` version.
3. Create tag in format `dali-lang-v<version>` and push:

```bash
git tag dali-lang-v0.1.4
git push origin dali-lang-v0.1.4
```

VS Code extension publish (GitHub Actions):

1. Add repository secret: `VSCE_PAT` (Visual Studio Marketplace personal access token).
2. Bump `dali-lang/editors/vscode/package.json` version.
3. Create tag in format `dali-vscode-v<version>` and push:

```bash
git tag dali-vscode-v0.1.5
git push origin dali-vscode-v0.1.5
```

Quality gates for portability + setup:

- `.github/workflows/dali-multi-arch-ci.yml`:
  - native smoke: `ubuntu-24.04`, `macos-14`, `windows-2025`
  - portable arch smoke: `linux/amd64` + `linux/arm64` (QEMU + Docker)
- `.github/workflows/dali-setup-smoke.yml`:
  - verifies `dali setup --skip-editor-install` across OS matrix
- `scripts/dali-security-suite.js`:
  - malicious corpus regression (unknown effect, unsafe capability, invalid unit, out-of-range latency, insecure task URL, hardened missing capability)

Recommended merge flow to `main`:

```bash
git checkout release_v2_0_12_clean
git pull --ff-only linux release_v2_0_12_clean
git checkout main
git pull --ff-only linux main
git merge --no-ff release_v2_0_12_clean -m "chore(dali): integrate security+ci hardening"
git push linux main
```

Capability policy file:
- `dali-lang/spec/capability-policy.json`
- Default model is deny-by-default capability enforcement.

Current scope:
- Parse `.dali` and `.dl` preset files.
- Compile one preset into a JavaScript module that builds a Web Audio graph.
- Support baseline effects: `preamp`, `low_shelf`, `peaking`, `high_shelf`, `compressor`, `limiter`.
- Security stage-1 active: strict whitelist validation for input/output targets, allowed effects, allowed effect params, and quality keys.
- Security stage-2 active: numeric range + unit validation for critical DSP params (gain/freq/q/attack/release/latency/sample_rate).
- Security stage-3 active: source size/line limits + complexity limits (preset/effect/param counts) for DoS resistance.
- Security stage-4 active: runtime sandbox guards in generated modules (AudioNode type checks, same-context isolation, safe connect/disconnect limits).
- Security stage-5 active: cryptographic signature infrastructure (ED25519 sign/verify + compile-time optional signature verification).
- Diagnostics improved: compiler/lint errors now include source-range code frames when line/column is available.

## Quick start

Compile sample preset:

```bash
node dali-lang/src/cli.js dali-lang/examples/web-bass-enhancer.dali dali-lang/examples/web-bass-enhancer.generated.js
```

Compile 32-band web EQ reference preset:

```bash
node dali-lang/src/cli.js dali-lang/examples/web-eq32-reference.dl dali-lang/examples/web-eq32-reference.generated.js
```

Compile all `.dali/.dl` presets in one command:

```bash
npm run -s dali:compile
```

This scans `dali-lang/examples` recursively and regenerates all `.generated.js` files.
Compiler now supports baseline `.dl v2` blocks (`engine`, `chain`, `quality`) and `eq32 band(...) = ...` syntax.
Strict mode is default in bulk compile. Use `--no-strict` only for temporary migration/debug.

Compile with AudioWorklet backend foundation:

```bash
npm run -s dali:compile:worklet
# or single file:
node dali-lang/src/cli.js dali-lang/examples/web-bass-enhancer.dl /tmp/web-bass-enhancer.worklet.generated.js --backend audioworklet
```

Compile with WASM skeleton target:

```bash
npm run -s dali:compile:wasm
# or single file:
node dali-lang/src/cli.js dali-lang/examples/web-bass-enhancer.dl /tmp/web-bass-enhancer.wasm.generated.js --target wasm
```

Worklet backend now runs baseline DSP chain inside the processor for:
- `preamp`, `low_shelf`, `peaking`, `high_shelf`, `compressor`, `limiter`

`audioworklet` backend output exposes an async builder (`await preset.buildGraph(...)`).

Lint preset files (validation + quality hints):

```bash
npm run -s dali:lint -- dali-lang/examples/web-bass-enhancer.dl dali-lang/examples/web-eq32-reference.dl
```

Task runtime dry-run (permissions + task planning, no external execution):

```bash
npm run -s dali:run -- dali-lang/examples/web-smart-task-reference.dl --dry-run
node dali-lang/src/cli.js run dali-lang/examples/web-smart-task-reference.dl --dry-run --json
```

Task runtime stub execution (sandboxed mock adapter, still no external system call):

```bash
node dali-lang/src/cli.js run dali-lang/examples/web-smart-task-reference.dl --no-dry-run --execute-stub
node dali-lang/src/cli.js run dali-lang/examples/web-smart-task-reference.dl --no-dry-run --execute-stub --json
```

Generate IR (with hash-based cache):

```bash
npm run -s dali:ir -- dali-lang/examples/web-smart-task-reference.dl
node dali-lang/src/cli.js ir dali-lang/examples/web-smart-task-reference.dl --json
```

Machine-readable JSON diagnostics:

```bash
node dali-lang/src/cli.js dali-lang/examples/web-bass-enhancer.dl /tmp/out.generated.js --json
node scripts/dali-lint.js --json dali-lang/examples/web-bass-enhancer.dl
node scripts/compile-dali-presets.js --json
```

LSP-compatible diagnostics payload:

```bash
npm run -s dali:lint:lsp -- dali-lang/examples/web-bass-enhancer.dl
```

VS Code tasks (Task: Run Task):

- `DALI: Lint Current File`
- `DALI: Lint Examples`
- `DALI: Compile All`
- `DALI: Compile Worklet`
- `DALI: Compile WASM`
- `DALI: Security Tests`
- `DALI: Run Dry-Run`
- `DALI: Run Stub Exec`
- `DALI: IR Current File`

These tasks are defined in `.vscode/tasks.json` and include problem matchers for DALI lint/compile errors.

Sign a preset file:

```bash
npm run -s dali:sign -- dali-lang/examples/web-bass-enhancer.dl --private-key dali-lang/keys/dali-ed25519.private.pem --key-id dali-main
```

Verify a preset signature:

```bash
npm run -s dali:verify-signature -- dali-lang/examples/web-bass-enhancer.dl --public-key dali-lang/keys/dali-ed25519.public.pem
```

Compile with signature verification enabled:

```bash
npm run -s dali:compile -- --verify-signatures --public-key dali-lang/keys/dali-ed25519.public.pem
```

Require every preset to be signed:

```bash
npm run -s dali:compile -- --require-signatures --public-key dali-lang/keys/dali-ed25519.public.pem
```

Run automated security tests (malformed corpus + fuzz + signature tamper):

```bash
npm run -s dali:test:security
```

Hardened compile mode:

```bash
node dali-lang/src/cli.js dali-lang/examples/web-eq32-reference.dl /tmp/out.generated.js --hardened
```

Strict mode (fail on any compile error):

```bash
node scripts/compile-dali-presets.js --strict
```

Install Linux file type + `D` icon for `.dali`/`.dl` files:

```bash
npm run -s dali:install:filetype:linux
```

Install Kate syntax highlighting for `.dali`/`.dl`:

```bash
npm run -s dali:install:kate:linux
```

Use VS Code syntax highlighting (local extension files):

```bash
# open this folder in VS Code and run Extension Development Host (F5)
dali-lang/editors/vscode
```

Full Linux runtime build (DALI + native addon + visualizer + runtime libs):

```bash
npm run -s build:runtime:linux
```

This single command executes:
- DALI lint + strict compile + security tests
- native addon rebuild (`dali_audio.node`)
- projectM visualizer CMake build and copy to `native-dist/linux`
- runtime library copy/check + Linux artifact verification
- syntax checks for `main.js`, `preload.js`, `renderer.js`

Load generated module in your web audio pipeline:

```js
const preset = require('./dali-lang/examples/web-bass-enhancer.generated.js');
const handle = await preset.buildGraphSafe(audioContext, sourceNode, audioContext.destination);
```

`buildGraphSafe` is always async-safe and works for both `webaudio` and `audioworklet` backends.

DaliNira web engine integration (current):
- Web 32-band effect state is initialized from `.dl` source file: `dali-lang/examples/web-eq32-reference.dl`.
- Renderer parses `preamp` + `peaking` bands from that file and applies them to WebAudio.
- Secure web mode whitelist: only `input web;`, `output speakers;`, and chain effects `preamp`, `peaking`, `limiter` are accepted.

`.dl v2` style (richer syntax) is also supported by web runtime:

```dl
preset "Web EQ32 Reference v2" {
  engine {
    input: web;
    output: speakers;
    safety: strict;
  }
  chain {
    preamp { gain: -1.5db; }
    eq32 {
      band(80hz) = 1.5db;
      band(125hz) = 2.0db;
    }
    limiter { ceiling: -0.8db; }
  }
}
```

## Why this architecture

`.dali` is designed as a DSL layer. Real-time processing still runs on Web Audio nodes (and later can target AudioWorklet/WASM backend).

Important: zero latency is not physically possible on real hardware. The goal is ultra-low and stable latency.

Roadmap file: [`dali-lang/ROADMAP.md`](./ROADMAP.md)

## Next milestones

1. Add validation + diagnostics with source ranges.
2. Add AudioWorklet backend target.
3. Add WASM DSP backend target.
4. Add adaptive loudness and bass-preserving protection blocks.
