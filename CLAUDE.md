## Project Overview
This is a Multimodal Shader JIT Compiler for a 24-hour hackathon. The system consists of a C++ native OpenGL renderer and a Python FastAPI orchestration backend. The goal is to use Cerebras ultra-fast inference (Gemma 4 31B) to dynamically compile and hot-reload GLSL shaders based on multimodal visual feedback.

## Tech Stack & Architecture
- **Renderer (C++):** OpenGL 3.3+, GLFW, GLEW. Uses `stb_image_write` for frame capture and `cpp-httplib` for local HTTP communication.
- **Orchestrator (Python):** FastAPI, Uvicorn, standard `requests` or `openai` Python SDK.
- **AI Integration:** Cerebras Inference API using the `gemma-4-31b` model. Requires multimodal Base64 image inputs and strict JSON structured outputs for GLSL code.

## Coding Conventions
- **C++:** Modern C++17. Use RAII for OpenGL resource management. Separate shader loading logic from the main render loop to allow for hot-reloading without crashing the context.
- **Python:** Use `async/await` for FastAPI routes. Type hint all function signatures.
- **API Contracts:** The C++ client POSTs a Base64 string to `/compile`. The Python server returns a JSON object containing a `glsl_fragment` string.

## Git Workflow
- Create a git commit after completing each logical unit of work (e.g., finishing a specific Phase from the HACKATHON_PLAN.md or fixing a bug).
- Always run pre-commit checks and ensure the code compiles before committing.
- Use Conventional Commits format (e.g., `feat:`, `fix:`, `refactor:`, `docs:`).
- Include a brief explanation in the commit body for complex changes.
- Do NOT push to the remote repository unless explicitly asked.

## Commands
- Python Setup: `python -m venv venv && source venv/bin/activate && pip install fastapi uvicorn openai`
- Python Run: `uvicorn backend.main:app --reload --port 8000`
- C++ Build: `mkdir -p build && cd build && cmake .. && make`
- C++ Run: `./build/shader_jit_engine`

## Rules (Do Not Break These)
- Do NOT modify the C++ OpenGL context setup once it is working.
- Do NOT use standard chat loops; the Cerebras API calls must enforce `strict: true` JSON schema outputs for the shader code.
- Do NOT use hosted image URLs for the multimodal API; Gemma 4 on Cerebras currently only supports Base64 data URIs.