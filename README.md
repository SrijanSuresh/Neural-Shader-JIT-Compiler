# Multimodal Shader JIT Compiler

A 24-hour hackathon project. A C++ OpenGL renderer captures its own framebuffer, sends it to a Python FastAPI server, which calls the Cerebras Gemma 4 31B vision model to generate a new GLSL fragment shader on the fly, which the renderer hot-reloads — all triggered by a single keypress.

---

## Quick Start

### Prerequisites
- A Cerebras API key — get one at [cerebras.ai](https://cerebras.ai)
- **Docker path:** Docker Desktop installed and running
- **Manual path:** Python 3.10+

---

### Option A — Docker (recommended, works anywhere)

```bash
# 1. Clone and enter the repo
git clone <repo-url>
cd Multimodal-JIT-compiler

# 2. Create your .env file
cp .env.example .env
# Open .env and replace the placeholder with your real key

# 3. Start the server
docker compose up --build
```

The API is now live at `http://localhost:8000`.  
To stop it: `Ctrl+C`, then `docker compose down`.

---

### Option B — Plain Python (no Docker)

```bash
# 1. Clone and enter the repo
git clone <repo-url>
cd Multimodal-JIT-compiler

# 2. Create and activate a virtual environment
python -m venv venv

# Windows
venv\Scripts\activate
# macOS / Linux
source venv/bin/activate

# 3. Install dependencies
pip install -r requirements.txt

# 4. Set your API key (pick one)

# Windows PowerShell
$env:CEREBRAS_API_KEY = "your_key_here"

# Windows CMD
set CEREBRAS_API_KEY=your_key_here

# macOS / Linux
export CEREBRAS_API_KEY="your_key_here"

# 5. Start the server (run from the repo root)
uvicorn backend.main:app --reload --port 8000
```

---

## Verify It's Working

```bash
curl -X POST http://localhost:8000/compile \
  -H "Content-Type: application/json" \
  -d '{"base64_image":"dGVzdA==","user_prompt":"make it glow red"}'
```

| What you see | Meaning |
|---|---|
| `{"detail":"CEREBRAS_API_KEY not set"}` | API key missing — check step 4 above |
| `{"detail":"Error code: 401 ..."}` | Wrong key — double-check it |
| `{"glsl_fragment":"#version 330 core..."}` | Everything works |

---

## API Reference

### `POST /compile`

**Request body**
```json
{
  "base64_image": "<PNG frame encoded as Base64 string>",
  "user_prompt": "make the colors pulse faster"
}
```

**Response**
```json
{
  "glsl_fragment": "#version 330 core\n..."
}
```

**Error responses**

| Code | Reason |
|------|--------|
| 422 | Missing or malformed request body |
| 503 | `CEREBRAS_API_KEY` environment variable not set |
| 502 | Cerebras API error (bad key, quota, network) |

Interactive docs available at `http://localhost:8000/docs` while the server is running.

---

## Project Structure

```
Multimodal-JIT-compiler/
├── backend/
│   └── main.py          # FastAPI server — /compile endpoint
├── src/
│   └── main.cpp         # C++ OpenGL renderer (Phase 2)
├── shaders/
│   ├── vertex.glsl      # Passthrough vertex shader
│   └── fragment.glsl    # Default fragment shader (replaced at runtime)
├── vendor/              # Header-only C++ libs (stb, httplib) — no install needed
├── Dockerfile
├── docker-compose.yml
├── requirements.txt
└── .env.example         # Copy to .env and fill in your key
```

---

## Building the C++ Renderer (Phase 2)

Requires: CMake 3.15+, a C++17 compiler, GLFW, and GLEW installed.

```bash
mkdir -p build && cd build
cmake ..
make
./shader_jit_engine
```

Press **Spacebar** in the renderer window to trigger a JIT compile cycle.
