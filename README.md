# Multimodal Shader JIT Compiler

A real-time GPU shader evolution system powered by **Cerebras ultra-fast inference** (Gemma 4 31B). The AI *watches* what the GPU renders, critiques it, and rewrites the GLSL shader code live — full round-trip in 2–6 seconds across four domains: dark fantasy, anime, biomedical, and cosmic.

---

## Demo

<div align="center">

<video src="https://github.com/SrijanSuresh/Neural-Shader-JIT-Compiler/releases/download/demo/demo.mp4" controls width="800"></video>

</div>

---

## Preset Gallery

<div align="center">
<table>
<tr>
<td align="center"><b>Dark Fantasy</b> &nbsp;·&nbsp; <kbd>1</kbd><br><img src="https://github.com/SrijanSuresh/Neural-Shader-JIT-Compiler/releases/download/demo/darkfantasy.gif" width="420"/></td>
<td align="center"><b>Persona / Anime</b> &nbsp;·&nbsp; <kbd>2</kbd><br><img src="https://github.com/SrijanSuresh/Neural-Shader-JIT-Compiler/releases/download/demo/anime.gif" width="420"/></td>
</tr>
<tr>
<td align="center"><b>Biomedical</b> &nbsp;·&nbsp; <kbd>3</kbd><br><img src="https://github.com/SrijanSuresh/Neural-Shader-JIT-Compiler/releases/download/demo/biomed.gif" width="420"/></td>
<td align="center"><b>Cosmic / Space</b> &nbsp;·&nbsp; <kbd>4</kbd><br><img src="https://github.com/SrijanSuresh/Neural-Shader-JIT-Compiler/releases/download/demo/planet.gif" width="420"/></td>
</tr>
</table>
</div>

---

## Pipeline

```
Live Render → Base64 PNG → Cerebras Critic  → scores + scene_description
                                   ↓
                          Cerebras Composer ← evolutionary genealogy (131k ctx)
                                   ↓
                          New GLSL → hot-reload → GPU renders next generation
```

---

## Why Cerebras?

A 31B multimodal VLM on a shared GPU cluster (A100/H100, moderate queue load) takes **15–50 seconds** per request. Cerebras's wafer-scale architecture delivers the same model in **2–6 seconds** end-to-end. This gap is what makes or breaks an interactive JIT loop.

At >10s per call, a two-agent pipeline (Critic → Composer) takes 20–100 seconds per evolution — impractical for live demos or creative tools. At <6s, you can hot-reload shaders interactively, run auto-evolve every 20 seconds, and chain thousands of evolutions per day.

---

## Performance metrics

All Cerebras numbers are **measured live** — the HUD shows `completion_tokens`, round-trip ms, and tok/s after every evolution. GPU cluster numbers are **estimates** based on the formula shown on-screen in the baseline panel.

### Measured (Cerebras / Gemma 4 31B)

| Metric | Value | How |
|---|---|---|
| End-to-end round-trip | **2–6 s** | Wall-clock HTTP request→response, shown live on HUD |
| GLSL tokens generated | **300–900 tok** per evolution | `completion_tokens` from API response |
| Throughput (end-to-end) | **shown live on HUD** | `completion_tokens ÷ round_trip_s` |
| Time to first token (TTFT) | **< 500 ms** | Cerebras wafer-scale: entire 31B model on one chip, no memory bus bottleneck |
| JSON schema compliance | **100%** | `strict: True` schema enforcement — model cannot output malformed JSON |

> The HUD displays `Latency: XXXX ms  (N tok)` and `Y tok/s  |  Zx vs GPU est.` after every successful evolution, so evaluators can read actual Cerebras throughput directly from the running demo.

### Estimated (GPU cluster baseline)

The on-screen baseline panel derives the GPU cluster estimate **instantly** from the token count returned by Cerebras. No real benchmark is run — the math is:

$$t_{\text{GPU}} = \frac{N_{\text{tok}}}{25 \ \text{tok/s}} \times 1000 + 8000 \ \text{ms}$$

where:

| Symbol | Value | Rationale |
|---|---|---|
| $N_{\text{tok}}$ | `completion_tokens` from API response | Exact count returned by Cerebras after each evolution |
| $25 \ \text{tok/s}$ | Conservative shared-A100 throughput | A dedicated A100 in fp16 achieves ~50–80 tok/s; under shared cluster queue load with a 31B model, 25 tok/s is the realistic enterprise scenario |
| $8000 \ \text{ms}$ | TTFT + queue overhead | Time-to-first-token on A100 for 31B (~2–5 s) + API/scheduling overhead (~3–5 s) |

The speedup ratio displayed on the HUD is:

$$\text{speedup} = \frac{t_{\text{GPU}}}{t_{\text{Cerebras}}}$$

Because the formula uses the *actual* token count from each evolution, the estimate changes every generation — a 4,000-token generation yields ~111,720 ms vs a 500-token one at ~28,000 ms. Both are computed in microseconds; only $t_{\text{Cerebras}}$ is wall-clock measured.

After the first Cerebras evolution, the formula and token count are printed on-screen so the estimate is fully auditable.

### Speedup comparison (example)

For a 487-token GLSL shader at 3,812 ms Cerebras round-trip:

$$t_{\text{GPU}} = \frac{487}{25} \times 1000 + 8000 = 19{,}480 + 8{,}000 = 27{,}480 \ \text{ms}$$

$$\text{speedup} = \frac{27{,}480}{3{,}812} \approx 7.2\times$$

| | Cerebras (measured) | GPU cluster (est.) |
|---|---|---|
| Time | **3,812 ms** | ~27,480 ms |
| Throughput | **~116 tok/s end-to-end** | ~25 tok/s |
| Speedup | — | **7.2× faster** |

---

## Daily throughput limits

With the Cerebras hackathon allocation (`gemma-4-31b`):

| Metric | Per evolution | Per day |
|---|---|---|
| Critic tokens (`/critique`) | ~400–600 tok | — |
| Composer tokens (`/compile`, `reasoning_effort: high`) | ~500–2000 tok output + internal reasoning | — |
| Typical total per evolution | ~1,000–3,000 tok | — |
| Daily budget (hackathon allocation) | — | ~144 M tokens |
| Max evolutions / day (at avg 8,500 tok) | — | **~16,900** |
| At 20 s auto-evolve interval | — | 4,320 evolutions |

> **Note on `reasoning_effort: high`:** The Composer call uses Gemma 4's extended reasoning. `completion_tokens` in the response reflects output tokens only; internal chain-of-thought tokens may be billed separately or not at all depending on Cerebras's billing model. Actual per-evolution cost may be lower than the 8,500-token estimate.

---

## Error correction and reliability

| Scenario | Behavior |
|---|---|
| Malformed JSON from model | Impossible — `strict: True` schema rejects non-compliant outputs before they reach the application |
| GLSL compile error in generated shader | C++ catches `GL_COMPILE_STATUS` failure → previous working shader is retained → HUD shows "Error (retaining)" → render loop continues without any visual glitch |
| Cerebras API error (503, timeout) | FastAPI returns 502 → C++ logs error → renderer stays on current shader → next SPACE press triggers a fresh attempt |
| Backend not running | C++ prints connection error, stays on seed shader indefinitely |

The renderer never crashes on bad model output. The seed shader always runs as a fallback.

---

## Prerequisites

- **Windows 10/11** with MinGW-w64 (`scoop install mingw cmake make`)
- **Python 3.10+**
- **Docker Desktop** (recommended — handles backend environment automatically)
- **Cerebras API key** — get access to `gemma-4-31b` at [cerebras.ai](https://cerebras.ai)
- **OpenGL 3.3+** GPU (any modern integrated or discrete GPU)

---

## Installation

### 1. Clone

```bash
git clone https://github.com/SrijanSuresh/Multimodal-JIT-compiler
cd Multimodal-JIT-compiler
```

### 2. Set your API key

```bash
# Create .env (gitignored — never committed)
echo "CEREBRAS_API_KEY=csk-your-key-here" > .env
```

### 3. Start the backend (Docker — recommended)

```bash
docker compose up -d --build
docker compose logs orchestrator   # should print: CEREBRAS_API_KEY present (XX chars) ✓
```

### 4. C++ renderer

```powershell
mkdir build; cd build
cmake -G "MinGW Makefiles" ..
mingw32-make -j4
```

---

## Running

**Terminal 1 — backend (if not using Docker):**
```powershell
$env:CEREBRAS_API_KEY = "csk-your-key-here"
.\venv\Scripts\python.exe -m uvicorn backend.main:app --port 8000
```

**Terminal 2 — renderer:**
```powershell
cd build
.\shader_jit_engine.exe
```

> **Tip:** If using Docker, never run a second uvicorn on port 8000 in the same Windows session — it will shadow Docker's port binding and intercept all requests without the API key.

---

## Controls

| Key | Action |
|---|---|
| `1` | **Dark Fantasy** preset (Bloodborne / Elden Ring) |
| `2` | **Persona / Anime** preset (Persona 5, vibrant geometric) |
| `3` | **Biomedical** preset (fluorescence microscopy) |
| `4` | **Cosmic / Space** preset (nebula, star fields) |
| `SPACE` | Evolve current shader — triggers Critic then Composer |
| `B` | **Baseline comparison** — two-bar panel: Cerebras (snaps green when done) vs GPU cluster estimate (crawls over 8s). Press SPACE while panel is open to run a live Cerebras evolution concurrently. |
| `C` | **Split-screen compare**: left = frozen pre-evolution frame, right = live Cerebras result. Requires at least one SPACE press first. |
| `A` | **Auto-evolve**: fires SPACE every 20 s automatically (great for demos) |
| `ESC` | Quit |

---

## Presets

### `1` — Dark Fantasy *(Bloodborne / Elden Ring)*
Domain-warped 6-octave fbm fog with crimson → violet → amber palette. Blood moon glow, eldritch rune ring shimmer, vignette. Evolves toward denser mist, sharper rune SDF glyphs, and more complex warp layers.

**Critic axes:** eldritch dread atmosphere · crimson fog density · domain-warp complexity

### `2` — Persona / Anime *(Persona 5)*
Rotating SDF box frames, diamond tile patterns, concentric ring grids, radial all-out-attack glow. Electric yellow, hot pink, cyan palette with anime scan-line filter. Evolves toward richer geometric layering and stylized motion.

**Critic axes:** pop-art energy · geometric precision · animated stylization

### `3` — Biomedical *(Fluorescence Microscopy)*
Voronoi cell membrane network, amber organelles, pulsing violet nucleus, cytoplasm fluid dynamics. Fluorescent pulse glow effects mimicking confocal microscopy. Evolves toward more realistic organelle structures and membrane detail.

**Critic axes:** cellular realism · organic form complexity · scientific visual clarity

### `4` — Cosmic / Space *(Nebula / Deep Sky)*
7-octave domain-warped nebula clouds, two-density star field with individual twinkle, stellar nursery cluster, pulsar beam strobe. Deep indigo → electric blue → gold star cores. Evolves toward denser nebula formations and stellar phenomena.

**Critic axes:** nebula grandeur · stellar density · galactic scale

---

## How it works

### Two-agent pipeline

Each `SPACE` press runs **two sequential Cerebras calls** in a background thread (render loop never stalls):

**Phase 1 — Critic** (`POST /critique`):
```json
{
  "scene_description": "Deep crimson fog swirls around rotating eldritch runes in amber light",
  "visual_complexity": 6.2,
  "atmosphere": 4.8,
  "technical_novelty": 5.1,
  "improvement_hint": "add a SDF rune glyph at center using polar coordinates"
}
```

The `scene_description` field is proof that Gemma 4 is actually *seeing* the rendered frame — it appears as a live bottom bar on-screen so judges can verify the model's visual perception in real time.

**Phase 2 — Composer** (`POST /compile`, `reasoning_effort: high`):
Receives the current render + **full generational history** (up to 4 previous gens with scores and hints) packed into the 131k context window. Targets the weakest axis identified by the Critic.

Returns a complete GLSL 3.3 core fragment shader — no markdown, no explanations, just valid GPU code.

### Context window utilization

Each generation adds ~1–3k tokens of GLSL + scores to the evolutionary history. By generation 4, the Composer has seen every technique it's already tried and the Critic scores for each — so each generation is genuinely informed by the full lineage.

### Strict JSON schema

Both endpoints enforce `response_format: json_schema` with `strict: True`. Zero parsing ambiguity. The Composer cannot output markdown fences or explanations — only valid JSON containing valid GLSL.

### Hot-reload without context loss

When the Composer returns new GLSL, the background thread sets `hasNew = true` in a mutex-guarded struct. The GL main thread picks this up at the top of the next frame, calls `glDeleteProgram` on the old shader, compiles the new one, and swaps — all on the GL context thread. No context loss, no flicker.

---

## Split-screen comparison (`C` key)

After any SPACE evolution:
1. Press `C` to toggle comparison mode
2. **Left half**: the frame *before* evolution (frozen snapshot)
3. **Right half**: live current shader (animating)
4. Overlay shows: `~XXXX ms (est.)` GPU cluster vs `YYYY ms (Zx faster)` Cerebras actual

---

## Auto-evolve mode (`A` key)

Press `A` to start hands-free evolution at 20-second intervals. Switch presets with `1`-`4` at any time — auto-evolve continues on the new preset. Great for unattended conference demos or recording video.

---

## Adding a custom preset

1. Write `shaders/my_preset.glsl`:
   ```glsl
   #version 330 core
   in  vec2 vUV;
   out vec4 FragColor;
   uniform float uTime;
   void main() { /* default shader */ }
   ```

2. Copy to `build/shaders/my_preset.glsl`

3. Add to `kPresets[]` in `src/main.cpp`:
   ```cpp
   {
       "My Preset",
       "shaders/my_preset.glsl",
       "Score for [your aesthetic]: [what the critic looks for].",
       "[Style directive for Composer]: [palette, techniques, must-haves].",
       GLFW_KEY_5
   },
   ```

4. Increment `kNumPresets` and rebuild.

---

## API reference

### `POST /critique`

```json
{
  "base64_image": "<PNG base64>",
  "glsl_source": "#version 330 core\n...",
  "style_context": "Score for cosmic nebula aesthetic."
}
```

Response:
```json
{
  "scene_description": "Swirling indigo nebula clouds with gold star cores and a pulsing blue pulsar beam",
  "visual_complexity": 7.3,
  "atmosphere": 5.1,
  "technical_novelty": 6.8,
  "improvement_hint": "layer 8 octaves of fbm for denser volumetric fog"
}
```

### `POST /compile`

```json
{
  "base64_image": "<PNG base64>",
  "user_prompt": "EVOLUTIONARY GENEALOGY:\n  Gen1 VC:6.2 ATM:4.8...\nIMPROVEMENT DIRECTIVE: ..."
}
```

Response:
```json
{
  "glsl_fragment": "#version 330 core\nin vec2 vUV;\n...",
  "completion_tokens": 487
}
```

`completion_tokens` is used live: the renderer computes $t_{\text{GPU}} = N_{\text{tok}} / 25 \times 1000 + 8000$ ms as the GPU cluster estimate and updates the baseline panel and HUD speedup ratio after every evolution.

---

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│  C++ Renderer  (OpenGL 3.3 Core, GLFW, GLAD)            │
│                                                         │
│  Render loop (main thread)   JIT thread (detached)      │
│  ┌───────────────────────┐   ┌─────────────────────┐   │
│  │ Draw quad with uTime  │   │ glReadPixels → PNG   │   │
│  │ HUD overlay (stb_tt)  │   │ POST /critique       │   │
│  │ 1-4 preset switch     │──▶│ POST /compile        │   │
│  │ C split-screen blit   │   │ JitState.hasNew=true │   │
│  │ A auto-evolve timer   │   └─────────────────────┘   │
│  └───────────────────────┘                              │
│         ↑ hot-reload on GL main thread (no ctx loss)    │
└─────────┼───────────────────────────────────────────────┘
          │ HTTP localhost:8000
┌─────────▼───────────────────────────────────────────────┐
│  Python FastAPI  (uvicorn / Docker)                     │
│  POST /critique → gemma-4-31b  strict JSON schema        │
│  POST /compile  → gemma-4-31b  strict JSON schema        │
│                   Cerebras API  https://api.cerebras.ai │
└─────────────────────────────────────────────────────────┘
```

## License

MIT
