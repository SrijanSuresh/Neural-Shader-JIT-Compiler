import json
import os

import openai
from fastapi import FastAPI, HTTPException
from fastapi.responses import RedirectResponse
from pydantic import BaseModel, Field

app = FastAPI(title="Shader JIT Orchestrator")

_MODEL = "gemma-4-31b"
_BASE_URL = "https://api.cerebras.ai/v1"

# ── Composer ──────────────────────────────────────────────────────────────────

_COMPOSE_SYSTEM = (
    "You are the Composer agent in an evolutionary GLSL shader pipeline. "
    "Given a rendered frame, critic scores, and the full generational history, "
    "generate the next-generation GLSL 3.3 core fragment shader. "
    "Requirements: 'in vec2 vUV;', 'out vec4 FragColor;', 'uniform float uTime;'. "
    "Return ONLY the complete GLSL 3.3 core shader source as 'glsl_fragment'. "
    "No markdown fences. No explanations. Just valid GLSL."
)

_COMPOSE_SCHEMA = {
    "type": "json_schema",
    "json_schema": {
        "name": "shader_output",
        "strict": True,
        "schema": {
            "type": "object",
            "properties": {"glsl_fragment": {"type": "string"}},
            "required": ["glsl_fragment"],
            "additionalProperties": False,
        },
    },
}

# ── Critic ────────────────────────────────────────────────────────────────────

_CRITIQUE_SYSTEM = (
    "You are a GPU shader art critic with multimodal vision. "
    "First, in 'scene_description', write ONE vivid sentence naming the specific colors, shapes, "
    "and motion you see in the rendered frame — be concrete, not generic. "
    "Then score the rendered frame on three axes (0.0–10.0):\n"
    "- visual_complexity: richness of detail, layering, and motion variety\n"
    "- atmosphere: how effectively it conveys the intended aesthetic and emotional feel\n"
    "- technical_novelty: sophistication of GLSL techniques (domain warping, SDF, PBR, etc)\n"
    "Then write ONE specific improvement_hint sentence targeting the lowest-scoring axis. "
    "Be concrete — name a specific technique, e.g. "
    "'add a SDF rune glyph using polar coordinates' or "
    "'layer 8 octaves of fbm for denser volumetric fog'."
)

_CRITIQUE_SCHEMA = {
    "type": "json_schema",
    "json_schema": {
        "name": "critique_output",
        "strict": True,
        "schema": {
            "type": "object",
            "properties": {
                "scene_description": {"type": "string"},
                "visual_complexity": {"type": "number"},
                "atmosphere":        {"type": "number"},
                "technical_novelty": {"type": "number"},
                "improvement_hint":  {"type": "string"},
            },
            "required": [
                "scene_description",
                "visual_complexity", "atmosphere",
                "technical_novelty", "improvement_hint",
            ],
            "additionalProperties": False,
        },
    },
}

# ── Shared ────────────────────────────────────────────────────────────────────

_EXAMPLE_PNG = (
    "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJ"
    "AAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg=="
)


def _make_client() -> openai.AsyncOpenAI:
    api_key = os.environ.get("CEREBRAS_API_KEY")
    if not api_key:
        raise HTTPException(status_code=503, detail="CEREBRAS_API_KEY not set")
    return openai.AsyncOpenAI(api_key=api_key, base_url=_BASE_URL)


@app.get("/", include_in_schema=False)
async def root() -> RedirectResponse:
    return RedirectResponse(url="/docs")


# ── /critique ─────────────────────────────────────────────────────────────────

class CritiqueRequest(BaseModel):
    base64_image:  str = Field(examples=[_EXAMPLE_PNG])
    glsl_source:   str = Field(examples=["void main() { FragColor = vec4(1.0); }"])
    style_context: str = Field(default="", examples=["Score for cosmic nebula aesthetic."])


class CritiqueResponse(BaseModel):
    scene_description: str
    visual_complexity: float
    atmosphere:        float
    technical_novelty: float
    improvement_hint:  str


@app.post("/critique", response_model=CritiqueResponse)
async def critique_shader(req: CritiqueRequest) -> CritiqueResponse:
    client = _make_client()

    style_note = f"\nStyle context: {req.style_context}" if req.style_context else ""

    try:
        response = await client.chat.completions.create(
            model=_MODEL,
            messages=[
                {"role": "system", "content": _CRITIQUE_SYSTEM},
                {
                    "role": "user",
                    "content": [
                        {
                            "type": "image_url",
                            "image_url": {"url": f"data:image/png;base64,{req.base64_image}"},
                        },
                        {
                            "type": "text",
                            "text": (
                                f"GLSL source:\n```glsl\n{req.glsl_source}\n```\n"
                                f"Score this render and give one concrete improvement hint.{style_note}"
                            ),
                        },
                    ],
                },
            ],
            response_format=_CRITIQUE_SCHEMA,  # type: ignore[arg-type]
            max_completion_tokens=512,
            temperature=0.3,
            top_p=1,
        )
    except Exception as exc:
        raise HTTPException(status_code=502, detail=str(exc)) from exc

    content = response.choices[0].message.content
    if not content:
        raise HTTPException(status_code=502, detail="Empty response from model")
    try:
        data = json.loads(content)
        return CritiqueResponse(**data)
    except (json.JSONDecodeError, KeyError) as exc:
        raise HTTPException(status_code=502, detail=f"Bad model output: {content}") from exc


# ── /compile ──────────────────────────────────────────────────────────────────

class CompileRequest(BaseModel):
    base64_image: str = Field(examples=[_EXAMPLE_PNG])
    user_prompt:  str = Field(examples=["evolve this shader with more complexity"])


class CompileResponse(BaseModel):
    glsl_fragment: str


@app.post("/compile", response_model=CompileResponse)
async def compile_shader(req: CompileRequest) -> CompileResponse:
    client = _make_client()
    try:
        response = await client.chat.completions.create(
            model=_MODEL,
            messages=[
                {"role": "system", "content": _COMPOSE_SYSTEM},
                {
                    "role": "user",
                    "content": [
                        {
                            "type": "image_url",
                            "image_url": {"url": f"data:image/png;base64,{req.base64_image}"},
                        },
                        {"type": "text", "text": req.user_prompt},
                    ],
                },
            ],
            response_format=_COMPOSE_SCHEMA,  # type: ignore[arg-type]
            max_completion_tokens=32768,
            temperature=0.2,
            top_p=1,
            extra_body={"reasoning_effort": "high"},
        )
    except Exception as exc:
        raise HTTPException(status_code=502, detail=str(exc)) from exc

    content = response.choices[0].message.content
    if not content:
        raise HTTPException(status_code=502, detail="Empty response from model")
    try:
        data = json.loads(content)
        return CompileResponse(glsl_fragment=data["glsl_fragment"])
    except (json.JSONDecodeError, KeyError) as exc:
        raise HTTPException(status_code=502, detail=f"Bad model output: {content}") from exc
