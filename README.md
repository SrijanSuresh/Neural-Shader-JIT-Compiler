# Neural Shader JIT Compiler

A real-time GPU shader evolution system powered by **Cerebras ultra-fast inference** (Gemma 4 31B). The AI *watches* what the GPU renders, critiques it, and rewrites the GLSL shader code live — full round-trip in ~4 seconds across four domains: dark fantasy, anime, biomedical, and cosmic.

```
Live Render → Base64 PNG → Cerebras Critic  → scores + hint
                                   ↓
                          Cerebras Composer ← evolutionary genealogy (131k ctx)
                                   ↓
                          New GLSL → hot-reload → GPU renders next generation
```

## Why Cerebras?

A 31B multimodal model on a standard GPU cluster takes **45–90 seconds** per call. Cerebras delivers the same model in **2–4 seconds** — making interactive JIT compilation possible.

| System | Latency (31B model) | Interactive? |
|---|---|---|
| GPU cluster | ~45,000 ms | No |
| Cerebras | ~3,500 ms | **Yes** |

---

## Prerequisites

- **Windows 10/11** with MinGW-w64 (`scoop install mingw cmake make`)
- **Python 3.10+**
- **Cerebras API key** — get access to `gemma-4-31b` at [cerebras.ai](https://cerebras.ai)
- **OpenGL 3.3+** GPU (any modern integrated or discrete GPU)

---

## Installation

### 1. Clone

```bash
git clone https://github.com/SrijanSuresh/Neural-Shader-JIT-Compiler
cd Neural-Shader-JIT-Compiler
```

### 2. Set your API key

```powershell
# Create .env (never committed)
"CEREBRAS_API_KEY=csk-your-key-here" | Out-File .env
```

### 3. Python backend

```powershell
python -m venv venv
.\venv\Scripts\Activate.ps1
pip install fastapi uvicorn openai
```

### 4. C++ renderer

```powershell
mkdir build; cd build
cmake -G "MinGW Makefiles" ..
make -j4
```

---

## Running

**Terminal 1 — backend:**
```powershell
$env:CEREBRAS_API_KEY = "csk-your-key-here"
.\venv\Scripts\python.exe -m uvicorn backend.main:app --port 8000
```

**Terminal 2 — renderer:**
```powershell
$env:CEREBRAS_API_KEY = "csk-your-key-here"
cd build
.\shader_jit_engine.exe
```

---

## Controls

| Key | Action |
|---|---|
| `1` | **Dark Fantasy** preset (Bloodborne / Elden Ring) |
| `2` | **Persona / Anime** preset (Persona 5, vibrant geometric) |
| `3` | **Biomedical** preset (fluorescence microscopy) |
| `4` | **Cosmic / Space** preset (nebula, star fields) |
| `SPACE` | Evolve current shader — triggers Critic then Composer |
| `B` | Baseline mode: shows fake 5s "GPU cluster" loading bar |
| `C` | **Split-screen compare**: left = before frame, right = live Cerebras result |
| `A` | **Auto-evolve**: fires SPACE every 20s automatically (great for demos) |
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
  "visual_complexity": 6.2,
  "atmosphere": 4.8,
  "technical_novelty": 5.1,
  "improvement_hint": "add a SDF rune glyph at center using polar coordinates"
}
```

**Phase 2 — Composer** (`POST /compile`):
Receives the current render + **full generational history** (up to 4 previous gens with scores and hints) packed into the 131k context window. Targets the weakest axis identified by the Critic.

Returns a complete GLSL 3.3 core fragment shader — no markdown, no explanations, just valid GPU code.

### Context window utilization

Each generation adds ~8k tokens of GLSL + scores to the evolutionary history. By generation 4, the Composer uses ~32k tokens of context — it knows every technique it's already tried and what the Critic scored poorly, so each generation is genuinely informed by the full lineage.

### Strict JSON schema

Both endpoints enforce `response_format: json_schema` with `strict: True`. Zero parsing ambiguity. The Composer cannot output markdown fences or explanations — only valid JSON containing valid GLSL.

---

## Split-screen comparison (`C` key)

After any SPACE evolution:
1. Press `C` to toggle comparison mode
2. **Left half**: the frame *before* evolution (frozen snapshot)
3. **Right half**: live current shader (animating)
4. Overlay shows: `BEFORE: ~45,000 ms` vs `AFTER: Xms (Yx faster)`

This is the demo money shot — the visual difference between generations with the speed numbers inline.

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
  "glsl_fragment": "#version 330 core\nin vec2 vUV;\n..."
}
```

---

## Quota usage

With Cerebras allocation (`gemma-4-31b`):

| Metric | Per evolution | Per day |
|---|---|---|
| Critic tokens | ~500 | — |
| Composer tokens | ~8,000 | — |
| Total per evolution | ~8,500 | — |
| Daily budget | — | 144M tokens |
| Max evolutions/day | — | **~16,900** |
| At 20s auto interval | — | 4,320 evolutions |

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
│  Python FastAPI  (uvicorn)                              │
│  POST /critique → gemma-4-31b strict JSON schema        │
│  POST /compile  → gemma-4-31b strict JSON schema        │
│                   Cerebras API  https://api.cerebras.ai │
└─────────────────────────────────────────────────────────┘
```

## License

MIT
